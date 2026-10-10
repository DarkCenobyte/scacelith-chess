#include "credential_store.h"
#include "crypto.h"
#include "json.h"
#include "net_sys.h"
#include "../core/log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <map>
#include <mutex>
#endif

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace net {

namespace {
constexpr size_t kMaxFileBytes = 256 * 1024;
const char kFileName[] = "Scacelith.credentials";

#ifdef _WIN32
const char kPrefix[] = "dpapi:";
#else
const char kPrefix[] = "bound:";
#endif
// A token the keyring keeps: the file holds this prefix and the id of its item (32 hex digits, a
// random value that tells nothing about the token).
const char kKeyringPrefix[] = "keyring:";
// A token kept in memory only (Linux, CredentialStore::held_): never written, the file's record has
// no token.
const char kMemoryPrefix[] = "memory:";

bool inKeyring(const std::string& blob) { return blob.compare(0, sizeof(kKeyringPrefix) - 1, kKeyringPrefix) == 0; }
std::string itemId(const std::string& blob) { return blob.substr(sizeof(kKeyringPrefix) - 1); }
bool inMemory(const std::string& blob) { return blob.compare(0, sizeof(kMemoryPrefix) - 1, kMemoryPrefix) == 0; }
// A token the file holds itself ("bound:" on Linux, "dpapi:" on Windows).
bool inFile(const std::string& blob) { return !blob.empty() && !inKeyring(blob) && !inMemory(blob); }

// setFileSessionsAllowed, and the stores it applies to at once.
std::atomic<bool> g_fileSessions{true};
std::mutex g_storesMu;
std::vector<CredentialStore*>& liveStores() {
    static std::vector<CredentialStore*> v;
    return v;
}

std::string newItemId() {
    uint8_t b[16];
    if (!crypto::randomBytes(b, sizeof(b))) return std::string();
    return crypto::hex(b, sizeof(b));
}

void wipe(std::string& s) {
    if (s.empty()) return;
    volatile char* p = &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}

// Linux: why the sessions stay in the file, said once per run (the first time a token is saved
// there, or read from there without moving to the keyring).
void noteFileFallback(const std::string& why, const std::string& path) {
#ifndef _WIN32
    static std::atomic<bool> said{false};
    if (said.exchange(true)) return;
    LOGW("net: the saved sessions are kept in %s in the clear, protected by the file's permissions (0600 in a 0700 "
         "folder), not in the system keyring: %s",
         path.c_str(), why.c_str());
#else
    (void)why;
    (void)path;
#endif
}

#ifndef _WIN32
std::string folderOf(const std::string& path) {
    const size_t cut = path.find_last_of('/');
    if (cut == std::string::npos) return ".";
    return cut == 0 ? std::string("/") : path.substr(0, cut);
}

// ssh's rule for a file of secrets: nothing for the group or the others (a folder 0700, a file
// 0600), repaired when it is more open (the log says so). False, with why, when it is more open
// and cannot be repaired (another owner, a read-only file system), or cannot be checked; the
// user's home folder and the root are never changed (a credentials file beside an --ini there),
// and count as such. A file not written yet is private.
bool makePrivate(const std::string& p, bool folder, std::string& why) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) {
        if (!folder && errno == ENOENT) return true;
        why = p + ": " + std::strerror(errno);
        return false;
    }
    if ((st.st_mode & 077) == 0) return true;
    const unsigned mode = unsigned(st.st_mode & 0777), want = folder ? 0700u : 0600u;
    bool spared = folder && p == "/";
    const char* home = std::getenv("HOME");
    struct stat hs;
    if (folder && home && home[0] && stat(home, &hs) == 0 && hs.st_dev == st.st_dev && hs.st_ino == st.st_ino) spared = true;
    char m[16];
    std::snprintf(m, sizeof m, "%03o", mode);
    if (spared || st.st_uid != geteuid() || chmod(p.c_str(), mode_t(want)) != 0) {
        why = p + " is open to other users (mode " + m + ")" +
              (spared ? ": the game leaves the permissions of that folder alone" : " and the game cannot change that");
        return false;
    }
    LOGW("net: %s was open to other users (mode %s): set to %03o", p.c_str(), m, want);
    return true;
}
#endif

// The keyring could not say what it keeps (or did not remove an item).
bool unanswered(Keyring::Result r) {
    return r == Keyring::Result::Locked || r == Keyring::Result::Unavailable || r == Keyring::Result::Cancelled;
}
bool notRemoved(Keyring::Result r) { return r == Keyring::Result::Locked || r == Keyring::Result::Unavailable; }

// The token of an unlock prompt: it fires after ms, or with parent (the store's interrupt()).
class Deadline {
public:
    Deadline(CancelToken& parent, int ms)
        : link_(&parent, [this] { token_.cancel(); }), timer_([this, ms] {
              std::unique_lock<std::mutex> lk(mu_);
              if (!cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return done_; })) token_.cancel();
          }) {}
    ~Deadline() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            done_ = true;
        }
        cv_.notify_all();
        timer_.join();
        link_.clear();
    }
    Deadline(const Deadline&) = delete;
    Deadline& operator=(const Deadline&) = delete;
    CancelToken* token() { return &token_; }

private:
    CancelToken token_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool done_ = false;
    AbortGuard link_;
    std::thread timer_;
};

bool keyringOff() {
    const char* env = std::getenv("SCACELITH_KEYRING");
    return env && std::strcmp(env, "off") == 0;
}

// Why there is no keyring (the store has none: keyring()).
#ifdef __APPLE__
std::string noKeyring() { return keyringOff() ? "disabled by SCACELITH_KEYRING=off" : "no keychain"; }
#else
std::string noKeyring() { return keyringOff() ? "disabled by SCACELITH_KEYRING=off" : "libsecret-1.so.0 cannot be loaded"; }
#endif

#ifdef __APPLE__
// SCACELITH_KEYRING=memory: a keyring in this process's memory instead of the user's keychain, for
// the unit tests (tests/test_main.cpp). A token never goes to the credentials file there: the
// stores and clients of the tests find the sessions that others kept through it instead, until the
// process ends. Never locked, never unavailable.
bool keyringInMemory() {
    const char* env = std::getenv("SCACELITH_KEYRING");
    return env && std::strcmp(env, "memory") == 0;
}

class MemoryKeyring final : public Keyring {
public:
    Result store(const std::string& origin, const std::string& id, const std::string& secret, CancelToken* cancel,
                 std::string&) override {
        if (cancel && cancel->cancelled()) return Result::Cancelled;
        std::lock_guard<std::mutex> lk(mu_);
        items_[origin + '\n' + id] = secret;
        return Result::Ok;
    }
    Result lookup(const std::string& origin, const std::string& id, std::string& secret, CancelToken* cancel,
                  std::string&) override {
        if (cancel && cancel->cancelled()) return Result::Cancelled;
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = items_.find(origin + '\n' + id);
        if (it == items_.end()) return Result::Missing;
        secret = it->second;
        return Result::Ok;
    }
    Result remove(const std::string& origin, const std::string& id, CancelToken* cancel, std::string&) override {
        if (cancel && cancel->cancelled()) return Result::Cancelled;
        std::lock_guard<std::mutex> lk(mu_);
        items_.erase(origin + '\n' + id);
        return Result::Ok;
    }
    Result unlock(const std::string&, const std::string&, CancelToken* cancel, std::string&) override {
        return cancel && cancel->cancelled() ? Result::Cancelled : Result::Ok;
    }

private:
    std::mutex mu_;
    std::map<std::string, std::string> items_;   // "<origin>\n<id>": the secret
};
#endif
}  // namespace

#ifdef __APPLE__
Keyring* defaultKeyring() {
    if (keyringInMemory()) {
        static Keyring* const memory = new MemoryKeyring();   // never destroyed, as the keychain's
        return memory;
    }
    return keyringOff() ? nullptr : keychainKeyring();
}
#else
Keyring* defaultKeyring() { return keyringOff() ? nullptr : secretServiceKeyring(); }
#endif

void setFileSessionsAllowed(bool allowed) {
    if (g_fileSessions.exchange(allowed) == allowed) return;
    std::lock_guard<std::mutex> stores(g_storesMu);
    for (CredentialStore* s : liveStores()) {
        std::lock_guard<std::mutex> lk(s->mu_);
        if (s->loaded_) s->reconcileLocked();
    }
}

bool fileSessionsAllowed() { return g_fileSessions.load(); }

// ---- token protection ----

std::string protectToken(const std::string& origin, const std::string& token) {
    if (token.empty() || origin.empty()) return std::string();
#ifdef _WIN32
    DATA_BLOB in{DWORD(token.size()), (BYTE*)token.data()};
    DATA_BLOB entropy{DWORD(origin.size()), (BYTE*)origin.data()};
    DATA_BLOB out{0, nullptr};
    if (!CryptProtectData(&in, L"Scacelith session", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        LOGW("net: CryptProtectData failed (%lu)", GetLastError());
        return std::string();
    }
    std::string blob = kPrefix + crypto::base64(out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return blob;
#else
    // The file format of a Linux build without a usable keyring (the store tries the keyring
    // first): the token is bound to its origin, in the clear. Only the file's permissions protect it.
    std::string plain = origin + '\n' + token;
    std::string blob = kPrefix + crypto::base64url(plain.data(), plain.size());
    wipe(plain);
    return blob;
#endif
}

bool unprotectToken(const std::string& origin, const std::string& blob, std::string& token) {
    token.clear();
    size_t pl = std::strlen(kPrefix);
    if (origin.empty() || blob.compare(0, pl, kPrefix) != 0) return false;
    std::vector<uint8_t> bytes;
#ifdef _WIN32
    if (!crypto::base64Decode(blob.substr(pl), bytes) || bytes.empty()) return false;
    DATA_BLOB in{DWORD(bytes.size()), bytes.data()};
    DATA_BLOB entropy{DWORD(origin.size()), (BYTE*)origin.data()};
    DATA_BLOB out{0, nullptr};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    token.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return !token.empty();
#else
    if (!crypto::base64urlDecode(blob.substr(pl), bytes)) return false;
    std::string plain(bytes.begin(), bytes.end());
    std::fill(bytes.begin(), bytes.end(), 0);
    bool ok = plain.size() > origin.size() + 1 && plain.compare(0, origin.size(), origin) == 0 && plain[origin.size()] == '\n';
    if (ok) token = plain.substr(origin.size() + 1);
    wipe(plain);
    return ok && !token.empty();
#endif
}

// ---- store ----

std::string CredentialStore::defaultPath() {
    return sys::settingsDirectory() + kFileName;   // with Scacelith.ini
}

CredentialStore::CredentialStore(std::string path) : path_(std::move(path)) {
    std::lock_guard<std::mutex> stores(g_storesMu);
    liveStores().push_back(this);
}

CredentialStore::~CredentialStore() {
    {
        std::lock_guard<std::mutex> stores(g_storesMu);
        auto& v = liveStores();
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }
    for (auto& h : held_) wipe(h.second);
}

void CredentialStore::setPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    loaded_ = false;
    records_.clear();
    for (auto& h : held_) wipe(h.second);
    held_.clear();
}

std::string CredentialStore::path() const {
    std::lock_guard<std::mutex> lk(mu_);
    return path_;
}

void CredentialStore::loadLocked() const {
    if (loaded_) return;
    loaded_ = true;
    records_.clear();
    if (path_.empty()) path_ = defaultPath();
    std::string text;
    if (!sys::readFile(path_, text, kMaxFileBytes)) return;
    json::Value doc;
    std::string err;
    if (!json::parse(text, doc, &err)) {
        LOGW("net: ignoring unreadable credentials file (%s)", err.c_str());
        return;
    }
    for (const json::Value& r : doc["records"].items()) {
        Record rec;
        rec.origin = r["origin"].asString();
        rec.username = r["username"].asString();
        rec.serverId = r["serverId"].asString();
        rec.pin = r["pin"].asString();
        rec.tokenBlob = r["token"].asString();
        if (inMemory(rec.tokenBlob)) rec.tokenBlob.clear();          // never written by the game
        if (rec.origin.empty() || findLocked(rec.origin)) continue;   // one record per origin
        records_.push_back(std::move(rec));
    }
    applyMovesLocked();
    reconcileLocked();
}

void CredentialStore::applyMovesLocked() const {
    bool moved = false;
    for (const auto& mv : moves_) {
        if (mv.first == mv.second || findLocked(mv.second)) continue;
        Record* r = findLocked(mv.first);
        if (!r) continue;
        // The token is bound to its origin (DPAPI entropy, or the clear binding): unwrap it for the
        // old origin, wrap it again for the new one (in the file: the keyring takes it on its first
        // read). One that cannot be read here moves without it. One the keyring keeps moves as it
        // is (no keyring call while the file loads, on any thread): its item names the old origin,
        // where read() finds it through this rule, and get() moves it to an item of the new one.
        std::string token, blob;
        if (inKeyring(r->tokenBlob) || inMemory(r->tokenBlob))
            blob = r->tokenBlob;
        else if (!r->tokenBlob.empty() && unprotectToken(mv.first, r->tokenBlob, token))
#ifdef __APPLE__
            blob = holdLocked(token);   // never written to the file there (kFileSessions)
#else
            blob = protectToken(mv.second, token);
#endif
        wipe(token);
        r->origin = mv.second;
        r->tokenBlob = blob;
        moved = true;
        LOGI("net: the saved session of %s moved to %s", mv.first.c_str(), mv.second.c_str());
    }
    if (moved) saveLocked();
}

void CredentialStore::addOriginMove(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) return;
    std::lock_guard<std::mutex> lk(mu_);
    moves_.emplace_back(from, to);
    if (loaded_) applyMovesLocked();
}

bool CredentialStore::saveLocked() const {
    // The permissions first (repaired when more open): a file open to other users for good keeps no
    // token in the clear (those it held are not used this run: readLocked).
    const bool closed = privateLocked();
    json::Value doc = json::Value::object();
    doc.set("version", 1);
    json::Value& list = doc.set("records", json::Value::array());
    for (Record& r : records_) {
        if (!closed && inFile(r.tokenBlob)) {
            LOGW("net: the saved session of %s is erased from %s, which other users can read", r.origin.c_str(), path_.c_str());
            r.tokenBlob.clear();
            r.unreadable = false;
            r.checked = true;
        }
        json::Value o = json::Value::object();
        o.set("origin", r.origin);
        o.set("username", r.username);
        o.set("serverId", r.serverId);
        o.set("pin", r.pin);
        o.set("token", inMemory(r.tokenBlob) ? std::string() : r.tokenBlob);
        list.push(std::move(o));
    }
    std::string text = doc.dump();
    text += '\n';
    if (!sys::writeFileAtomic(path_, text, true)) {
        LOGW("net: could not write %s", path_.c_str());
        return false;
    }
    return true;
}

CredentialStore::Record* CredentialStore::findLocked(const std::string& origin) const {
    for (Record& r : records_)
        if (r.origin == origin) return &r;
    return nullptr;
}

bool CredentialStore::get(const std::string& origin, Credential& out, bool unlock) const {
    std::lock_guard<std::mutex> io(ioMu_);
    std::string blob;
    {
        std::lock_guard<std::mutex> lk(mu_);
        loadLocked();
        Record* r = findLocked(origin);
        if (!r) return false;
        out = Credential();
        out.origin = r->origin;
        out.username = r->username;
        out.serverId = r->serverId;
        out.pinnedSha256 = r->pin;
        if (r->tokenBlob.empty()) return true;
        if (!inKeyring(r->tokenBlob)) {
            // In the file or in memory: read here. The keyring takes it once that works.
            if (readLocked(*r, out.token)) blob = r->tokenBlob;
        } else {
            blob = r->tokenBlob;
        }
    }
    if (blob.empty()) return true;
    if (!inKeyring(blob)) {
        migrate(origin, blob, out.token);
        return true;
    }
    std::string why, itemOrigin;
    Keyring::Result status = Keyring::Result::Ok;
    const bool readable = read(origin, blob, unlock ? Prompt::Once : Prompt::Never, out.token, why, &itemOrigin, &status);
    // Locked, and not asked: a read that needs the token may still ask, so it is still a session.
    const bool unreadable = !readable && !(status == Keyring::Result::Locked && !unlockDeclined_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        Record* r = findLocked(origin);
        if (r && r->tokenBlob == blob) {
            // Said once; hasToken() follows the last read (an unlocked keyring makes it readable again).
            if (unreadable && !r->unreadable) LOGW("net: the saved session of %s is not readable from the system keyring (%s)", origin.c_str(), why.c_str());
            r->unreadable = unreadable;
            r->checked = true;
        }
    }
    if (readable && itemOrigin != origin) migrate(origin, blob, out.token, itemOrigin);   // a moved record
    return true;
}

bool CredentialStore::hasToken(const std::string& origin) const {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    Record* r = findLocked(origin);
    if (!r || r->tokenBlob.empty()) return false;
    // A token the keyring keeps counts until a read fails (get(), on a network thread): the game
    // thread asks this, and never waits for the keyring.
    if (inKeyring(r->tokenBlob)) return !r->unreadable;
    if (!r->checked) {
        std::string token;
        readLocked(*r, token);
        wipe(token);
    }
    return !r->unreadable;
}

bool CredentialStore::readLocked(Record& r, std::string& token) const {
    // Said once; from then on hasToken() is false (the game offers to sign in, not to resume).
    bool readable = false;
    token.clear();
    if (inMemory(r.tokenBlob)) {
        auto it = held_.find(r.tokenBlob);
        readable = it != held_.end() && !it->second.empty();
        if (readable) token = it->second;
    } else if (!privateLocked()) {
        // Open to other users for good: a token in the clear there is not used.
        if (!r.unreadable) LOGW("net: the saved session of %s in %s is not used: other users can read it", r.origin.c_str(), path_.c_str());
    } else {
        readable = unprotectToken(r.origin, r.tokenBlob, token);
        if (!readable && !r.unreadable) LOGW("net: the saved session of %s cannot be decrypted here", r.origin.c_str());
    }
    r.unreadable = !readable;
    r.checked = true;
    return readable;
}

bool CredentialStore::privateLocked() const {
#ifdef _WIN32
    return true;
#else
    if (path_.empty()) path_ = defaultPath();
    std::string why;
    const bool closed = makePrivate(folderOf(path_), true, why) && makePrivate(path_, false, why);
    if (!closed && !openWarned_) LOGW("net: no session is kept in the clear in %s: %s", path_.c_str(), why.c_str());
    openWarned_ = !closed;
    return closed;
#endif
}

std::string CredentialStore::holdLocked(const std::string& token) const {
    const std::string blob = kMemoryPrefix + std::to_string(++heldSeq_);
    held_[blob] = token;
    return blob;
}

void CredentialStore::dropLocked(const std::string& blob) const {
    auto it = held_.find(blob);
    if (it == held_.end()) return;
    wipe(it->second);
    held_.erase(it);
}

void CredentialStore::reconcileLocked() const {
#ifndef _WIN32
    // macOS: never in the file (kFileSessions), whatever the option says.
    const bool allowed = kFileSessions && fileSessionsAllowed();
    bool changed = false, closed = true, checked = false;
    for (Record& r : records_) {
        const bool file = inFile(r.tokenBlob), memory = inMemory(r.tokenBlob);
        if (allowed ? !memory : !file) continue;
        if (!checked) {
            closed = privateLocked();
            checked = true;
        }
        if (file) {
            // The player does not allow the file: the token moves to memory (unread when the file is
            // open to other users), and is erased from the file.
            std::string token;
            r.tokenBlob = closed && unprotectToken(r.origin, r.tokenBlob, token) ? holdLocked(token) : std::string();
            wipe(token);
            r.checked = false;
            r.unreadable = false;
            LOGI("net: the saved session of %s is erased from %s (Options > Online): %s", r.origin.c_str(), path_.c_str(),
                 r.tokenBlob.empty() ? "signed out" : "kept in memory until the game quits");
            changed = true;
        } else if (closed) {
            // Allowed again (or the file closed since): the token kept in memory goes to the file.
            auto it = held_.find(r.tokenBlob);
            const std::string blob = it == held_.end() ? std::string() : protectToken(r.origin, it->second);
            if (blob.empty()) continue;
            dropLocked(r.tokenBlob);
            r.tokenBlob = blob;   // read from the record this run, whether the file is written or not
            r.checked = false;
            r.unreadable = false;
            noteFileFallback("Options > Online allows it", path_);
            changed = true;
        }
    }
    if (changed && !saveLocked()) LOGW("net: %s could not be written: the sessions it holds stay as they were", path_.c_str());
#endif
}

// ---- the keyring (under ioMu_, never mu_: the game thread's calls do not wait for it) ----

Keyring* CredentialStore::keyring() const {
    if (!keyringSet_) {
        keyring_ = defaultKeyring();
        keyringSet_ = true;
    }
    return keyring_;
}

void CredentialStore::setKeyring(Keyring* keyring) {
    std::lock_guard<std::mutex> io(ioMu_);
    keyring_ = keyring;
    keyringSet_ = true;
}

void CredentialStore::setUnlockTimeout(int ms) {
    std::lock_guard<std::mutex> io(ioMu_);
    unlockTimeoutMs_ = ms > 0 ? ms : 1;
}

void CredentialStore::interrupt() { cancel_.cancel(); }

template <typename Again>
Keyring::Result CredentialStore::afterUnlock(Keyring& k, Keyring::Result res, const std::string& origin, const std::string& id,
                                             Prompt prompt, Again again) const {
    if (res != Keyring::Result::Locked || prompt == Prompt::Never || (prompt == Prompt::Once && unlockDeclined_)) return res;
    if (!unlock(k, origin, id)) return cancel_.cancelled() ? Keyring::Result::Cancelled : res;
    res = again();
    if (res == Keyring::Result::Locked) unlockDeclined_ = true;   // unlocked, and still locked: not asked again
    return res;
}

// One prompt at a time (ioMu_), off the game thread (the store's keyring calls run on the network
// threads). Not shown again this run once dismissed, unanswered or failing (unlockDeclined_), but by
// a sign-in (Prompt::Always), whose prompt unlocked clears that.
bool CredentialStore::unlock(Keyring& k, const std::string& origin, const std::string& id) const {
    LOGI("net: the system keyring is locked: asking the desktop to unlock it");
    std::string why;
    Keyring::Result res;
    {
        Deadline deadline(cancel_, unlockTimeoutMs_);
        res = k.unlock(origin, id, deadline.token(), why);
    }
    if (res == Keyring::Result::Missing) return true;   // the item went meanwhile: the call says so
    if (res == Keyring::Result::Ok) {
        unlockDeclined_ = false;
        LOGI("net: the system keyring is unlocked");
        return true;
    }
    if (cancel_.cancelled()) return false;               // shutting down: nothing to remember
    if (res == Keyring::Result::Cancelled) why = "the unlock prompt was not answered in time";
    unlockDeclined_ = true;
    LOGW("net: the system keyring stays locked (%s): no other unlock prompt this run, except for a sign-in", why.c_str());
    return false;
}

// Only a sign-in saves a token (put): a locked keyring is worth a prompt then, even after one
// declined. Without a keyring that takes it (none, failing, locked, interrupt()), the fallback.
std::string CredentialStore::protect(const std::string& origin, const std::string& token) const {
    Keyring* k = keyring();
    std::string why = noKeyring();
    if (k) {
        const std::string id = newItemId();
        Keyring::Result res = Keyring::Result::Unavailable;
        if (id.empty()) {
            why = "no random id";
        } else {
            res = k->store(origin, id, token, &cancel_, why);
            res = afterUnlock(*k, res, origin, std::string(), Prompt::Always, [&] { return k->store(origin, id, token, &cancel_, why); });
        }
        if (res == Keyring::Result::Ok) return kKeyringPrefix + id;
        if (res == Keyring::Result::Cancelled) why = "the game is closing";
        if (res == Keyring::Result::Locked) {
            // Said at each sign-in: the keyring takes it once unlocked (migrate(), at a read).
            LOGW("net: the system keyring is locked (%s): the session of %s moves there once it is unlocked", why.c_str(),
                 origin.c_str());
            why = "the system keyring is locked";
        }
    }
    return fallback(origin, token, why);
}

// The file (DPAPI on Windows; in the clear on Linux, when the player allows it and the file is
// private), else memory: put() holds the token.
std::string CredentialStore::fallback(const std::string& origin, const std::string& token, const std::string& why) const {
#ifdef _WIN32
    (void)why;
    return protectToken(origin, token);
#elif defined(__APPLE__)
    // Never in the file there (kFileSessions): memory, until the game quits.
    (void)token;
    LOGW("net: the session of %s is kept in memory until the game quits, not in the keychain (%s)", origin.c_str(), why.c_str());
    return kMemoryPrefix;
#else
    bool allowed = fileSessionsAllowed(), closed = false;
    std::string at;
    {
        std::lock_guard<std::mutex> lk(mu_);
        loadLocked();
        if (allowed) closed = privateLocked();
        at = path_;
    }
    if (allowed && closed) {
        noteFileFallback(why, at);
        return protectToken(origin, token);
    }
    LOGW("net: the session of %s is kept in memory until the game quits, not in the system keyring (%s) nor in %s (%s)",
         origin.c_str(), why.c_str(), at.c_str(), allowed ? "other users can read it" : "not allowed by Options > Online");
    return kMemoryPrefix;
#endif
}

std::vector<std::string> CredentialStore::movedFrom(const std::string& origin) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> from;
    for (const auto& mv : moves_)
        if (mv.second == origin && mv.first != origin) from.push_back(mv.first);
    return from;
}

bool CredentialStore::read(const std::string& origin, const std::string& blob, Prompt prompt, std::string& token,
                           std::string& why, std::string* itemOrigin, Keyring::Result* status) const {
    token.clear();
    if (itemOrigin) *itemOrigin = origin;
    Keyring::Result res = Keyring::Result::Missing;
    if (inMemory(blob)) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = held_.find(blob);
        if (it != held_.end()) {
            token = it->second;
            res = Keyring::Result::Ok;
        }
    } else if (!inKeyring(blob)) {
        if (unprotectToken(origin, blob, token)) res = Keyring::Result::Ok;
    } else if (Keyring* k = keyring()) {
        const std::string id = itemId(blob);
        std::string at = origin;
        res = k->lookup(origin, id, token, &cancel_, why);
        // A record an origin move gave this origin: its item still names the former one. Only there,
        // so that a reference copied into another record still finds nothing.
        if (res == Keyring::Result::Missing) {
            for (const std::string& from : movedFrom(origin)) {
                res = k->lookup(from, id, token, &cancel_, why);
                if (res == Keyring::Result::Missing) continue;
                at = from;
                break;
            }
        }
        res = afterUnlock(*k, res, at, id, prompt, [&] { return k->lookup(at, id, token, &cancel_, why); });
        if (itemOrigin) *itemOrigin = at;
        if (res == Keyring::Result::Missing) why = "no such item";
        if (res == Keyring::Result::Cancelled) why = "interrupted";
    } else {
        why = "no keyring";
        res = Keyring::Result::Unavailable;
    }
    if (res == Keyring::Result::Ok && token.empty()) res = Keyring::Result::Missing;
    if (status) *status = res;
    if (res == Keyring::Result::Ok) return true;
    wipe(token);
    return false;
}

// A token the file or memory holds goes to the keyring, and a moved record's item becomes one of
// its new origin: the record then points to the new item. Nothing changes when the keyring does not
// take it (never a prompt here: background work), or when the file cannot be written (the new item
// is removed).
void CredentialStore::migrate(const std::string& origin, const std::string& blob, const std::string& token,
                              const std::string& itemOrigin) const {
    Keyring* k = keyring();
    if (cancel_.cancelled()) return;
    if (!k) {
        if (inFile(blob)) noteFileFallback(noKeyring(), path());
        return;
    }
    const std::string id = newItemId();
    std::string why;
    if (id.empty()) return;
    const Keyring::Result res = k->store(origin, id, token, &cancel_, why);
    if (res != Keyring::Result::Ok) {
        if (res != Keyring::Result::Cancelled && inFile(blob)) noteFileFallback(why, path());
        return;
    }
    bool saved = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Record* r = findLocked(origin);
        if (r && r->tokenBlob == blob) {
            r->tokenBlob = kKeyringPrefix + id;
            saved = saveLocked();
            if (!saved) r->tokenBlob = blob;   // the file (or memory) still holds it
            else if (inMemory(blob)) dropLocked(blob);
        }
    }
    if (!saved) {
        if (notRemoved(k->remove(origin, id, &cancel_, why)))
            LOGW("net: a copy of the saved session of %s could not be removed from the system keyring (%s)", origin.c_str(), why.c_str());
    } else if (inKeyring(blob)) {
        if (notRemoved(k->remove(itemOrigin, itemId(blob), &cancel_, why)))
            LOGW("net: the saved session of %s could not be removed from the system keyring (%s)", itemOrigin.c_str(), why.c_str());
    } else {
        LOGI("net: the saved session of %s moved to the system keyring", origin.c_str());
    }
}

// A logout, a forgotten server, a replaced token: a locked keyring is worth a prompt (Prompt::Once).
void CredentialStore::forget(const std::string& origin, const std::string& blob) const {
    if (inMemory(blob)) {
        std::lock_guard<std::mutex> lk(mu_);
        dropLocked(blob);
        return;
    }
    if (!inKeyring(blob)) return;
    Keyring* k = keyring();
    if (!k) return;
    const std::string id = itemId(blob);
    auto removeAt = [&](const std::string& at) {
        std::string why;
        Keyring::Result res = k->remove(at, id, &cancel_, why);
        res = afterUnlock(*k, res, at, id, Prompt::Once, [&] { return k->remove(at, id, &cancel_, why); });
        if (notRemoved(res)) LOGW("net: the saved session of %s could not be removed from the system keyring (%s)", at.c_str(), why.c_str());
    };
    removeAt(origin);
    // A moved record not read since: its item names the former origin (its id is random: it can
    // only be this one).
    for (const std::string& from : movedFrom(origin)) removeAt(from);
}

std::string CredentialStore::username(const std::string& origin) const {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    const Record* r = findLocked(origin);
    return r ? r->username : std::string();
}

std::string CredentialStore::pin(const std::string& origin) const {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    const Record* r = findLocked(origin);
    return r ? r->pin : std::string();
}

bool CredentialStore::put(const Credential& c, bool* stored, Kept* kept) {
    if (stored) *stored = false;
    if (kept) *kept = Kept::None;
    if (c.origin.empty()) return false;
    std::lock_guard<std::mutex> io(ioMu_);
    std::string blob;
    if (!c.token.empty()) {
        blob = protect(c.origin, c.token);
        if (blob.empty()) return false;
    }
    if (stored) *stored = true;
    std::string old;
    bool saved;
    {
        std::lock_guard<std::mutex> lk(mu_);
        loadLocked();
        if (inMemory(blob)) blob = holdLocked(c.token);
        Record* r = findLocked(c.origin);
        if (!r) {
            records_.push_back(Record());
            r = &records_.back();
            r->origin = c.origin;
        }
        old = r->tokenBlob;
        r->username = c.username;
        r->serverId = c.serverId;
        r->pin = c.pinnedSha256;
        r->tokenBlob = blob;
        r->unreadable = false;
        r->checked = true;
        saved = saveLocked();
        // Where it is now (a file that turned out open to other users dropped it: saveLocked).
        if (kept && !r->tokenBlob.empty())
            *kept = inKeyring(r->tokenBlob) ? Kept::Keyring : inMemory(r->tokenBlob) ? Kept::Memory : Kept::File;
    }
    // The previous token's item (or the token kept in memory), once the file no longer points to it.
    if (old != blob && (saved || inMemory(old))) forget(c.origin, old);
    return saved;
}

bool CredentialStore::firstNotice(Kept kept) const {
#ifdef _WIN32
    (void)kept;
    return false;
#else
    if (kept == Kept::File) return !noticedFile_.exchange(true);
    if (kept == Kept::Memory) return !noticedMemory_.exchange(true);
    return false;
#endif
}

bool CredentialStore::clearToken(const std::string& origin) {
    std::lock_guard<std::mutex> io(ioMu_);
    std::string old;
    bool saved;
    {
        std::lock_guard<std::mutex> lk(mu_);
        loadLocked();
        Record* r = findLocked(origin);
        if (!r) return true;
        old = r->tokenBlob;
        r->tokenBlob.clear();
        saved = saveLocked();
    }
    if (saved || inMemory(old)) forget(origin, old);
    return saved;
}

bool CredentialStore::clearToken(const std::string& origin, const std::string& token) {
    std::lock_guard<std::mutex> io(ioMu_);
    std::string blob;
    {
        std::lock_guard<std::mutex> lk(mu_);
        loadLocked();
        Record* r = findLocked(origin);
        if (!r || r->tokenBlob.empty()) return true;
        blob = r->tokenBlob;
    }
    std::string saved, why;
    Keyring::Result status = Keyring::Result::Ok;
    const bool readable = read(origin, blob, Prompt::Never, saved, why, nullptr, &status);
    const bool another = readable && saved != token;
    wipe(saved);
    if (another) return true;   // another one since
    // The keyring cannot say which token it keeps (locked, failing, interrupted): the reference
    // stays, since it may name one saved since. A refused token is cleared at its next refusal.
    if (unanswered(status)) return false;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Record* r = findLocked(origin);
        if (!r || r->tokenBlob != blob) return true;
        r->tokenBlob.clear();
        ok = saveLocked();
    }
    if (ok || inMemory(blob)) forget(origin, blob);
    return ok;
}

bool CredentialStore::clearPin(const std::string& origin) {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    Record* r = findLocked(origin);
    if (!r || r->pin.empty()) return true;
    r->pin.clear();
    return saveLocked();
}

bool CredentialStore::erase(const std::string& origin) {
    std::lock_guard<std::mutex> io(ioMu_);
    std::string old;
    bool saved = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        loadLocked();
        for (size_t i = 0; i < records_.size(); ++i)
            if (records_[i].origin == origin) {
                old = records_[i].tokenBlob;
                records_.erase(records_.begin() + std::ptrdiff_t(i));
                saved = saveLocked();
                break;
            }
    }
    if (saved || inMemory(old)) forget(origin, old);
    return saved;
}

std::vector<std::string> CredentialStore::origins() const {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    std::vector<std::string> v;
    for (const Record& r : records_) v.push_back(r.origin);
    return v;
}

}  // namespace net

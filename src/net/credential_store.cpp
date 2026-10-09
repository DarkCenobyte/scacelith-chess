#include "credential_store.h"
#include "crypto.h"
#include "json.h"
#include "net_sys.h"
#include "../core/log.h"
#include <atomic>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
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

bool inKeyring(const std::string& blob) { return blob.compare(0, sizeof(kKeyringPrefix) - 1, kKeyringPrefix) == 0; }
std::string itemId(const std::string& blob) { return blob.substr(sizeof(kKeyringPrefix) - 1); }

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

// Linux: why the sessions stay in the file, said once per run.
void noteFileFallback(const std::string& why, const std::string& path) {
#ifndef _WIN32
    static std::atomic<bool> said{false};
    if (said.exchange(true)) return;
    LOGW("net: no system keyring for the saved sessions (%s): they are written to %s in the clear, protected only by "
         "the file's permissions",
         why.c_str(), path.c_str());
#else
    (void)why;
    (void)path;
#endif
}
}  // namespace

namespace {
bool keyringOff() {
    const char* env = std::getenv("SCACELITH_KEYRING");
    return env && std::strcmp(env, "off") == 0;
}
}  // namespace

Keyring* defaultKeyring() { return keyringOff() ? nullptr : secretServiceKeyring(); }

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

CredentialStore::CredentialStore(std::string path) : path_(std::move(path)) {}

void CredentialStore::setPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
    loaded_ = false;
    records_.clear();
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
        if (rec.origin.empty() || findLocked(rec.origin)) continue;   // one record per origin
        records_.push_back(std::move(rec));
    }
    applyMovesLocked();
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
        if (inKeyring(r->tokenBlob))
            blob = r->tokenBlob;
        else if (!r->tokenBlob.empty() && unprotectToken(mv.first, r->tokenBlob, token))
            blob = protectToken(mv.second, token);
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
    json::Value doc = json::Value::object();
    doc.set("version", 1);
    json::Value& list = doc.set("records", json::Value::array());
    for (const Record& r : records_) {
        json::Value o = json::Value::object();
        o.set("origin", r.origin);
        o.set("username", r.username);
        o.set("serverId", r.serverId);
        o.set("pin", r.pin);
        o.set("token", r.tokenBlob);
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

bool CredentialStore::get(const std::string& origin, Credential& out) const {
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
            // In the file: read here. The keyring takes it once that works.
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
    const bool readable = read(origin, blob, out.token, why, &itemOrigin);
    {
        std::lock_guard<std::mutex> lk(mu_);
        Record* r = findLocked(origin);
        if (r && r->tokenBlob == blob) {
            // Said once; hasToken() follows the last read (an unlocked keyring makes it readable again).
            if (!readable && !r->unreadable) LOGW("net: the saved session of %s is not readable from the system keyring (%s)", origin.c_str(), why.c_str());
            r->unreadable = !readable;
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
    bool readable = unprotectToken(r.origin, r.tokenBlob, token);
    if (!readable && !r.unreadable) LOGW("net: the saved session of %s cannot be decrypted here", r.origin.c_str());
    r.unreadable = !readable;
    r.checked = true;
    return readable;
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

void CredentialStore::interrupt() { cancel_.cancel(); }

// The blob that keeps a token: a keyring item when the keyring takes it, else the file's own
// format. "" when it could not be kept (DPAPI failed, or interrupt()).
std::string CredentialStore::protect(const std::string& origin, const std::string& token) const {
    Keyring* k = keyring();
    std::string why = keyringOff() ? "SCACELITH_KEYRING=off" : "libsecret-1.so.0 cannot be loaded";
    if (k) {
        const std::string id = newItemId();
        const Keyring::Result res = id.empty() ? Keyring::Result::Unavailable : k->store(origin, id, token, &cancel_, why);
        if (res == Keyring::Result::Ok) return kKeyringPrefix + id;
        if (res == Keyring::Result::Cancelled) return std::string();
        if (id.empty()) why = "no random id";
    }
    noteFileFallback(why, path());
    return protectToken(origin, token);
}

std::vector<std::string> CredentialStore::movedFrom(const std::string& origin) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> from;
    for (const auto& mv : moves_)
        if (mv.second == origin && mv.first != origin) from.push_back(mv.first);
    return from;
}

bool CredentialStore::read(const std::string& origin, const std::string& blob, std::string& token, std::string& why,
                           std::string* itemOrigin) const {
    token.clear();
    if (itemOrigin) *itemOrigin = origin;
    if (!inKeyring(blob)) return unprotectToken(origin, blob, token);
    Keyring* k = keyring();
    if (!k) {
        why = "no keyring";
        return false;
    }
    const std::string id = itemId(blob);
    Keyring::Result res = k->lookup(origin, id, token, &cancel_, why);
    // A record an origin move gave this origin: its item still names the former one. Only there,
    // so that a reference copied into another record still finds nothing.
    if (res == Keyring::Result::Missing) {
        for (const std::string& from : movedFrom(origin)) {
            res = k->lookup(from, id, token, &cancel_, why);
            if (res == Keyring::Result::Missing) continue;
            if (itemOrigin) *itemOrigin = from;
            break;
        }
    }
    if (res == Keyring::Result::Missing) why = "no such item";
    if (res == Keyring::Result::Cancelled) why = "interrupted";
    if (res == Keyring::Result::Ok && !token.empty()) return true;
    wipe(token);
    return false;
}

// A token the file holds goes to the keyring, and a moved record's item becomes one of its new
// origin: the record then points to the new item. Nothing changes when the keyring does not take
// it, or when the file cannot be written (the new item is removed).
void CredentialStore::migrate(const std::string& origin, const std::string& blob, const std::string& token,
                              const std::string& itemOrigin) const {
    Keyring* k = keyring();
    if (!k || cancel_.cancelled()) return;
    const std::string id = newItemId();
    std::string why;
    if (id.empty() || k->store(origin, id, token, &cancel_, why) != Keyring::Result::Ok) return;
    bool saved = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Record* r = findLocked(origin);
        if (r && r->tokenBlob == blob) {
            r->tokenBlob = kKeyringPrefix + id;
            saved = saveLocked();
            if (!saved) r->tokenBlob = blob;   // the file still holds it
        }
    }
    if (!saved) {
        k->remove(origin, id, &cancel_, why);
    } else if (inKeyring(blob)) {
        k->remove(itemOrigin, itemId(blob), &cancel_, why);
    } else {
        LOGI("net: the saved session of %s moved to the system keyring", origin.c_str());
    }
}

void CredentialStore::forget(const std::string& origin, const std::string& blob) const {
    if (!inKeyring(blob)) return;
    Keyring* k = keyring();
    if (!k) return;
    std::string why;
    if (k->remove(origin, itemId(blob), &cancel_, why) == Keyring::Result::Unavailable)
        LOGW("net: the saved session of %s could not be removed from the system keyring (%s)", origin.c_str(), why.c_str());
    // A moved record not read since: its item names the former origin (its id is random: it can
    // only be this one).
    for (const std::string& from : movedFrom(origin)) k->remove(from, itemId(blob), &cancel_, why);
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

bool CredentialStore::put(const Credential& c, bool* stored) {
    if (stored) *stored = false;
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
    }
    // The previous token's item, once the file no longer points to it.
    if (saved && old != blob) forget(c.origin, old);
    return saved;
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
    if (saved) forget(origin, old);
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
    const bool readable = read(origin, blob, saved, why);
    const bool another = readable && saved != token;
    wipe(saved);
    if (another) return true;   // another one since
    bool ok;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Record* r = findLocked(origin);
        if (!r || r->tokenBlob != blob) return true;
        r->tokenBlob.clear();
        ok = saveLocked();
    }
    if (ok) forget(origin, blob);
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
    if (saved) forget(origin, old);
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

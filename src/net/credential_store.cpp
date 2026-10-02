#include "credential_store.h"
#include "crypto.h"
#include "json.h"
#include "net_sys.h"
#include "../core/log.h"
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

void wipe(std::string& s) {
    if (s.empty()) return;
    volatile char* p = &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}
}  // namespace

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
    // Development builds: no OS secret store; the token is bound to its origin in the clear.
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
    std::string exe = sys::exeDirectory() + kFileName;
    std::string data = sys::userDataDirectory() + kFileName;
    if (sys::fileExists(exe)) return exe;
    if (sys::fileExists(data)) return data;
    // Same place as Scacelith.ini: next to the executable when that directory is writable.
    return sys::directoryWritable(sys::exeDirectory()) ? exe : data;
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
        // old origin, wrap it again for the new one. One that cannot be read here moves without it.
        std::string token, blob;
        if (!r->tokenBlob.empty() && unprotectToken(mv.first, r->tokenBlob, token)) blob = protectToken(mv.second, token);
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
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    const Record* r = findLocked(origin);
    if (!r) return false;
    out = Credential();
    out.origin = r->origin;
    out.username = r->username;
    out.serverId = r->serverId;
    out.pinnedSha256 = r->pin;
    if (!r->tokenBlob.empty() && !unprotectToken(origin, r->tokenBlob, out.token))
        LOGW("net: the saved session of %s cannot be decrypted here", origin.c_str());
    return true;
}

bool CredentialStore::hasToken(const std::string& origin) const {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    const Record* r = findLocked(origin);
    return r && !r->tokenBlob.empty();
}

std::string CredentialStore::username(const std::string& origin) const {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    const Record* r = findLocked(origin);
    return r ? r->username : std::string();
}

bool CredentialStore::put(const Credential& c) {
    if (c.origin.empty()) return false;
    std::string blob;
    if (!c.token.empty()) {
        blob = protectToken(c.origin, c.token);
        if (blob.empty()) return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    Record* r = findLocked(c.origin);
    if (!r) {
        records_.push_back(Record());
        r = &records_.back();
        r->origin = c.origin;
    }
    r->username = c.username;
    r->serverId = c.serverId;
    r->pin = c.pinnedSha256;
    r->tokenBlob = blob;
    return saveLocked();
}

bool CredentialStore::clearToken(const std::string& origin) {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    Record* r = findLocked(origin);
    if (!r) return true;
    r->tokenBlob.clear();
    return saveLocked();
}

bool CredentialStore::clearToken(const std::string& origin, const std::string& token) {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    Record* r = findLocked(origin);
    if (!r || r->tokenBlob.empty()) return true;
    std::string saved;
    if (unprotectToken(origin, r->tokenBlob, saved) && saved != token) return true;   // another one since
    r->tokenBlob.clear();
    return saveLocked();
}

bool CredentialStore::erase(const std::string& origin) {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    for (size_t i = 0; i < records_.size(); ++i)
        if (records_[i].origin == origin) {
            records_.erase(records_.begin() + std::ptrdiff_t(i));
            return saveLocked();
        }
    return true;
}

std::vector<std::string> CredentialStore::origins() const {
    std::lock_guard<std::mutex> lk(mu_);
    loadLocked();
    std::vector<std::string> v;
    for (const Record& r : records_) v.push_back(r.origin);
    return v;
}

}  // namespace net

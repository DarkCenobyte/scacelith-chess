// Per-server credentials of the online client, kept apart from Scacelith.ini.
//
// One record per server origin ("host:apiPort", ServerEndpoint::origin()): the user name last
// used there, the session token, the server id announced by /api/v1/info when the token was
// obtained, and the pinned certificate fingerprint (hex SHA-256) if the player set one.
//
// The file (Scacelith.credentials, JSON) lives next to the executable like Scacelith.ini, or in
// the user data directory (%APPDATA%\Scacelith\, ~/.config/scacelith/) when the executable's
// directory is not writable. Tokens are never stored in clear on Windows: DPAPI
// (CryptProtectData, current user, CRYPTPROTECT_UI_FORBIDDEN) with the origin as additional
// entropy, so a token blob moved to another origin's record cannot be decrypted there. Linux
// builds (development only) write the file with mode 0600 and bind each token to its origin in
// the clear. Either way get(origin) only ever returns a token that was saved for that origin.
//
// Thread-safe (one mutex); the file is read on first use and rewritten atomically on changes.
#pragma once
#include <mutex>
#include <string>
#include <vector>

namespace net {

struct Credential {
    std::string origin;
    std::string username;
    std::string token;           // session token ("" = logged out)
    std::string serverId;        // /api/v1/info serverId when the token was saved
    std::string pinnedSha256;    // hex, lower-case; "" = OS trust store
};

class CredentialStore {
public:
    explicit CredentialStore(std::string path = std::string());   // "" = defaultPath()
    static std::string defaultPath();
    void setPath(const std::string& path);       // switches file (reloaded on next use)
    std::string path() const;

    // The record of this exact origin, token decrypted. A token that cannot be decrypted for
    // this origin (other user, other machine, blob copied from another origin) comes back empty.
    bool get(const std::string& origin, Credential& out) const;
    bool hasToken(const std::string& origin) const;   // without decrypting
    std::string username(const std::string& origin) const;

    bool put(const Credential& c);               // creates or replaces c.origin's record; saves
    bool clearToken(const std::string& origin);  // logout: keeps user name, server id and pin
    bool erase(const std::string& origin);       // forgets the origin entirely
    std::vector<std::string> origins() const;

private:
    struct Record {
        std::string origin, username, serverId, pin, tokenBlob;
    };
    mutable std::mutex mu_;
    mutable std::string path_;
    mutable bool loaded_ = false;
    mutable std::vector<Record> records_;

    void loadLocked() const;
    bool saveLocked() const;
    Record* findLocked(const std::string& origin) const;
};

// Token protection used by the store (exposed for the unit tests).
std::string protectToken(const std::string& origin, const std::string& token);    // "" on failure
bool unprotectToken(const std::string& origin, const std::string& blob, std::string& token);

}  // namespace net

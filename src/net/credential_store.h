// Per-server credentials of the online client, kept apart from Scacelith.ini.
//
// One record per server origin ("host:apiPort", ServerEndpoint::origin()): the user name last
// used there, the session token, the server id announced by /api/v1/info when the token was
// obtained, and the pinned certificate fingerprint (hex SHA-256) if the player set one.
//
// The file (Scacelith.credentials, JSON) lives with Scacelith.ini (net::sys::settingsDirectory():
// the user data directory %APPDATA%\scacelith\ or $XDG_CONFIG_HOME/scacelith/, or the executable's folder
// of a portable install), or next to an explicit --ini file. Where the tokens are kept:
//   - Windows: in the file, never in clear: DPAPI (CryptProtectData, current user,
//     CRYPTPROTECT_UI_FORBIDDEN) with the origin as additional entropy, so a token blob moved to
//     another origin's record cannot be decrypted there ("dpapi:").
//   - Linux: in the system keyring when there is one (Keyring below: the Secret Service, GNOME
//     Keyring, KWallet, KeePassXC...), one item per token found by its origin and a random id; the
//     file holds only "keyring:" and that id. A token the file still holds in the format below
//     moves to the keyring the first time it is read while the keyring works.
//   - Linux without a usable keyring (none installed or running, no D-Bus session, the keyring
//     locked: the game never asks to unlock it, SCACELITH_KEYRING=off): in the file, bound to its
//     origin but in the clear ("bound:", base64url of origin + '\n' + token), protected only by the
//     file's permissions (0600, in a 0700 folder). Said once in the log.
// Either way get(origin) only ever returns a token that was saved for that origin.
//
// Thread-safe. The file is read on first use and rewritten atomically on changes. The calls the
// game thread makes (hasToken, username, pin, origins) never wait for the keyring: its calls
// (get, put, clearToken, erase, on the network threads) run outside the lock of the records, one
// at a time.
//
// Origin moves (addOriginMove): a server that changed its port keeps its players signed in. The
// online client registers one for the official server of the build, which moved from port 44664
// to 443: on load, a file with a record for "host:44664" and none for "host:443" has that record
// moved (user name, token re-protected for the new origin, server id, pin) and is saved again, so
// it happens once (a token the keyring keeps moves to an item of the new origin on its first
// read). Never for a community server: the client registers no other move.
#pragma once
#include "transport.h"   // CancelToken
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace net {

// The system's secret store, where a Linux build keeps the session tokens. Each item is found by
// its origin and an id the store makes for it (new for every token). The tests give a store a
// fake one (CredentialStore::setKeyring).
class Keyring {
public:
    enum class Result {
        Ok,
        Missing,       // lookup: no such item
        Unavailable,   // no keyring, a locked one, or an error: why says which
        Cancelled,     // the store was interrupted (CredentialStore::interrupt)
    };
    virtual ~Keyring() = default;
    // Keeps secret under (origin, id), replacing an item with the same two.
    virtual Result store(const std::string& origin, const std::string& id, const std::string& secret, CancelToken* cancel,
                         std::string& why) = 0;
    virtual Result lookup(const std::string& origin, const std::string& id, std::string& secret, CancelToken* cancel,
                          std::string& why) = 0;
    // Ok also when there was no such item.
    virtual Result remove(const std::string& origin, const std::string& id, CancelToken* cancel, std::string& why) = 0;
};

// The Secret Service (org.freedesktop.secrets over the D-Bus session bus) through libsecret,
// loaded at run time: the game is not linked with it (libsecret-1.so.0, secret_service.cpp).
// nullptr on Windows and when the library cannot be loaded. It never prompts: a locked keyring,
// or one without a default collection, is Unavailable.
Keyring* secretServiceKeyring();
// The keyring new stores use: secretServiceKeyring(), or none with SCACELITH_KEYRING=off in the
// environment (the unit tests run so: tests/test_main.cpp).
Keyring* defaultKeyring();

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
    // this origin (other user, other machine, blob copied from another origin) comes back empty
    // and is no token for hasToken(), this run (the file keeps it: another Windows account
    // sharing a portable install may own it).
    bool get(const std::string& origin, Credential& out) const;
    // A token is saved and can be decrypted here (tried once per record and run).
    bool hasToken(const std::string& origin) const;
    std::string username(const std::string& origin) const;
    std::string pin(const std::string& origin) const;   // the saved pin (without decrypting the token)

    // Creates or replaces c.origin's record and saves the file. False when the token could not be
    // protected (nothing changed: *stored false) or the file not written (*stored true: the record
    // holds for this run).
    bool put(const Credential& c, bool* stored = nullptr);
    bool clearToken(const std::string& origin);  // logout: keeps user name, server id and pin
    // The same, only while the saved token is 'token' (the one a server refused): a token saved
    // since (a new sign-in on another thread) is kept.
    bool clearToken(const std::string& origin, const std::string& token);
    bool clearPin(const std::string& origin);    // forgets the pin only (keeps the session)
    bool erase(const std::string& origin);       // forgets the origin entirely
    std::vector<std::string> origins() const;
    // When the file has no record for `to` and has one for `from`, that record becomes `to`'s
    // (applied at every load of a file, at once when one is loaded; see the note above). A token
    // the keyring keeps is found under `from` while the rule is registered, and moves to an item
    // of `to` on its first read (get).
    void addOriginMove(const std::string& from, const std::string& to);

    // The keyring of the tokens; nullptr: the file only. defaultKeyring() until set.
    void setKeyring(Keyring* keyring);
    // Ends a keyring call in progress and makes the next ones fail at once, without falling back
    // to the file (the client is shutting down: OnlineClient's destructor).
    void interrupt();

private:
    struct Record {
        std::string origin, username, serverId, pin, tokenBlob;
        bool checked = false;      // whether tokenBlob decrypts here is known (get, hasToken, put)
        bool unreadable = false;   // tokenBlob could not be decrypted (or found in the keyring) here
    };
    mutable std::mutex mu_;        // the records, the path and the file
    mutable std::mutex ioMu_;      // one keyring call sequence at a time (taken before mu_)
    mutable std::string path_;
    mutable bool loaded_ = false;
    mutable std::vector<Record> records_;
    std::vector<std::pair<std::string, std::string>> moves_;   // from, to
    mutable bool keyringSet_ = false;
    mutable Keyring* keyring_ = nullptr;
    mutable CancelToken cancel_;   // interrupt()

    void loadLocked() const;
    void applyMovesLocked() const;
    bool saveLocked() const;
    Record* findLocked(const std::string& origin) const;
    bool readLocked(Record& r, std::string& token) const;   // decrypts r's file-held token, notes the outcome
    std::vector<std::string> movedFrom(const std::string& origin) const;   // the moves' sources for origin
    // Under ioMu_, not mu_ (they may call the keyring):
    Keyring* keyring() const;
    std::string protect(const std::string& origin, const std::string& token) const;
    // *itemOrigin: the origin the keyring item was found under (a moved record's is its former one).
    bool read(const std::string& origin, const std::string& blob, std::string& token, std::string& why,
              std::string* itemOrigin = nullptr) const;
    // Stores token in a new keyring item of origin and points the record to it, while it still
    // holds blob; then removes blob's item, if it is one (found under itemOrigin).
    void migrate(const std::string& origin, const std::string& blob, const std::string& token,
                 const std::string& itemOrigin = std::string()) const;
    void forget(const std::string& origin, const std::string& blob) const;   // removes a keyring item
};

// Token protection used by the store (exposed for the unit tests).
std::string protectToken(const std::string& origin, const std::string& token);    // "" on failure
bool unprotectToken(const std::string& origin, const std::string& blob, std::string& token);

}  // namespace net

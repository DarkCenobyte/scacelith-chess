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
//     file holds only "keyring:" and that id. A token kept as below moves to the keyring the first
//     time it is read while the keyring works (and is unlocked).
//   - Linux when no keyring can keep it (none installed or running, no D-Bus session,
//     SCACELITH_KEYRING=off, a locked one the player did not unlock (below), the client shutting
//     down): by default in the file, bound to its origin but in the clear ("bound:", base64url of
//     origin + '\n' + token), protected by the file's permissions: the file 0600 in a 0700 folder,
//     checked at every read and write of the file and repaired when they are more open (ssh's rule;
//     the log says so). Where they cannot be repaired, no token in the clear is read from or
//     written to that file. The player is told once per run (firstNotice: the sign-in's notice).
//   - Linux with the option "Remember my sign-in when the system keyring is unavailable" off
//     (Options > Online; setFileSessionsAllowed), or a file that cannot be made private: in memory
//     only, until the game quits ("memory:" records are written without their token). A token an
//     earlier run left in the file is then moved to memory (unread when the file is open to other
//     users) and erased from the file.
// Either way get(origin) only ever returns a token that was saved for that origin. The choice of the
// file by default is an accepted risk (audit A08): docs/ONLINE_CLIENT.md, "Where the sessions are
// kept".
//
// A locked keyring (Linux): the store asks the desktop to unlock it (the Secret Service shows its
// own prompt) when a sign-in saves a token (put), when a token is needed now (get with unlock: a
// connection, a request that needs the session) and when one is removed (logout, a forgotten
// server); never for background work (a token moving to the keyring, a token a server refused).
// The store waits kUnlockTimeoutMs at most for the answer, and stops waiting at interrupt() (the
// prompt stays on the desktop: answered later, it still unlocks the keyring). One dismissed,
// unanswered or failing is not shown again this run, except for a new sign-in: that sign-in's token
// then goes where a token goes without a keyring (above: it moves to the keyring at a read once the
// keyring is unlocked), a read finds no session this time (the reference stays: hasToken() is false
// until a read succeeds), and a removal leaves the item (the log says so).
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
#include <atomic>
#include <map>
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
        Missing,       // lookup, unlock: no such item
        Locked,        // the item, or the collection a new item goes to, is locked: unlock() opens it
        Unavailable,   // no keyring, or an error (a dismissed unlock prompt too): why says which
        Cancelled,     // cancel fired: the store was interrupted (CredentialStore::interrupt), or
                       // an unlock prompt timed out
    };
    virtual ~Keyring() = default;
    // Keeps secret under (origin, id), replacing an item with the same two.
    virtual Result store(const std::string& origin, const std::string& id, const std::string& secret, CancelToken* cancel,
                         std::string& why) = 0;
    virtual Result lookup(const std::string& origin, const std::string& id, std::string& secret, CancelToken* cancel,
                          std::string& why) = 0;
    // Ok also when there was no such item; Locked or Unavailable while it is still there (a locked
    // keyring keeps its items).
    virtual Result remove(const std::string& origin, const std::string& id, CancelToken* cancel, std::string& why) = 0;
    // Asks the keyring to unlock the item (origin, id), or the collection new items go to when id is
    // "": the desktop shows its unlock prompt. Ok once unlocked (or when it was not locked);
    // Unavailable when the player dismissed the prompt or it failed; Cancelled once cancel fires
    // (no longer waited for, the prompt may stay). Blocks until then: the store gives it a token
    // with a timeout.
    virtual Result unlock(const std::string& origin, const std::string& id, CancelToken* cancel, std::string& why) = 0;
};

// The Secret Service (org.freedesktop.secrets over the D-Bus session bus) through libsecret,
// loaded at run time: the game is not linked with it (libsecret-1.so.0, secret_service.cpp).
// nullptr on Windows and when the library cannot be loaded. Only unlock() prompts: the other calls
// find a locked keyring Locked, and one without a default collection Unavailable.
Keyring* secretServiceKeyring();
// The keyring new stores use: secretServiceKeyring(), or none with SCACELITH_KEYRING=off in the
// environment (the unit tests run so: tests/test_main.cpp).
Keyring* defaultKeyring();
// Linux: whether the stores may keep a session no keyring can keep in their file (in the clear,
// 0600; the note above), or only in memory until the game quits. The game's option "Remember my
// sign-in when the system keyring is unavailable" (game::Settings::onlineRememberWithoutKeyring),
// on by default. Applied at once to every store already loaded: off, the tokens their files hold
// move to memory and are erased from the files; on, the tokens kept in memory go to the files.
// No effect on Windows (DPAPI).
void setFileSessionsAllowed(bool allowed);
bool fileSessionsAllowed();

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
    ~CredentialStore();
    CredentialStore(const CredentialStore&) = delete;
    CredentialStore& operator=(const CredentialStore&) = delete;
    static std::string defaultPath();
    void setPath(const std::string& path);       // switches file (reloaded on next use)
    std::string path() const;

    // The record of this exact origin, token decrypted. A token that cannot be decrypted for
    // this origin (other user, other machine, blob copied from another origin) comes back empty
    // and is no token for hasToken(), this run (the file keeps it: another Windows account
    // sharing a portable install may own it). unlock: the token is needed now (a connection, a
    // request that needs the session), worth the keyring's unlock prompt when the keyring keeps it
    // locked (see the note above). Without it such a token comes back empty, and still counts for
    // hasToken() until a prompt was dismissed or unanswered this run.
    bool get(const std::string& origin, Credential& out, bool unlock = false) const;
    // A token is saved and can be decrypted here (tried once per record and run).
    bool hasToken(const std::string& origin) const;
    std::string username(const std::string& origin) const;
    std::string pin(const std::string& origin) const;   // the saved pin (without decrypting the token)

    // Where put() kept a token: nowhere (no token, or put() failed), in the system keyring, in the
    // file (Windows: DPAPI; Linux: in the clear, no keyring could keep it), or in memory until the
    // game quits (Linux: no keyring could keep it, and the file may not).
    enum class Kept { None, Keyring, File, Memory };
    // Creates or replaces c.origin's record and saves the file. False when the token could not be
    // protected (nothing changed: *stored false) or the file not written (*stored true: the record
    // holds for this run).
    bool put(const Credential& c, bool* stored = nullptr, Kept* kept = nullptr);
    // Whether the player is to be told where a sign-in's token was kept: true the first time this
    // run (per store) that put() kept one outside the keyring on Linux (File or Memory, each once);
    // false for the keyring and on Windows.
    bool firstNotice(Kept kept) const;
    bool clearToken(const std::string& origin);  // logout: keeps user name, server id and pin
    // The same, only while the saved token is 'token' (the one a server refused): a token saved
    // since (a new sign-in on another thread) is kept, and so is one the keyring cannot show now
    // (locked, failing, interrupted: false).
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
    // How long an unlock prompt waits for the player (kUnlockTimeoutMs; the tests shorten it).
    void setUnlockTimeout(int ms);
    static constexpr int kUnlockTimeoutMs = 60000;
    // Ends a keyring call in progress (an unlock prompt too) and makes the next ones fail at once
    // (the client is shutting down: OnlineClient's destructor). A sign-in's token then goes where a
    // token goes without a keyring (the note above); reads and removals keep the files as they are.
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
    // Under ioMu_: an unlock prompt was dismissed, unanswered or failed this run (only a sign-in
    // asks again).
    mutable bool unlockDeclined_ = false;
    int unlockTimeoutMs_ = kUnlockTimeoutMs;
    // Under mu_: the tokens kept in memory only (Linux), by their records' "memory:" blobs.
    mutable std::map<std::string, std::string> held_;
    mutable unsigned heldSeq_ = 0;
    mutable bool openWarned_ = false;   // under mu_: the file was said open to other users
    mutable std::atomic<bool> noticedFile_{false}, noticedMemory_{false};   // firstNotice

    // When a keyring call that finds the keyring locked asks the desktop to unlock it.
    enum class Prompt {
        Never,   // background work: a token moving to the keyring, a token a server refused
        Once,    // a token needed now, a removal: not after a prompt declined this run
        Always,  // a sign-in's token (put): asks again
    };

    void loadLocked() const;
    void applyMovesLocked() const;
    bool saveLocked() const;
    Record* findLocked(const std::string& origin) const;
    bool readLocked(Record& r, std::string& token) const;   // reads r's token from the file or memory, notes the outcome
    // Linux: whether the file may hold a token in the clear (its folder 0700, itself 0600, repaired
    // when more open). Always true on Windows.
    bool privateLocked() const;
    std::string holdLocked(const std::string& token) const;   // keeps token in memory: its "memory:" blob
    void dropLocked(const std::string& blob) const;           // forgets a token kept in memory
    // The records follow setFileSessionsAllowed (and the file's permissions): see there.
    void reconcileLocked() const;
    friend void setFileSessionsAllowed(bool allowed);
    std::vector<std::string> movedFrom(const std::string& origin) const;   // the moves' sources for origin
    // Under ioMu_, not mu_ (they may call the keyring):
    Keyring* keyring() const;
    // The blob that keeps a sign-in's token: a keyring item, else fallback(). "" when it could not
    // be kept at all (DPAPI failed). A "memory:" blob is held by put().
    std::string protect(const std::string& origin, const std::string& token) const;
    // Where a token no keyring keeps goes: the file, else (Linux) memory (kMemoryPrefix alone).
    std::string fallback(const std::string& origin, const std::string& token, const std::string& why) const;
    // *itemOrigin: the origin the keyring item was found under (a moved record's is its former one).
    // *status: Ok with the token; Missing when it is not there or cannot be read at all; Locked,
    // Unavailable or Cancelled when the keyring could not say what it keeps.
    bool read(const std::string& origin, const std::string& blob, Prompt prompt, std::string& token, std::string& why,
              std::string* itemOrigin = nullptr, Keyring::Result* status = nullptr) const;
    // res: what a keyring call on (origin, id) ("": a new item) gave. When it found the keyring
    // locked and prompt allows it, asks the desktop to unlock it (unlock()) and returns again(), the
    // same call once more; else res (Cancelled once interrupted).
    template <typename Again>
    Keyring::Result afterUnlock(Keyring& k, Keyring::Result res, const std::string& origin, const std::string& id,
                                Prompt prompt, Again again) const;
    // The desktop's unlock prompt, unlockTimeoutMs_ at most. True once unlocked.
    bool unlock(Keyring& k, const std::string& origin, const std::string& id) const;
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

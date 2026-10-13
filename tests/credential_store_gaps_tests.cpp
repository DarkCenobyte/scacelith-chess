// The credential store and its keyrings (net/credential_store.h), the paths tests/net_tests.cpp
// leaves out: a credentials file that is not JSON (ignored, then written again), an origin move
// whose former keyring item cannot be removed (the session still moves), and on Linux the Secret
// Service keyring without a session bus (every call says so, libsecret is not even called) and
// with a bus address where nobody listens. The Secret Service tests need libsecret-1.so.0 and are
// skipped without it; they never reach a running keyring (no bus, or a bus nobody serves), so
// they need no desktop session and change nothing in one.
#include "test.h"
#include "headless.h"
#include "net/credential_store.h"
#include "net/json.h"
#include "net/net_sys.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace {

// A folder for a credentials file, private as the store wants it (0700).
struct PrivateDir {
    headless::TempDir dir;
    explicit PrivateDir(const char* tag) : dir(tag) {
        std::error_code ec;
        std::filesystem::permissions(dir.path, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
    }
    std::string file(const char* name) const { return dir.file(name); }
};

// A keyring in memory whose removals can be refused (a keyring that fails, or keeps the item
// locked).
class RefusingKeyring final : public net::Keyring {
public:
    bool refuseRemovals = false;
    int removes = 0;

    Result store(const std::string& origin, const std::string& id, const std::string& secret, net::CancelToken*,
                 std::string&) override {
        std::lock_guard<std::mutex> lk(m_);
        items_[{origin, id}] = secret;
        return Result::Ok;
    }
    Result lookup(const std::string& origin, const std::string& id, std::string& secret, net::CancelToken*, std::string&) override {
        std::lock_guard<std::mutex> lk(m_);
        auto it = items_.find({origin, id});
        if (it == items_.end()) return Result::Missing;
        secret = it->second;
        return Result::Ok;
    }
    Result remove(const std::string& origin, const std::string& id, net::CancelToken*, std::string& why) override {
        std::lock_guard<std::mutex> lk(m_);
        ++removes;
        if (refuseRemovals) {
            why = "removals refused (test)";
            return Result::Unavailable;
        }
        items_.erase({origin, id});
        return Result::Ok;
    }
    Result unlock(const std::string&, const std::string&, net::CancelToken*, std::string&) override { return Result::Ok; }

    int holding(const std::string& secret, const std::string& origin) {
        std::lock_guard<std::mutex> lk(m_);
        int n = 0;
        for (auto& i : items_) n += i.second == secret && i.first.first == origin;
        return n;
    }

private:
    std::mutex m_;
    std::map<std::pair<std::string, std::string>, std::string> items_;
};

// The token field of origin's record in the file, "" when there is none.
std::string fileToken(const std::string& path, const std::string& origin) {
    std::string text;
    net::json::Value doc;
    if (!net::sys::readFile(path, text, 1 << 20) || !net::json::parse(text, doc)) return std::string();
    for (const net::json::Value& r : doc["records"].items())
        if (r["origin"].asString() == origin) return r["token"].asString();
    return std::string();
}

bool writeFile(const std::string& path, const std::string& data) {
    std::FILE* f = net::sys::openFile(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace

TEST(credential_store_gaps_file_not_json_ignored) {
    // A file cut in the middle (or anything that is not JSON): no record, and the next save
    // writes a whole file again.
    PrivateDir d("cred-gaps-notjson");
    const std::string path = d.file("Scacelith.credentials");
    REQUIRE(writeFile(path, "{\"version\":1,\"records\":[{\"origin\":\"a.example.org:443\",\"username\":\"al"));
    RefusingKeyring k;
    {
        net::CredentialStore s(path);
        s.setKeyring(&k);
        CHECK(s.origins().empty());
        CHECK(!s.hasToken("a.example.org:443"));
        net::Credential out;
        CHECK(!s.get("a.example.org:443", out));
        net::Credential c;
        c.origin = "b.example.org:443";
        c.username = "bob";
        c.token = "sct_" + std::string(43, 'B');
        CHECK(s.put(c));
    }
    net::CredentialStore again(path);
    again.setKeyring(&k);
    net::Credential out;
    CHECK(again.get("b.example.org:443", out));
    CHECK_EQ(out.username, std::string("bob"));
    CHECK_EQ(out.token, "sct_" + std::string(43, 'B'));
}

TEST(credential_store_gaps_moved_item_not_removed) {
    // An origin move with the token in the keyring: at its first read the token goes to an item
    // of the new origin. When the keyring refuses to remove the former item, the session still
    // moves (the log says the copy stays).
    PrivateDir d("cred-gaps-move");
    const std::string path = d.file("Scacelith.credentials");
    const std::string oldO = "play.example.org:44664", newO = "play.example.org:443";
    const std::string token = "sct_" + std::string(43, 'M');
    RefusingKeyring k;
    {
        net::CredentialStore s(path);
        s.setKeyring(&k);
        net::Credential c;
        c.origin = oldO;
        c.username = "alice";
        c.token = token;
        CHECK(s.put(c));
    }
    const std::string ref = fileToken(path, oldO);
    CHECK_EQ(ref.compare(0, 8, "keyring:"), 0);
    k.refuseRemovals = true;
    {
        net::CredentialStore s(path);
        s.setKeyring(&k);
        s.addOriginMove(oldO, newO);
        net::Credential out;
        CHECK(s.get(newO, out));
        CHECK_EQ(out.token, token);
        const std::string moved = fileToken(path, newO);
        CHECK(moved.compare(0, 8, "keyring:") == 0 && moved != ref);
        CHECK_EQ(k.holding(token, newO), 1);
        CHECK_EQ(k.holding(token, oldO), 1);   // could not be removed
        CHECK(k.removes >= 1);
    }
}

#if !defined(_WIN32) && !defined(__APPLE__)
namespace {

// Sets (or, with nullptr, unsets) an environment variable for a test's scope, restored after it.
struct ScopedEnv {
    std::string name, saved;
    bool had;
    ScopedEnv(const char* n, const char* value) : name(n) {
        const char* was = std::getenv(n);
        had = was != nullptr;
        if (had) saved = was;
        if (value) setenv(n, value, 1);
        else unsetenv(n);
    }
    ~ScopedEnv() {
        if (had) setenv(name.c_str(), saved.c_str(), 1);
        else unsetenv(name.c_str());
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
};

const char kOrigin[] = "secret-service-gaps.invalid:443";

// Every call of k, for an item and (unlock) for the collection, gives Unavailable "no D-Bus session bus".
void checkNoBus(net::Keyring& k) {
    using R = net::Keyring::Result;
    std::string why, secret;
    CHECK(k.store(kOrigin, "id1", "x", nullptr, why) == R::Unavailable);
    CHECK_EQ(why, std::string("no D-Bus session bus"));
    why.clear();
    CHECK(k.lookup(kOrigin, "id1", secret, nullptr, why) == R::Unavailable);
    CHECK_EQ(why, std::string("no D-Bus session bus"));
    CHECK(secret.empty());
    why.clear();
    CHECK(k.remove(kOrigin, "id1", nullptr, why) == R::Unavailable);
    CHECK_EQ(why, std::string("no D-Bus session bus"));
    why.clear();
    CHECK(k.unlock(kOrigin, "id1", nullptr, why) == R::Unavailable);
    CHECK_EQ(why, std::string("no D-Bus session bus"));
    why.clear();
    CHECK(k.unlock(kOrigin, "", nullptr, why) == R::Unavailable);
    CHECK_EQ(why, std::string("no D-Bus session bus"));
}

}  // namespace

TEST(credential_store_gaps_secret_service_without_session_bus) {
    net::Keyring* k = net::secretServiceKeyring();
    if (!k) SKIP("libsecret-1.so.0 cannot be loaded");
    headless::TempDir dir("cred-gaps-nobus");
    ScopedEnv bus("DBUS_SESSION_BUS_ADDRESS", nullptr);
    {
        ScopedEnv runtime("XDG_RUNTIME_DIR", nullptr);
        checkNoBus(*k);
    }
    {
        ScopedEnv empty("DBUS_SESSION_BUS_ADDRESS", ""), runtime("XDG_RUNTIME_DIR", "run/user/1000");   // not absolute
        checkNoBus(*k);
    }
    {
        ScopedEnv runtime("XDG_RUNTIME_DIR", dir.path.c_str());   // no "bus" there
        checkNoBus(*k);
    }
    {
        REQUIRE(writeFile(dir.file("bus"), ""));                 // a "bus" that is not a socket
        ScopedEnv runtime("XDG_RUNTIME_DIR", dir.path.c_str());
        checkNoBus(*k);
    }
    // A store that is shutting down asks nothing, bus or not.
    net::CancelToken cancel;
    cancel.cancel();
    std::string why, secret;
    CHECK(k->lookup(kOrigin, "id1", secret, &cancel, why) == net::Keyring::Result::Cancelled);
    CHECK(k->store(kOrigin, "id1", "x", &cancel, why) == net::Keyring::Result::Cancelled);
    CHECK(k->unlock(kOrigin, "", &cancel, why) == net::Keyring::Result::Cancelled);
}

TEST(credential_store_gaps_secret_service_bus_nobody_serves) {
    // A session bus address where nothing listens: libsecret is called and fails, with GLib's
    // message. Only reads and removals of an item that cannot exist: nothing would change even
    // in a real keyring.
    net::Keyring* k = net::secretServiceKeyring();
    if (!k) SKIP("libsecret-1.so.0 cannot be loaded");
    headless::TempDir dir("cred-gaps-deadbus");
    const std::string address = "unix:path=" + dir.file("no-such-bus");
    ScopedEnv bus("DBUS_SESSION_BUS_ADDRESS", address.c_str());
    std::string why, secret;
    const net::Keyring::Result r = k->lookup(kOrigin, "no-such-id", secret, nullptr, why);
    if (r == net::Keyring::Result::Missing) SKIP("this process already holds a connection to a session bus");
    CHECK(r == net::Keyring::Result::Unavailable);
    CHECK(!why.empty() && why != "no D-Bus session bus");
    why.clear();
    CHECK(k->remove(kOrigin, "no-such-id", nullptr, why) == net::Keyring::Result::Unavailable);
    CHECK(!why.empty());
    why.clear();
    CHECK(k->unlock(kOrigin, "no-such-id", nullptr, why) == net::Keyring::Result::Unavailable);
    CHECK(!why.empty());
}
#endif

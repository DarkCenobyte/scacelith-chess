// The system keyring of a Linux build (credential_store.h, Keyring): the Secret Service
// (org.freedesktop.secrets on the D-Bus session bus: GNOME Keyring, KWallet, KeePassXC...) through
// libsecret, loaded at run time. The game is not linked with it (a release must run where it is
// missing: the store then keeps the sessions in its file), so the few types and functions used
// here are declared below, as libsecret and GLib define them (their ABI is stable).
//
// Items: schema "com.scacelith.Session" (libsecret adds it as the xdg:schema attribute), string
// attributes "origin" and "id", label "Scacelith session (<origin>)", the token as a text/plain
// secret, in the default collection (usually the "login" keyring).
//
// Never a prompt: an unlock dialog would pop up over the game at any network call, and a dismissed
// one at the next. A locked default collection, or none, is Unavailable before anything is
// stored, searches do not unlock (SECRET_SEARCH_UNLOCK is not given), and an item a locked keyring
// keeps is not removed (Unavailable). Without a D-Bus session bus (no DBUS_SESSION_BUS_ADDRESS nor
// $XDG_RUNTIME_DIR/bus) libsecret is not even called: GLib would start a bus of its own
// (dbus-launch) for an X11 display. Every call takes the store's CancelToken, which cancels the
// D-Bus call in progress (GCancellable).
#include "credential_store.h"

#ifndef _WIN32
#include "../core/log.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <string>
#include <sys/stat.h>
#endif

namespace net {

#ifdef _WIN32

Keyring* secretServiceKeyring() { return nullptr; }   // DPAPI protects the file (credential_store.cpp)

#else

namespace {

// ---- GLib, GObject and libsecret, as their headers declare them ----

using gboolean = int;
using gpointer = void*;
struct GError {
    uint32_t domain;
    int code;
    char* message;
};
struct GList {
    gpointer data;
    GList* next;
    GList* prev;
};
struct GHashTable;
struct GCancellable;
struct SecretService;
struct SecretCollection;
struct SecretItem;
struct SecretValue;
using GHashFunc = unsigned (*)(const void*);
using GEqualFunc = gboolean (*)(const void*, const void*);

struct SecretSchemaAttribute {
    const char* name;
    int type;   // SECRET_SCHEMA_ATTRIBUTE_STRING = 0
};
struct SecretSchema {
    const char* name;
    int flags;   // SECRET_SCHEMA_NONE = 0: the name is matched (xdg:schema)
    SecretSchemaAttribute attributes[32];
    int reserved;
    gpointer reserved1, reserved2, reserved3, reserved4, reserved5, reserved6, reserved7;
};

constexpr int kServiceNone = 0;               // SECRET_SERVICE_NONE
constexpr int kCollectionNone = 0;            // SECRET_COLLECTION_NONE
constexpr int kSearchNone = 0;                // SECRET_SEARCH_NONE (the first item, locked or not)
constexpr int kSearchLoadSecrets = 1 << 3;    // SECRET_SEARCH_LOAD_SECRETS (without UNLOCK: no prompt)
const char kDefaultCollection[] = "default";  // SECRET_COLLECTION_DEFAULT

const SecretSchema kSchema = {"com.scacelith.Session", 0, {{"origin", 0}, {"id", 0}, {nullptr, 0}}, 0,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

struct Api {
    SecretService* (*serviceGetSync)(int flags, GCancellable*, GError**);
    SecretCollection* (*collectionForAliasSync)(SecretService*, const char* alias, int flags, GCancellable*, GError**);
    gboolean (*collectionGetLocked)(SecretCollection*);
    gboolean (*serviceStoreSync)(SecretService*, const SecretSchema*, GHashTable* attributes, const char* collection,
                                 const char* label, SecretValue*, GCancellable*, GError**);
    GList* (*serviceSearchSync)(SecretService*, const SecretSchema*, GHashTable* attributes, int flags, GCancellable*, GError**);
    gboolean (*serviceClearSync)(SecretService*, const SecretSchema*, GHashTable* attributes, GCancellable*, GError**);
    gboolean (*itemGetLocked)(SecretItem*);
    SecretValue* (*itemGetSecret)(SecretItem*);
    SecretValue* (*valueNew)(const char* secret, ptrdiff_t length, const char* contentType);
    const char* (*valueGet)(SecretValue*, size_t* length);
    void (*valueUnref)(gpointer);
    // GLib and GObject: libsecret's own dependencies, found through its handle.
    GHashTable* (*hashTableNew)(GHashFunc, GEqualFunc);
    gboolean (*hashTableInsert)(GHashTable*, gpointer key, gpointer value);
    void (*hashTableUnref)(GHashTable*);
    GHashFunc strHash;
    GEqualFunc strEqual;
    void (*listFree)(GList*);
    void (*objectUnref)(gpointer);
    void (*errorFree)(GError*);
    GCancellable* (*cancellableNew)();
    void (*cancellableCancel)(GCancellable*);
};

template <typename F>
bool symbol(void* lib, F& fn, const char* name) {
    fn = reinterpret_cast<F>(dlsym(lib, name));
    return fn != nullptr;
}

const Api* api() {
    static const Api* const loaded = []() -> const Api* {
        // Never closed: GLib cannot be unloaded (its type system, the GDBus worker thread).
        void* lib = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            LOGI("net: libsecret-1.so.0 not found: no system keyring");
            return nullptr;
        }
        static Api a;
        const bool ok =
            symbol(lib, a.serviceGetSync, "secret_service_get_sync") &&
            symbol(lib, a.collectionForAliasSync, "secret_collection_for_alias_sync") &&
            symbol(lib, a.collectionGetLocked, "secret_collection_get_locked") &&
            symbol(lib, a.serviceStoreSync, "secret_service_store_sync") &&
            symbol(lib, a.serviceSearchSync, "secret_service_search_sync") &&
            symbol(lib, a.serviceClearSync, "secret_service_clear_sync") &&
            symbol(lib, a.itemGetLocked, "secret_item_get_locked") && symbol(lib, a.itemGetSecret, "secret_item_get_secret") &&
            symbol(lib, a.valueNew, "secret_value_new") && symbol(lib, a.valueGet, "secret_value_get") &&
            symbol(lib, a.valueUnref, "secret_value_unref") && symbol(lib, a.hashTableNew, "g_hash_table_new") &&
            symbol(lib, a.hashTableInsert, "g_hash_table_insert") && symbol(lib, a.hashTableUnref, "g_hash_table_unref") &&
            symbol(lib, a.strHash, "g_str_hash") && symbol(lib, a.strEqual, "g_str_equal") &&
            symbol(lib, a.listFree, "g_list_free") && symbol(lib, a.objectUnref, "g_object_unref") &&
            symbol(lib, a.errorFree, "g_error_free") && symbol(lib, a.cancellableNew, "g_cancellable_new") &&
            symbol(lib, a.cancellableCancel, "g_cancellable_cancel");
        if (!ok) {
            LOGW("net: libsecret-1.so.0 lacks a function: no system keyring");
            return nullptr;
        }
        return &a;
    }();
    return loaded;
}

// Whether GLib would find a session bus without starting one (dbus-launch).
bool sessionBus() {
    const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    if (address && *address) return true;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') return false;
    struct stat st;
    return stat((std::string(runtime) + "/bus").c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

// One keyring call: the service, the attributes of the item, and a GCancellable that the store's
// CancelToken cancels. The results of the calls are checked by the caller; everything here is
// released at the end.
class Call {
public:
    Call(const Api& a, CancelToken* cancel, const std::string& origin, const std::string& id)
        : a_(a), origin_(origin), id_(id), cancellable_(a.cancellableNew()), guard_(cancel, [this] { a_.cancellableCancel(cancellable_); }),
          cancel_(cancel) {
        attrs_ = a_.hashTableNew(a_.strHash, a_.strEqual);
        a_.hashTableInsert(attrs_, const_cast<char*>("origin"), const_cast<char*>(origin_.c_str()));
        a_.hashTableInsert(attrs_, const_cast<char*>("id"), const_cast<char*>(id_.c_str()));
    }
    ~Call() {
        guard_.clear();
        if (collection_) a_.objectUnref(collection_);
        if (service_) a_.objectUnref(service_);
        a_.hashTableUnref(attrs_);
        a_.objectUnref(cancellable_);
        if (error_) a_.errorFree(error_);
    }
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;

    GCancellable* cancellable() const { return cancellable_; }
    GHashTable* attributes() const { return attrs_; }
    GError** error() {
        if (error_) a_.errorFree(error_);
        error_ = nullptr;
        return &error_;
    }
    bool cancelled() const { return cancel_ && cancel_->cancelled(); }
    // The outcome of a failed step: Cancelled once the store is interrupted, else Unavailable with
    // GLib's message (or 'fallback').
    Keyring::Result failed(std::string& why, const char* fallback) const {
        if (cancelled()) return Keyring::Result::Cancelled;
        why = error_ && error_->message ? error_->message : fallback;
        return Keyring::Result::Unavailable;
    }
    // Connects to the service (D-Bus activation starts the keyring daemon if needed).
    bool open(std::string& why, Keyring::Result& res) {
        if (cancelled()) { res = Keyring::Result::Cancelled; return false; }
        if (!sessionBus()) {
            why = "no D-Bus session bus";
            res = Keyring::Result::Unavailable;
            return false;
        }
        service_ = a_.serviceGetSync(kServiceNone, cancellable_, error());
        if (!service_) { res = failed(why, "no Secret Service"); return false; }
        return true;
    }
    // The default collection, unlocked (it is never unlocked here: that would prompt).
    bool openDefaultCollection(std::string& why, Keyring::Result& res) {
        collection_ = a_.collectionForAliasSync(service_, kDefaultCollection, kCollectionNone, cancellable_, error());
        if (!collection_) { res = failed(why, "no default keyring"); return false; }
        if (a_.collectionGetLocked(collection_)) {
            why = "the default keyring is locked";
            res = Keyring::Result::Unavailable;
            return false;
        }
        return true;
    }
    SecretService* service() const { return service_; }

private:
    const Api& a_;
    const std::string origin_, id_;
    GCancellable* cancellable_;
    AbortGuard guard_;
    CancelToken* cancel_;
    GHashTable* attrs_ = nullptr;
    SecretService* service_ = nullptr;
    SecretCollection* collection_ = nullptr;
    GError* error_ = nullptr;
};

class SecretServiceKeyring final : public Keyring {
public:
    explicit SecretServiceKeyring(const Api& a) : a_(a) {}

    Result store(const std::string& origin, const std::string& id, const std::string& secret, CancelToken* cancel,
                 std::string& why) override {
        Call call(a_, cancel, origin, id);
        Result res = Result::Ok;
        if (!call.open(why, res) || !call.openDefaultCollection(why, res)) return res;
        SecretValue* value = a_.valueNew(secret.data(), ptrdiff_t(secret.size()), "text/plain");
        const std::string label = "Scacelith session (" + origin + ")";
        const bool ok = a_.serviceStoreSync(call.service(), &kSchema, call.attributes(), kDefaultCollection, label.c_str(), value,
                                            call.cancellable(), call.error());
        a_.valueUnref(value);
        return ok ? Result::Ok : call.failed(why, "not stored");
    }

    Result lookup(const std::string& origin, const std::string& id, std::string& secret, CancelToken* cancel,
                  std::string& why) override {
        secret.clear();
        Call call(a_, cancel, origin, id);
        Result res = Result::Ok;
        if (!call.open(why, res)) return res;
        GError** err = call.error();
        GList* items = a_.serviceSearchSync(call.service(), &kSchema, call.attributes(), kSearchLoadSecrets, call.cancellable(), err);
        if (*err) return call.failed(why, "search failed");
        if (!items) return Result::Missing;
        SecretItem* item = static_cast<SecretItem*>(items->data);
        SecretValue* value = a_.itemGetLocked(item) ? nullptr : a_.itemGetSecret(item);
        if (value) {
            size_t n = 0;
            const char* p = a_.valueGet(value, &n);
            if (p) secret.assign(p, n);
            a_.valueUnref(value);
        } else {
            res = Result::Unavailable;
            why = a_.itemGetLocked(item) ? "the keyring is locked" : "the secret could not be read";
        }
        for (GList* l = items; l; l = l->next) a_.objectUnref(l->data);
        a_.listFree(items);
        if (res == Result::Ok && secret.empty()) {
            res = Result::Unavailable;
            why = "empty secret";
        }
        return res;
    }

    Result remove(const std::string& origin, const std::string& id, CancelToken* cancel, std::string& why) override {
        Call call(a_, cancel, origin, id);
        Result res = Result::Ok;
        if (!call.open(why, res)) return res;
        GError** err = call.error();
        a_.serviceClearSync(call.service(), &kSchema, call.attributes(), call.cancellable(), err);   // FALSE: nothing removed
        if (*err) return call.failed(why, "not removed");
        // secret_service_clear removes the unlocked items only, and says nothing of a locked one
        // (FALSE, no error, as when nothing matched): an item still found is one a locked keyring
        // keeps.
        err = call.error();
        GList* items = a_.serviceSearchSync(call.service(), &kSchema, call.attributes(), kSearchNone, call.cancellable(), err);
        if (*err) return call.failed(why, "not removed");
        if (!items) return Result::Ok;
        why = a_.itemGetLocked(static_cast<SecretItem*>(items->data)) ? "the keyring is locked" : "not removed";
        for (GList* l = items; l; l = l->next) a_.objectUnref(l->data);
        a_.listFree(items);
        return Result::Unavailable;
    }

private:
    const Api& a_;
};

}  // namespace

Keyring* secretServiceKeyring() {
    // Never destroyed: the static objects of the process are destroyed at exit in the reverse order
    // of their construction, and the online client (net::onlineClient(), made before the first
    // keyring call) is destroyed after this one, while its network threads may still be in a
    // keyring call (its destructor interrupts them, then waits for them).
    static Keyring* const keyring = []() -> Keyring* {
        const Api* a = api();
        return a ? new SecretServiceKeyring(*a) : nullptr;
    }();
    return keyring;
}

#endif

}  // namespace net

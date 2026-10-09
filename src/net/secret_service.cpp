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
// A prompt only from unlock(), which the store calls when it chooses to (credential_store.h: a
// sign-in, a token needed now, a removal; not again once declined): the other calls never prompt. A
// locked default collection is Locked before anything is stored (CreateItem would prompt), searches
// do not unlock (SECRET_SEARCH_UNLOCK is not given), and an item a locked keyring keeps is neither
// read nor removed (Locked); a missing default collection is Unavailable. Without a D-Bus session
// bus (no DBUS_SESSION_BUS_ADDRESS nor $XDG_RUNTIME_DIR/bus) libsecret is not even called: GLib
// would start a bus of its own (dbus-launch) for an X11 display. Every call takes the store's
// CancelToken, which cancels the D-Bus call in progress (GCancellable), or stops waiting for the
// prompt (left to the player: see Call::unlock).
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
struct GMainContext;
struct GObject;
struct GAsyncResult;
struct SecretService;
struct SecretCollection;
struct SecretItem;
struct SecretValue;
using GHashFunc = unsigned (*)(const void*);
using GEqualFunc = gboolean (*)(const void*, const void*);
using GAsyncReadyCallback = void (*)(GObject* source, GAsyncResult* result, gpointer userData);

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
constexpr int kServiceUnknown = 2;            // G_DBUS_ERROR_SERVICE_UNKNOWN
constexpr int kNameHasNoOwner = 3;            // G_DBUS_ERROR_NAME_HAS_NO_OWNER

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
    // objects: the items or collections (GDBusProxy) to unlock. The result: how many were, -1 on error.
    void (*serviceUnlock)(SecretService*, GList* objects, GCancellable*, GAsyncReadyCallback, gpointer userData);
    int (*serviceUnlockFinish)(SecretService*, GAsyncResult*, GList** unlocked, GError**);
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
    uint32_t (*dbusErrorQuark)();
    GMainContext* (*mainContextNew)();
    void (*mainContextUnref)(GMainContext*);
    void (*mainContextPushThreadDefault)(GMainContext*);
    void (*mainContextPopThreadDefault)(GMainContext*);
    gboolean (*mainContextIteration)(GMainContext*, gboolean mayBlock);
    void (*mainContextWakeup)(GMainContext*);
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
            LOGI("net: libsecret-1.so.0 not found: no system keyring (it comes with libsecret-1-0 on Debian and Ubuntu, "
                 "libsecret on Fedora and Arch)");
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
            symbol(lib, a.serviceUnlock, "secret_service_unlock") &&
            symbol(lib, a.serviceUnlockFinish, "secret_service_unlock_finish") &&
            symbol(lib, a.itemGetLocked, "secret_item_get_locked") && symbol(lib, a.itemGetSecret, "secret_item_get_secret") &&
            symbol(lib, a.valueNew, "secret_value_new") && symbol(lib, a.valueGet, "secret_value_get") &&
            symbol(lib, a.valueUnref, "secret_value_unref") && symbol(lib, a.hashTableNew, "g_hash_table_new") &&
            symbol(lib, a.hashTableInsert, "g_hash_table_insert") && symbol(lib, a.hashTableUnref, "g_hash_table_unref") &&
            symbol(lib, a.strHash, "g_str_hash") && symbol(lib, a.strEqual, "g_str_equal") &&
            symbol(lib, a.listFree, "g_list_free") && symbol(lib, a.objectUnref, "g_object_unref") &&
            symbol(lib, a.errorFree, "g_error_free") && symbol(lib, a.cancellableNew, "g_cancellable_new") &&
            symbol(lib, a.cancellableCancel, "g_cancellable_cancel") && symbol(lib, a.dbusErrorQuark, "g_dbus_error_quark") &&
            symbol(lib, a.mainContextNew, "g_main_context_new") && symbol(lib, a.mainContextUnref, "g_main_context_unref") &&
            symbol(lib, a.mainContextPushThreadDefault, "g_main_context_push_thread_default") &&
            symbol(lib, a.mainContextPopThreadDefault, "g_main_context_pop_thread_default") &&
            symbol(lib, a.mainContextIteration, "g_main_context_iteration") &&
            symbol(lib, a.mainContextWakeup, "g_main_context_wakeup");
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

// The end of secret_service_unlock, on the main context of the call that started it.
struct Unlocking {
    const Api* a;
    bool done = false;
    int count = -1;
    GError* error = nullptr;
};

void unlocked(GObject* source, GAsyncResult* result, gpointer data) {
    Unlocking* u = static_cast<Unlocking*>(data);
    u->count = u->a->serviceUnlockFinish(reinterpret_cast<SecretService*>(source), result, nullptr, &u->error);
    u->done = true;
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
    // GLib's message (or 'fallback'), which says "no Secret Service" when no program provides it on
    // the session bus (none installed, or none that D-Bus can start).
    Keyring::Result failed(std::string& why, const char* fallback) const {
        if (cancelled()) return Keyring::Result::Cancelled;
        why = error_ && error_->message ? error_->message : fallback;
        if (error_ && error_->domain == a_.dbusErrorQuark() && (error_->code == kServiceUnknown || error_->code == kNameHasNoOwner))
            why = "no Secret Service (" + why + ")";
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
    // The default collection (Locked when it is: only unlock() opens it).
    bool openDefaultCollection(std::string& why, Keyring::Result& res) {
        collection_ = a_.collectionForAliasSync(service_, kDefaultCollection, kCollectionNone, cancellable_, error());
        if (!collection_) { res = failed(why, "no default keyring"); return false; }
        if (a_.collectionGetLocked(collection_)) {
            why = "the default keyring is locked";
            res = Keyring::Result::Locked;
            return false;
        }
        return true;
    }
    SecretService* service() const { return service_; }
    SecretCollection* collection() const { return collection_; }

    // Asks the service to unlock object (an item or a collection): it shows the desktop's prompt.
    // The answer is awaited on a main context of this thread's own, which the store's token wakes
    // (its timeout, interrupt()): the call is then given up, left pending on that context, which
    // nothing runs again. The prompt is not withdrawn but left to the player (answered, it still
    // unlocks the keyring): its Dismiss, which libsecret sends when the call's GCancellable is
    // cancelled, makes gnome-keyring-daemon abort while it shows the prompt (an assertion of
    // gkd-secret-unlock.c, in every release up to 48 at least). So no GCancellable here.
    Keyring::Result unlock(gpointer object, std::string& why) {
        guard_.clear();   // the token now wakes this thread instead
        if (cancelled()) return Keyring::Result::Cancelled;
        GMainContext* context = a_.mainContextNew();
        a_.mainContextPushThreadDefault(context);
        AbortGuard wake(cancel_, [this, context] { a_.mainContextWakeup(context); });
        Unlocking* u = new Unlocking{&a_};
        GList objects{object, nullptr, nullptr};
        a_.serviceUnlock(service_, &objects, nullptr, unlocked, u);
        while (!u->done && !cancelled()) a_.mainContextIteration(context, true);   // an answer, or the token
        wake.clear();
        if (!u->done) {
            // Given up: u stays with the pending call (whose callback will never run).
            a_.mainContextPopThreadDefault(context);
            a_.mainContextUnref(context);
            why = "interrupted";
            return Keyring::Result::Cancelled;
        }
        while (a_.mainContextIteration(context, false)) {
        }   // what the call left (as libsecret's own sync calls do)
        a_.mainContextPopThreadDefault(context);
        a_.mainContextUnref(context);
        Keyring::Result res = Keyring::Result::Ok;
        if (u->count > 0) {
            res = Keyring::Result::Ok;
        } else {
            // No error, nothing unlocked: the player dismissed the prompt (or the desktop has no
            // prompter: gnome-keyring then dismisses it at once).
            why = u->error && u->error->message ? u->error->message : "the unlock prompt was dismissed";
            res = Keyring::Result::Unavailable;
        }
        if (u->error) a_.errorFree(u->error);
        delete u;
        return res;
    }

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
        } else if (a_.itemGetLocked(item)) {
            res = Result::Locked;
            why = "the keyring is locked";
        } else {
            res = Result::Unavailable;
            why = "the secret could not be read";
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
        const bool locked = a_.itemGetLocked(static_cast<SecretItem*>(items->data));
        why = locked ? "the keyring is locked" : "not removed";
        for (GList* l = items; l; l = l->next) a_.objectUnref(l->data);
        a_.listFree(items);
        return locked ? Result::Locked : Result::Unavailable;
    }

    Result unlock(const std::string& origin, const std::string& id, CancelToken* cancel, std::string& why) override {
        Call call(a_, cancel, origin, id);
        Result res = Result::Ok;
        if (!call.open(why, res)) return res;
        if (id.empty()) {
            // The collection a new item goes to.
            if (call.openDefaultCollection(why, res)) return Result::Ok;   // not locked
            if (res != Result::Locked) return res;
            return call.unlock(call.collection(), why);
        }
        GError** err = call.error();
        GList* items = a_.serviceSearchSync(call.service(), &kSchema, call.attributes(), kSearchNone, call.cancellable(), err);
        if (*err) return call.failed(why, "search failed");
        if (!items) return Result::Missing;
        SecretItem* item = static_cast<SecretItem*>(items->data);
        // Unlocking the item opens its collection (the Secret Service may not unlock items alone).
        res = a_.itemGetLocked(item) ? call.unlock(item, why) : Result::Ok;
        for (GList* l = items; l; l = l->next) a_.objectUnref(l->data);
        a_.listFree(items);
        return res;
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

// The system keyring of a macOS build (credential_store.h, Keyring): the user's keychain, through
// the Security framework's generic password items.
//
// Items: class kSecClassGenericPassword, service "Scacelith", account "<origin> <id>" (both in the
// item's primary key: a reference copied into another origin's record finds nothing), label
// "Scacelith session (<origin>)", the token as the item's data. They go to the default keychain
// (usually "login"), a file-based keychain: the data protection keychain needs an application
// identifier entitlement, which an ad-hoc signed app does not have.
//
// No call prompts but unlock(): the others run with the keychain's user interface disallowed
// (SecKeychainSetUserInteractionAllowed), so that a locked keychain, or an item whose access list
// does not name this build of the game (an ad-hoc signature names one build only: an item that an
// earlier version made), gives errSecInteractionNotAllowed: Locked. unlock() lets the system ask:
// its prompt to unlock the keychain, or to let the game use the item ("Always Allow" adds this build
// to the item's access list). "Allow" lets the game read the item once: the token that prompt
// gave is kept for the lookup the store makes right after it (CredentialStore::afterUnlock), and
// for nothing else. The prompt runs on a thread of its own, given up when the store's token fires
// (its deadline, interrupt()): the prompt stays on the screen, and answered later it still unlocks
// the keychain or adds the game to the item's access list.
#include "credential_store.h"

#ifdef __APPLE__
#include "../core/log.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace net {

namespace {

const char kService[] = "Scacelith";

// A Core Foundation object, released at the end of its scope.
template <typename T>
class Cf {
public:
    explicit Cf(T ref = nullptr) : ref_(ref) {}
    ~Cf() {
        if (ref_) CFRelease(ref_);
    }
    Cf(const Cf&) = delete;
    Cf& operator=(const Cf&) = delete;
    T get() const { return ref_; }
    T* out() { return &ref_; }   // for a Copy function's result (empty before)

private:
    T ref_;
};

CFStringRef cfString(const std::string& s) {
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(s.data()), CFIndex(s.size()),
                                   kCFStringEncodingUTF8, false);
}

// The Security framework's message for status, and its number.
std::string message(OSStatus status) {
    Cf<CFStringRef> text(SecCopyErrorMessageString(status, nullptr));
    char buf[512];
    std::string s = "OSStatus " + std::to_string(int(status));
    if (text.get() && CFStringGetCString(text.get(), buf, sizeof buf, kCFStringEncodingUTF8)) s = std::string(buf) + " (" + s + ")";
    return s;
}

// What a keychain call gave, for the store.
Keyring::Result outcome(OSStatus status, std::string& why) {
    switch (status) {
    case errSecSuccess: return Keyring::Result::Ok;
    case errSecItemNotFound: return Keyring::Result::Missing;
    case errSecInteractionNotAllowed:
        why = "the keychain is locked, or the item does not let this build of the game read it without asking";
        return Keyring::Result::Locked;
    case errSecUserCanceled:
    case errSecAuthFailed:
        why = "the keychain prompt was dismissed (" + message(status) + ")";
        return Keyring::Result::Unavailable;
    default: why = message(status); return Keyring::Result::Unavailable;
    }
}

// The query of the item (origin, id): its class, service and account. nullptr when it cannot be
// made (out of memory).
CFMutableDictionaryRef itemQuery(const std::string& origin, const std::string& id) {
    Cf<CFStringRef> service(cfString(kService)), account(cfString(origin + " " + id));
    if (!service.get() || !account.get()) return nullptr;
    CFMutableDictionaryRef q =
        CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!q) return nullptr;
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service.get());
    CFDictionarySetValue(q, kSecAttrAccount, account.get());
    return q;
}

// The secret of the item (origin, id), as SecItemCopyMatching gives it.
OSStatus copySecret(const std::string& origin, const std::string& id, std::string& secret) {
    secret.clear();
    Cf<CFMutableDictionaryRef> q(itemQuery(origin, id));
    if (!q.get()) return errSecAllocate;
    CFDictionarySetValue(q.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q.get(), kSecMatchLimit, kSecMatchLimitOne);
    Cf<CFTypeRef> data;
    const OSStatus status = SecItemCopyMatching(q.get(), data.out());
    if (status != errSecSuccess) return status;
    if (!data.get() || CFGetTypeID(data.get()) != CFDataGetTypeID()) return errSecDecode;
    CFDataRef bytes = static_cast<CFDataRef>(data.get());
    secret.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)), size_t(CFDataGetLength(bytes)));
    return errSecSuccess;
}

// The legacy keychain's user interface, which SecItem calls on a file-based keychain obey. These
// calls are deprecated since macOS 10.10 (the data protection keychain has none), and still the
// only way to keep those calls from prompting.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

std::mutex g_quietMu;   // one quiet call at a time: the setting is the process's

// fn with the keychain's user interface disallowed: a call that would prompt fails with
// errSecInteractionNotAllowed instead. The previous setting is restored after.
OSStatus quietly(const std::function<OSStatus()>& fn) {
    std::lock_guard<std::mutex> lk(g_quietMu);
    Boolean was = true;
    if (SecKeychainGetUserInteractionAllowed(&was) != errSecSuccess) was = true;
    SecKeychainSetUserInteractionAllowed(false);
    const OSStatus status = fn();
    SecKeychainSetUserInteractionAllowed(was);
    return status;
}

// Whether the default keychain (where new items go) is unlocked; errSecSuccess and *keychain (to
// release) when there is one.
OSStatus defaultKeychain(SecKeychainRef* keychain, bool& unlocked) {
    unlocked = false;
    *keychain = nullptr;
    OSStatus status = SecKeychainCopyDefault(keychain);
    if (status != errSecSuccess || !*keychain) return status != errSecSuccess ? status : errSecNoDefaultKeychain;
    SecKeychainStatus state = 0;
    status = SecKeychainGetStatus(*keychain, &state);
    unlocked = status == errSecSuccess && (state & kSecUnlockStateStatus) != 0;
    return errSecSuccess;
}

// Shows the system's prompt to unlock keychain (the password of the keychain, asked by macOS).
OSStatus unlockPrompt(SecKeychainRef keychain) { return SecKeychainUnlock(keychain, 0, nullptr, false); }

#pragma clang diagnostic pop

// A call that may show a prompt, on a thread of its own (prompted: its status and the secret it
// read). Waits for it until cancel fires: Cancelled then, and the thread finishes alone (what it
// gives is dropped).
struct Prompted {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    OSStatus status = errSecSuccess;
    std::string secret;
};

bool runPrompt(CancelToken* cancel, std::function<OSStatus(std::string&)> fn, OSStatus& status, std::string& secret) {
    auto p = std::make_shared<Prompted>();
    std::thread([p, fn = std::move(fn)] {
        std::string s;
        const OSStatus st = fn(s);
        std::lock_guard<std::mutex> lk(p->mu);
        p->status = st;
        p->secret.swap(s);
        p->done = true;
        p->cv.notify_all();
    }).detach();
    AbortGuard wake(cancel, [p] {
        std::lock_guard<std::mutex> lk(p->mu);
        p->cv.notify_all();
    });
    std::unique_lock<std::mutex> lk(p->mu);
    p->cv.wait(lk, [&] { return p->done || (cancel && cancel->cancelled()); });
    if (!p->done) return false;
    status = p->status;
    secret.swap(p->secret);
    return true;
}

void wipe(std::string& s) {
    volatile char* c = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; i < s.size(); ++i) c[i] = 0;
    s.clear();
}

class KeychainKeyring final : public Keyring {
public:
    Result store(const std::string& origin, const std::string& id, const std::string& secret, CancelToken* cancel,
                 std::string& why) override {
        if (cancel && cancel->cancelled()) return Result::Cancelled;
        Cf<CFMutableDictionaryRef> item(itemQuery(origin, id));
        Cf<CFStringRef> label(cfString("Scacelith session (" + origin + ")"));
        Cf<CFDataRef> value(CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(secret.data()), CFIndex(secret.size())));
        if (!item.get() || !label.get() || !value.get()) return outcome(errSecAllocate, why);
        CFDictionarySetValue(item.get(), kSecAttrLabel, label.get());
        CFDictionarySetValue(item.get(), kSecValueData, value.get());
        const OSStatus status = quietly([&] {
            OSStatus st = SecItemAdd(item.get(), nullptr);
            if (st != errSecDuplicateItem) return st;
            // The same origin and id: its data replaced.
            Cf<CFMutableDictionaryRef> query(itemQuery(origin, id));
            Cf<CFMutableDictionaryRef> change(
                CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
            if (!query.get() || !change.get()) return OSStatus(errSecAllocate);
            CFDictionarySetValue(change.get(), kSecValueData, value.get());
            return SecItemUpdate(query.get(), change.get());
        });
        if (status == errSecItemNotFound) return outcome(errSecDuplicateItem, why);   // gone between the two calls
        return outcome(status, why);
    }

    Result lookup(const std::string& origin, const std::string& id, std::string& secret, CancelToken* cancel,
                  std::string& why) override {
        secret.clear();
        if (cancel && cancel->cancelled()) return Result::Cancelled;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (promptedOrigin_ == origin && promptedId_ == id && !promptedSecret_.empty()) {
                secret.swap(promptedSecret_);   // the read the prompt allowed (see the note above)
                forgetPromptedLocked();
                return Result::Ok;
            }
            forgetPromptedLocked();
        }
        const OSStatus status = quietly([&] { return copySecret(origin, id, secret); });
        Result res = outcome(status, why);
        if (res == Result::Ok && secret.empty()) {
            why = "empty secret";
            res = Result::Unavailable;
        }
        if (res != Result::Ok) wipe(secret);
        return res;
    }

    Result remove(const std::string& origin, const std::string& id, CancelToken* cancel, std::string& why) override {
        if (cancel && cancel->cancelled()) return Result::Cancelled;
        Cf<CFMutableDictionaryRef> q(itemQuery(origin, id));
        if (!q.get()) return outcome(errSecAllocate, why);
        const OSStatus status = quietly([&] { return SecItemDelete(q.get()); });
        if (status == errSecItemNotFound) return Result::Ok;
        return outcome(status, why);
    }

    Result unlock(const std::string& origin, const std::string& id, CancelToken* cancel, std::string& why) override {
        if (cancel && cancel->cancelled()) return Result::Cancelled;
        OSStatus status = errSecSuccess;
        std::string secret;
        if (id.empty()) {
            // The keychain new items go to.
            SecKeychainRef keychain = nullptr;
            bool unlocked = false;
            status = defaultKeychain(&keychain, unlocked);
            if (status != errSecSuccess) return outcome(status, why);
            if (unlocked) {
                CFRelease(keychain);
                return Result::Ok;
            }
            // The thread owns the reference from now on.
            if (!runPrompt(cancel,
                           [keychain](std::string&) {
                               const OSStatus st = unlockPrompt(keychain);
                               CFRelease(keychain);
                               return st;
                           },
                           status, secret)) {
                why = "interrupted";
                return Result::Cancelled;
            }
            return outcome(status, why);
        }
        // The item: reading it asks to unlock its keychain, or to let this build use it.
        if (!runPrompt(cancel, [origin, id](std::string& s) { return copySecret(origin, id, s); }, status, secret)) {
            why = "interrupted";
            return Result::Cancelled;
        }
        const Result res = outcome(status, why);
        std::lock_guard<std::mutex> lk(mu_);
        forgetPromptedLocked();
        if (res == Result::Ok && !secret.empty()) {
            promptedOrigin_ = origin;
            promptedId_ = id;
            promptedSecret_.swap(secret);
        }
        wipe(secret);
        return res;
    }

private:
    void forgetPromptedLocked() {
        promptedOrigin_.clear();
        promptedId_.clear();
        wipe(promptedSecret_);
    }

    std::mutex mu_;
    // The item the last unlock() read (see the note above), until the next lookup.
    std::string promptedOrigin_, promptedId_, promptedSecret_;
};

}  // namespace

Keyring* keychainKeyring() {
    // Never destroyed, as the Secret Service's (secret_service.cpp): the online client's network
    // threads may still be in a keychain call while the process's static objects go.
    static Keyring* const keyring = new KeychainKeyring();
    return keyring;
}

}  // namespace net

#endif

#include "direct_crypto.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#endif

namespace net {
namespace direct {

namespace {

void wipe(void* p, size_t n) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (n--) *v++ = 0;
}

const char kMagic[4] = {'S', 'C', 'D', 'M'};
const char* kInfo = "scacelith direct match v1";
const char* kGuestConfirmLabel = "SCDM guest confirm";
const char* kHostConfirmLabel = "SCDM host confirm";

#ifdef _WIN32
bool ok(NTSTATUS s) { return s >= 0; }

// Algorithm providers are opened once per process and never closed (they are thread-safe).
struct Providers {
    BCRYPT_ALG_HANDLE sha = nullptr, hmac = nullptr, aes = nullptr, ecdh = nullptr;
    bool valid = false;
    Providers() {
        valid = ok(BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
                ok(BCryptOpenAlgorithmProvider(&hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
                ok(BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
                ok(BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) &&
                ok(BCryptOpenAlgorithmProvider(&ecdh, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0));
    }
};
const Providers& providers() {
    static Providers p;
    return p;
}

bool bcryptHash(BCRYPT_ALG_HANDLE alg, const uint8_t* key, size_t keyLen, const uint8_t* p, size_t n, uint8_t out[32]) {
    BCRYPT_HASH_HANDLE h = nullptr;
    if (!ok(BCryptCreateHash(alg, &h, nullptr, 0, (PUCHAR)key, ULONG(keyLen), 0))) return false;
    bool good = (n == 0 || ok(BCryptHashData(h, (PUCHAR)p, ULONG(n), 0))) && ok(BCryptFinishHash(h, out, 32, 0));
    BCryptDestroyHash(h);
    return good;
}
#endif

}  // namespace

// ---- join codes ---------------------------------------------------------------------------------

std::string newJoinCode() {
    const size_t alpha = std::strlen(kCodeAlphabet);   // 31
    const unsigned limit = unsigned(256 / alpha * alpha);   // 248: rejection sampling, no modulo bias
    std::string code;
    while (code.size() < size_t(kCodeLength)) {
        uint8_t buf[32];
        if (!randomBytes(buf, sizeof buf)) return "";
        for (uint8_t b : buf)
            if (b < limit && code.size() < size_t(kCodeLength)) code += kCodeAlphabet[b % alpha];
    }
    return code;
}

bool normalizeJoinCode(const std::string& in, std::string& out) {
    out.clear();
    for (char c : in) {
        if (c == '-' || c == ' ' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (!std::strchr(kCodeAlphabet, c) || c == 0) { out.clear(); return false; }
        out += c;
        if (out.size() > size_t(kCodeLength)) { out.clear(); return false; }
    }
    if (out.size() != size_t(kCodeLength)) { out.clear(); return false; }
    return true;
}

std::string formatJoinCode(const std::string& c) {
    if (c.size() != size_t(kCodeLength)) return c;
    return c.substr(0, 4) + "-" + c.substr(4, 4) + "-" + c.substr(8, 4);
}

// ---- primitives ---------------------------------------------------------------------------------

bool randomBytes(uint8_t* out, size_t n) {
#ifdef _WIN32
    return ok(BCryptGenRandom(nullptr, out, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
    return RAND_bytes(out, int(n)) == 1;
#endif
}

bool sha256(const uint8_t* p, size_t n, uint8_t out[32]) {
#ifdef _WIN32
    return providers().valid && bcryptHash(providers().sha, nullptr, 0, p, n, out);
#else
    unsigned len = 0;
    return EVP_Digest(p, n, out, &len, EVP_sha256(), nullptr) == 1 && len == 32;
#endif
}

bool hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t n, uint8_t out[32]) {
#ifdef _WIN32
    // BCrypt refuses a zero-length HMAC key; HMAC pads keys with zeros, so one zero byte is equivalent.
    static const uint8_t zero = 0;
    if (keyLen == 0) { key = &zero; keyLen = 1; }
    return providers().valid && bcryptHash(providers().hmac, key, keyLen, msg, n, out);
#else
    unsigned len = 0;
    static const uint8_t zero = 0;
    return HMAC(EVP_sha256(), keyLen ? key : &zero, int(keyLen), n ? msg : &zero, n, out, &len) != nullptr && len == 32;
#endif
}

bool hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const uint8_t* info, size_t infoLen,
                uint8_t* out, size_t outLen) {
    if (outLen > 255 * 32) return false;
    uint8_t prk[32];
    static const uint8_t zeros[32] = {};
    if (!hmacSha256(saltLen ? salt : zeros, saltLen ? saltLen : 32, ikm, ikmLen, prk)) return false;   // extract
    std::vector<uint8_t> block;
    uint8_t t[32];
    size_t done = 0;
    for (uint8_t i = 1; done < outLen; ++i) {                                                         // expand
        block.clear();
        if (i > 1) block.insert(block.end(), t, t + 32);
        block.insert(block.end(), info, info + infoLen);
        block.push_back(i);
        if (!hmacSha256(prk, 32, block.data(), block.size(), t)) { wipe(prk, 32); return false; }
        size_t take = std::min<size_t>(32, outLen - done);
        std::memcpy(out + done, t, take);
        done += take;
    }
    wipe(prk, 32);
    wipe(t, 32);
    if (!block.empty()) wipe(block.data(), block.size());
    return true;
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= uint8_t(a[i] ^ b[i]);
    return d == 0;
}

// ---- AES-256-GCM --------------------------------------------------------------------------------

#ifdef _WIN32
struct AesGcm::State {
    BCRYPT_KEY_HANDLE key = nullptr;
    ~State() { if (key) BCryptDestroyKey(key); }
};

AesGcm::AesGcm() : s_(new State) {}
AesGcm::~AesGcm() = default;

bool AesGcm::setKey(const uint8_t key[32]) {
    if (!providers().valid) return false;
    if (s_->key) { BCryptDestroyKey(s_->key); s_->key = nullptr; }
    return ok(BCryptGenerateSymmetricKey(providers().aes, &s_->key, nullptr, 0, (PUCHAR)key, 32, 0));
}

bool AesGcm::seal(const uint8_t nonce[12], const uint8_t* aad, size_t aadLen, const uint8_t* pt, size_t ptLen, uint8_t* out) {
    if (!s_->key) return false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
    BCRYPT_INIT_AUTH_MODE_INFO(ai);
    ai.pbNonce = (PUCHAR)nonce;
    ai.cbNonce = 12;
    ai.pbAuthData = (PUCHAR)aad;
    ai.cbAuthData = ULONG(aadLen);
    ai.pbTag = out + ptLen;
    ai.cbTag = 16;
    ULONG len = 0;
    return ok(BCryptEncrypt(s_->key, (PUCHAR)pt, ULONG(ptLen), &ai, nullptr, 0, out, ULONG(ptLen), &len, 0)) && len == ptLen;
}

bool AesGcm::open(const uint8_t nonce[12], const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t inLen, uint8_t* out) {
    if (!s_->key || inLen < 16) return false;
    size_t ctLen = inLen - 16;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
    BCRYPT_INIT_AUTH_MODE_INFO(ai);
    ai.pbNonce = (PUCHAR)nonce;
    ai.cbNonce = 12;
    ai.pbAuthData = (PUCHAR)aad;
    ai.cbAuthData = ULONG(aadLen);
    ai.pbTag = (PUCHAR)(in + ctLen);
    ai.cbTag = 16;
    ULONG len = 0;
    bool good = ok(BCryptDecrypt(s_->key, (PUCHAR)in, ULONG(ctLen), &ai, nullptr, 0, out, ULONG(ctLen), &len, 0)) && len == ctLen;
    if (!good && ctLen) wipe(out, ctLen);
    return good;
}
#else
struct AesGcm::State {
    uint8_t key[32] = {};
    bool hasKey = false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    ~State() { wipe(key, 32); if (ctx) EVP_CIPHER_CTX_free(ctx); }
};

AesGcm::AesGcm() : s_(new State) {}
AesGcm::~AesGcm() = default;

bool AesGcm::setKey(const uint8_t key[32]) {
    std::memcpy(s_->key, key, 32);
    s_->hasKey = true;
    return s_->ctx != nullptr;
}

bool AesGcm::seal(const uint8_t nonce[12], const uint8_t* aad, size_t aadLen, const uint8_t* pt, size_t ptLen, uint8_t* out) {
    EVP_CIPHER_CTX* c = s_->ctx;
    if (!s_->hasKey || !c) return false;
    int len = 0;
    if (EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
        EVP_EncryptInit_ex(c, nullptr, nullptr, s_->key, nonce) != 1)
        return false;
    if (aadLen && EVP_EncryptUpdate(c, nullptr, &len, aad, int(aadLen)) != 1) return false;
    if (ptLen && EVP_EncryptUpdate(c, out, &len, pt, int(ptLen)) != 1) return false;
    if (EVP_EncryptFinal_ex(c, out + ptLen, &len) != 1) return false;
    return EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out + ptLen) == 1;
}

bool AesGcm::open(const uint8_t nonce[12], const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t inLen, uint8_t* out) {
    EVP_CIPHER_CTX* c = s_->ctx;
    if (!s_->hasKey || !c || inLen < 16) return false;
    size_t ctLen = inLen - 16;
    int len = 0;
    uint8_t tag[16];
    std::memcpy(tag, in + ctLen, 16);
    bool good = EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
                EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
                EVP_DecryptInit_ex(c, nullptr, nullptr, s_->key, nonce) == 1 &&
                (!aadLen || EVP_DecryptUpdate(c, nullptr, &len, aad, int(aadLen)) == 1) &&
                (!ctLen || EVP_DecryptUpdate(c, out, &len, in, int(ctLen)) == 1) &&
                EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1 &&
                EVP_DecryptFinal_ex(c, out + ctLen, &len) == 1;
    if (!good && ctLen) wipe(out, ctLen);
    return good;
}
#endif

// ---- ECDH P-256 ---------------------------------------------------------------------------------

#ifdef _WIN32
struct EcdhP256::State {
    BCRYPT_KEY_HANDLE key = nullptr;
    ~State() { if (key) BCryptDestroyKey(key); }
};

EcdhP256::EcdhP256() : s_(new State) {}
EcdhP256::~EcdhP256() = default;

static bool exportPublic(BCRYPT_KEY_HANDLE key, uint8_t pub[65]) {
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    ULONG len = 0;
    if (!ok(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof blob, &len, 0)) || len != sizeof blob) return false;
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    if (hdr->cbKey != 32) return false;
    pub[0] = 0x04;
    std::memcpy(pub + 1, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    return true;
}

bool EcdhP256::generate() {
    if (!providers().valid) return false;
    if (s_->key) { BCryptDestroyKey(s_->key); s_->key = nullptr; }
    if (!ok(BCryptGenerateKeyPair(providers().ecdh, &s_->key, 256, 0)) || !ok(BCryptFinalizeKeyPair(s_->key, 0))) return false;
    return exportPublic(s_->key, pub_);
}

bool EcdhP256::setKeyPair(const uint8_t d[32], const uint8_t pub[65]) {
    if (!providers().valid || pub[0] != 0x04) return false;
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96];
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    hdr->dwMagic = BCRYPT_ECDH_PRIVATE_P256_MAGIC;
    hdr->cbKey = 32;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), pub + 1, 64);
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64, d, 32);
    if (s_->key) { BCryptDestroyKey(s_->key); s_->key = nullptr; }
    bool good = ok(BCryptImportKeyPair(providers().ecdh, nullptr, BCRYPT_ECCPRIVATE_BLOB, &s_->key, blob, sizeof blob, 0));
    wipe(blob, sizeof blob);
    return good && exportPublic(s_->key, pub_);
}

bool EcdhP256::agree(const uint8_t peer[65], uint8_t secret[32]) {
    if (!s_->key || peer[0] != 0x04) return false;
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    hdr->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), peer + 1, 64);
    BCRYPT_KEY_HANDLE pk = nullptr;
    if (!ok(BCryptImportKeyPair(providers().ecdh, nullptr, BCRYPT_ECCPUBLIC_BLOB, &pk, blob, sizeof blob, 0))) return false;
    BCRYPT_SECRET_HANDLE sec = nullptr;
    bool good = ok(BCryptSecretAgreement(s_->key, pk, &sec, 0));
    ULONG len = 0;
    uint8_t raw[32];
    if (good) good = ok(BCryptDeriveKey(sec, BCRYPT_KDF_RAW_SECRET, nullptr, raw, sizeof raw, &len, 0)) && len == 32;
    if (sec) BCryptDestroySecret(sec);
    BCryptDestroyKey(pk);
    if (!good) return false;
    // BCRYPT_KDF_RAW_SECRET returns the X coordinate little-endian: the standard form is big-endian.
    for (int i = 0; i < 32; ++i) secret[i] = raw[31 - i];
    wipe(raw, 32);
    return true;
}
#else
struct EcdhP256::State {
    EVP_PKEY* key = nullptr;
    ~State() { if (key) EVP_PKEY_free(key); }
};

EcdhP256::EcdhP256() : s_(new State) {}
EcdhP256::~EcdhP256() = default;

static bool exportPublic(EVP_PKEY* key, uint8_t pub[65]) {
    size_t len = 0;
    return EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, pub, 65, &len) == 1 && len == 65 && pub[0] == 0x04;
}

// A P-256 key from its encoded public point (and the private scalar when d is given).
static EVP_PKEY* keyFromData(const uint8_t pub[65], const uint8_t* d) {
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    BIGNUM* priv = d ? BN_bin2bn(d, 32, nullptr) : nullptr;
    OSSL_PARAM* params = nullptr;
    EVP_PKEY_CTX* ctx = nullptr;
    EVP_PKEY* key = nullptr;
    if (bld && OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) == 1 &&
        OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, pub, 65) == 1 &&
        (!d || (priv && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, priv) == 1)) &&
        (params = OSSL_PARAM_BLD_to_param(bld)) != nullptr && (ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)) != nullptr &&
        EVP_PKEY_fromdata_init(ctx) == 1) {
        if (EVP_PKEY_fromdata(ctx, &key, d ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params) != 1) key = nullptr;
    }
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    BN_clear_free(priv);
    return key;
}

bool EcdhP256::generate() {
    if (s_->key) { EVP_PKEY_free(s_->key); s_->key = nullptr; }
    s_->key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    return s_->key && exportPublic(s_->key, pub_);
}

bool EcdhP256::setKeyPair(const uint8_t d[32], const uint8_t pub[65]) {
    if (s_->key) { EVP_PKEY_free(s_->key); s_->key = nullptr; }
    if (pub[0] != 0x04) return false;
    s_->key = keyFromData(pub, d);
    return s_->key && exportPublic(s_->key, pub_);
}

bool EcdhP256::agree(const uint8_t peer[65], uint8_t secret[32]) {
    if (!s_->key || peer[0] != 0x04) return false;
    EVP_PKEY* pk = keyFromData(peer, nullptr);   // decoding checks that the point is on the curve
    if (!pk) return false;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(s_->key, nullptr);
    size_t len = 32;
    bool good = ctx && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, pk) == 1 &&
                EVP_PKEY_derive(ctx, secret, &len) == 1 && len == 32;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return good;
}
#endif

// ---- SecureChannel ------------------------------------------------------------------------------

SecureChannel::SecureChannel(Role role, const std::string& code) : role_(role), code_(code) {}

SecureChannel::~SecureChannel() {
    wipe(confirmGuest_, 32);
    wipe(confirmHost_, 32);
    wipe(nonce_, 32);
}

bool SecureChannel::fail(Failure f) {
    if (status_ != Status::Failed) {
        status_ = Status::Failed;
        failure_ = f;
    }
    in_.clear();
    messages_.clear();
    return false;
}

void SecureChannel::makeHello(uint8_t out[kHelloLen]) const {
    std::memcpy(out, kMagic, 4);
    out[4] = kVersion;
    std::memcpy(out + 5, nonce_, 32);
    std::memcpy(out + 37, ecdh_.publicKey(), 65);
}

bool SecureChannel::checkHello(const uint8_t* h) {
    if (std::memcmp(h, kMagic, 4) != 0) return fail(Failure::BadHello);
    if (h[4] != kVersion) return fail(Failure::BadVersion);
    if (h[37] != 0x04) return fail(Failure::BadHello);
    // A reflected hello (our own nonce or key sent back) is refused.
    if (std::memcmp(h + 5, nonce_, 32) == 0 || std::memcmp(h + 37, ecdh_.publicKey(), 65) == 0) return fail(Failure::BadHello);
    return true;
}

bool SecureChannel::deriveKeys(const uint8_t* peerHello) {
    uint8_t z[32];
    if (!ecdh_.agree(peerHello + 37, z)) return fail(Failure::BadHello);
    uint8_t ikm[32 + kCodeLength];
    std::memcpy(ikm, z, 32);
    std::memcpy(ikm + 32, code_.data(), kCodeLength);
    uint8_t salt[64];
    std::memcpy(salt, guestHello_ + 5, 32);
    std::memcpy(salt + 32, hostHello_ + 5, 32);
    uint8_t okm[128];
    bool good = hkdfSha256(salt, sizeof salt, ikm, sizeof ikm, reinterpret_cast<const uint8_t*>(kInfo), std::strlen(kInfo), okm,
                           sizeof okm);
    if (good) {
        const uint8_t* g2h = okm;
        const uint8_t* h2g = okm + 32;
        good = sendKey_.setKey(role_ == Role::Guest ? g2h : h2g) && recvKey_.setKey(role_ == Role::Guest ? h2g : g2h);
        std::memcpy(confirmGuest_, okm + 64, 32);
        std::memcpy(confirmHost_, okm + 96, 32);
        uint8_t transcript[2 * kHelloLen];
        std::memcpy(transcript, guestHello_, kHelloLen);
        std::memcpy(transcript + kHelloLen, hostHello_, kHelloLen);
        good = good && sha256(transcript, sizeof transcript, th_);
    }
    wipe(z, sizeof z);
    wipe(ikm, sizeof ikm);
    wipe(okm, sizeof okm);
    return good ? true : fail(Failure::Crypto);
}

bool SecureChannel::makeConfirm(const uint8_t key[32], const char* label, uint8_t out[kConfirmLen]) {
    AesGcm k;
    static const uint8_t zeroNonce[12] = {};
    return k.setKey(key) && k.seal(zeroNonce, reinterpret_cast<const uint8_t*>(label), std::strlen(label), th_, 32, out);
}

bool SecureChannel::checkConfirm(const uint8_t key[32], const char* label, const uint8_t* in) {
    AesGcm k;
    static const uint8_t zeroNonce[12] = {};
    uint8_t pt[32];
    bool good = k.setKey(key) && k.open(zeroNonce, reinterpret_cast<const uint8_t*>(label), std::strlen(label), in, kConfirmLen, pt) &&
                constantTimeEqual(pt, th_, 32);
    wipe(pt, 32);
    return good;
}

bool SecureChannel::start() {
    if (step_ != Step::Init || status_ != Status::Handshake) return false;
    if (code_.size() != size_t(kCodeLength)) return fail(Failure::WrongCode);
    if (!ecdh_.generate() || !randomBytes(nonce_, 32)) return fail(Failure::Crypto);
    if (role_ == Role::Guest) {
        makeHello(guestHello_);
        out_.insert(out_.end(), guestHello_, guestHello_ + kHelloLen);
    }
    step_ = Step::AwaitHello;
    return true;
}

bool SecureChannel::receive(const uint8_t* p, size_t n) {
    if (status_ == Status::Failed) return false;
    if (step_ == Step::Init) return fail(Failure::BadHello);
    in_.insert(in_.end(), p, p + n);
    for (;;) {
        switch (step_) {
        case Step::Init:
            return fail(Failure::BadHello);
        case Step::AwaitHello: {
            // A stranger's garbage is refused as soon as its first bytes are wrong.
            size_t have = std::min(in_.size(), size_t(4));
            if (have && std::memcmp(in_.data(), kMagic, have) != 0) return fail(Failure::BadHello);
            if (in_.size() < kHelloLen) return true;
            if (!checkHello(in_.data())) return false;
            if (role_ == Role::Guest) {
                std::memcpy(hostHello_, in_.data(), kHelloLen);
            } else {
                std::memcpy(guestHello_, in_.data(), kHelloLen);
                makeHello(hostHello_);
            }
            in_.erase(in_.begin(), in_.begin() + kHelloLen);
            if (!deriveKeys(role_ == Role::Guest ? hostHello_ : guestHello_)) return false;
            if (role_ == Role::Host) {
                out_.insert(out_.end(), hostHello_, hostHello_ + kHelloLen);
            } else {
                uint8_t c[kConfirmLen];
                if (!makeConfirm(confirmGuest_, kGuestConfirmLabel, c)) return fail(Failure::Crypto);
                out_.insert(out_.end(), c, c + kConfirmLen);
            }
            step_ = Step::AwaitConfirm;
            break;
        }
        case Step::AwaitConfirm: {
            if (in_.size() < kConfirmLen) return true;
            if (role_ == Role::Host) {
                // The host answers only a guest that proved the code.
                if (!checkConfirm(confirmGuest_, kGuestConfirmLabel, in_.data())) return fail(Failure::WrongCode);
                uint8_t c[kConfirmLen];
                if (!makeConfirm(confirmHost_, kHostConfirmLabel, c)) return fail(Failure::Crypto);
                out_.insert(out_.end(), c, c + kConfirmLen);
            } else {
                if (!checkConfirm(confirmHost_, kHostConfirmLabel, in_.data())) return fail(Failure::WrongCode);
            }
            in_.erase(in_.begin(), in_.begin() + kConfirmLen);
            wipe(confirmGuest_, 32);
            wipe(confirmHost_, 32);
            step_ = Step::Done;
            status_ = Status::Established;
            break;
        }
        case Step::Done:
            return processFrames();
        }
    }
}

void SecureChannel::frameNonce(uint64_t counter, uint8_t nonce[12]) {
    std::memset(nonce, 0, 4);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = uint8_t(counter >> (56 - 8 * i));
}

bool SecureChannel::processFrames() {
    size_t pos = 0;
    while (in_.size() - pos >= 2) {
        size_t len = size_t(in_[pos]) | (size_t(in_[pos + 1]) << 8);
        if (len < 17 || len > kMaxPlaintext + 16) return fail(Failure::BadFrame);
        if (in_.size() - pos < 2 + len) break;
        if (recvCounter_ == UINT64_MAX) return fail(Failure::AuthFailed);
        uint8_t nonce[12];
        frameNonce(recvCounter_, nonce);
        std::vector<uint8_t> pt(len - 16);
        if (!recvKey_.open(nonce, &in_[pos], 2, &in_[pos + 2], len, pt.data())) return fail(Failure::AuthFailed);
        ++recvCounter_;
        messages_.push_back(std::move(pt));
        pos += 2 + len;
    }
    if (pos) in_.erase(in_.begin(), in_.begin() + pos);
    return true;
}

bool SecureChannel::popMessage(std::vector<uint8_t>& out) {
    if (messages_.empty()) return false;
    out = std::move(messages_.front());
    messages_.pop_front();
    return true;
}

bool SecureChannel::send(const uint8_t* p, size_t n) {
    if (status_ != Status::Established) return false;
    if (n == 0 || n > kMaxPlaintext) return fail(Failure::TooLarge);
    if (sendCounter_ == UINT64_MAX) return fail(Failure::Crypto);
    size_t len = n + 16;
    size_t at = out_.size();
    out_.resize(at + 2 + len);
    out_[at] = uint8_t(len & 0xFF);
    out_[at + 1] = uint8_t(len >> 8);
    uint8_t nonce[12];
    frameNonce(sendCounter_, nonce);
    if (!sendKey_.seal(nonce, &out_[at], 2, p, n, &out_[at + 2])) {
        out_.resize(at);
        return fail(Failure::Crypto);
    }
    ++sendCounter_;
    return true;
}

}  // namespace direct
}  // namespace net

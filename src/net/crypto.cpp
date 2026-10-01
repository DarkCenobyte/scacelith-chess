#include "crypto.h"
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(SCACELITH_HAS_OPENSSL)
#include <openssl/evp.h>
#include <openssl/rand.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace net {
namespace crypto {

namespace {

#if !defined(_WIN32) && !defined(SCACELITH_HAS_OPENSSL)
// ---- portable SHA-256 / SHA-1 (FIPS 180-4), only for Linux builds without OpenSSL ----
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

struct Sha256Ctx {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t used = 0;
    uint64_t total = 0;

    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total += n;
        while (n) {
            size_t k = 64 - used < n ? 64 - used : n;
            std::memcpy(buf + used, p, k);
            used += k; p += k; n -= k;
            if (used == 64) { block(buf); used = 0; }
        }
    }
    Sha256 final() {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80, zero = 0;
        update(&pad, 1);
        while (used != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        Sha256 out;
        for (int i = 0; i < 8; ++i)
            for (int k = 0; k < 4; ++k) out[4 * i + k] = uint8_t(h[i] >> (24 - 8 * k));
        return out;
    }
};

Sha1 sha1Portable(const void* data, size_t n) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> m(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
    uint64_t bits = uint64_t(n) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 0; i < 8; ++i) m.push_back(uint8_t(bits >> (56 - 8 * i)));
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(m[off + 4 * i]) << 24 | uint32_t(m[off + 4 * i + 1]) << 16 | uint32_t(m[off + 4 * i + 2]) << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    Sha1 out;
    for (int i = 0; i < 5; ++i)
        for (int k = 0; k < 4; ++k) out[4 * i + k] = uint8_t(h[i] >> (24 - 8 * k));
    return out;
}
#endif

#if defined(_WIN32)
// Algorithm providers are opened once and kept for the life of the process.
BCRYPT_ALG_HANDLE provider(LPCWSTR id) {
    BCRYPT_ALG_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&h, id, nullptr, 0) != 0) return nullptr;
    return h;
}
BCRYPT_ALG_HANDLE sha256Provider() {
    static BCRYPT_ALG_HANDLE h = provider(BCRYPT_SHA256_ALGORITHM);
    return h;
}
BCRYPT_ALG_HANDLE sha1Provider() {
    static BCRYPT_ALG_HANDLE h = provider(BCRYPT_SHA1_ALGORITHM);
    return h;
}
bool bcryptDigest(BCRYPT_ALG_HANDLE alg, const void* data, size_t n, uint8_t* out, ULONG outLen) {
    BCRYPT_HASH_HANDLE hh = nullptr;
    if (!alg || BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) != 0) return false;
    bool ok = BCryptHashData(hh, (PUCHAR)data, ULONG(n), 0) == 0 && BCryptFinishHash(hh, out, outLen, 0) == 0;
    BCryptDestroyHash(hh);
    return ok;
}
#endif

// Hashes prefix || suffix for many suffixes, reusing the state after the prefix.
class PrefixHasher {
public:
    explicit PrefixHasher(const std::string& prefix) {
#if defined(_WIN32)
        ok_ = sha256Provider() && BCryptCreateHash(sha256Provider(), &prefix_, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(prefix_, (PUCHAR)prefix.data(), ULONG(prefix.size()), 0) == 0;
#elif defined(SCACELITH_HAS_OPENSSL)
        prefix_ = EVP_MD_CTX_new();
        work_ = EVP_MD_CTX_new();
        ok_ = prefix_ && work_ && EVP_DigestInit_ex(prefix_, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(prefix_, prefix.data(), prefix.size()) == 1;
#else
        prefix_.update(prefix.data(), prefix.size());
        ok_ = true;
#endif
    }
    ~PrefixHasher() {
#if defined(_WIN32)
        if (prefix_) BCryptDestroyHash(prefix_);
#elif defined(SCACELITH_HAS_OPENSSL)
        EVP_MD_CTX_free(prefix_);
        EVP_MD_CTX_free(work_);
#endif
    }
    PrefixHasher(const PrefixHasher&) = delete;
    PrefixHasher& operator=(const PrefixHasher&) = delete;
    bool ok() const { return ok_; }

    bool hash(const char* suffix, size_t n, Sha256& out) {
#if defined(_WIN32)
        BCRYPT_HASH_HANDLE w = nullptr;
        if (BCryptDuplicateHash(prefix_, &w, nullptr, 0, 0) != 0) return false;
        bool ok = BCryptHashData(w, (PUCHAR)suffix, ULONG(n), 0) == 0 && BCryptFinishHash(w, out.data(), 32, 0) == 0;
        BCryptDestroyHash(w);
        return ok;
#elif defined(SCACELITH_HAS_OPENSSL)
        unsigned len = 0;
        return EVP_MD_CTX_copy_ex(work_, prefix_) == 1 && EVP_DigestUpdate(work_, suffix, n) == 1 &&
               EVP_DigestFinal_ex(work_, out.data(), &len) == 1 && len == 32;
#else
        Sha256Ctx w = prefix_;
        w.update(suffix, n);
        out = w.final();
        return true;
#endif
    }

private:
    bool ok_ = false;
#if defined(_WIN32)
    BCRYPT_HASH_HANDLE prefix_ = nullptr;
#elif defined(SCACELITH_HAS_OPENSSL)
    EVP_MD_CTX* prefix_ = nullptr;
    EVP_MD_CTX* work_ = nullptr;
#else
    Sha256Ctx prefix_;
#endif
};

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kB64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string encode64(const void* data, size_t n, const char* alphabet, bool pad) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = uint32_t(p[i]) << 16 | uint32_t(p[i + 1]) << 8 | p[i + 2];
        out += alphabet[v >> 18];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = uint32_t(p[i]) << 16;
        out += alphabet[v >> 18];
        out += alphabet[(v >> 12) & 63];
        if (pad) out += "==";
    } else if (n - i == 2) {
        uint32_t v = uint32_t(p[i]) << 16 | uint32_t(p[i + 1]) << 8;
        out += alphabet[v >> 18];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        if (pad) out += '=';
    }
    return out;
}

// Strict decoder: unused trailing bits must be zero, padding only where it belongs.
bool decode64(const std::string& in, std::vector<uint8_t>& out, bool url, bool padRequired) {
    std::string s = in;
    size_t padCount = 0;
    while (!s.empty() && s.back() == '=' && padCount < 2) { s.pop_back(); ++padCount; }
    if (padCount && (s.size() + padCount) % 4 != 0) return false;
    if (padRequired && (s.size() + padCount) % 4 != 0) return false;
    if (s.size() % 4 == 1) return false;
    out.clear();
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == (url ? '-' : '+')) v = 62;
        else if (c == (url ? '_' : '/')) v = 63;
        else return false;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
            acc &= (1u << bits) - 1;
        }
    }
    return acc == 0;
}

// Decimal ASCII of v into buf (no terminator); returns the length.
size_t decimal(uint64_t v, char* buf) {
    char tmp[24];
    size_t n = 0;
    do { tmp[n++] = char('0' + v % 10); v /= 10; } while (v);
    for (size_t i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    return n;
}

}  // namespace

const char* backendName() {
#if defined(_WIN32)
    return "bcrypt";
#elif defined(SCACELITH_HAS_OPENSSL)
    return "openssl";
#else
    return "portable";
#endif
}

Sha256 sha256(const void* data, size_t n) {
    Sha256 out{};
#if defined(_WIN32)
    bcryptDigest(sha256Provider(), data, n, out.data(), 32);
#elif defined(SCACELITH_HAS_OPENSSL)
    unsigned len = 0;
    EVP_Digest(data, n, out.data(), &len, EVP_sha256(), nullptr);
#else
    Sha256Ctx c;
    c.update(data, n);
    out = c.final();
#endif
    return out;
}

// ---- incremental SHA-256 ----
#if defined(_WIN32)
struct Sha256Stream::State {
    BCRYPT_HASH_HANDLE h = nullptr;
    void open() {
        if (sha256Provider()) BCryptCreateHash(sha256Provider(), &h, nullptr, 0, nullptr, 0, 0);
    }
    void close() {
        if (h) BCryptDestroyHash(h);
        h = nullptr;
    }
};
#elif defined(SCACELITH_HAS_OPENSSL)
struct Sha256Stream::State {
    EVP_MD_CTX* ctx = nullptr;
    void open() {
        ctx = EVP_MD_CTX_new();
        if (ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) close();
    }
    void close() {
        EVP_MD_CTX_free(ctx);
        ctx = nullptr;
    }
};
#else
struct Sha256Stream::State {
    Sha256Ctx c;
    void open() { c = Sha256Ctx(); }
    void close() {}
};
#endif

Sha256Stream::Sha256Stream() : st_(new State) { st_->open(); }
Sha256Stream::~Sha256Stream() { st_->close(); }

void Sha256Stream::reset() {
    st_->close();
    st_->open();
    bytes_ = 0;
}

void Sha256Stream::update(const void* data, size_t n) {
    bytes_ += n;
#if defined(_WIN32)
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (n && st_->h) {
        ULONG k = n > 0x40000000u ? 0x40000000u : ULONG(n);
        BCryptHashData(st_->h, (PUCHAR)p, k, 0);
        p += k;
        n -= k;
    }
#elif defined(SCACELITH_HAS_OPENSSL)
    if (st_->ctx) EVP_DigestUpdate(st_->ctx, data, n);
#else
    st_->c.update(data, n);
#endif
}

Sha256 Sha256Stream::finish() {
    Sha256 out{};
#if defined(_WIN32)
    if (st_->h) BCryptFinishHash(st_->h, out.data(), 32, 0);
#elif defined(SCACELITH_HAS_OPENSSL)
    unsigned len = 0;
    if (st_->ctx) EVP_DigestFinal_ex(st_->ctx, out.data(), &len);
#else
    out = st_->c.final();
#endif
    reset();
    return out;
}

Sha1 sha1(const void* data, size_t n) {
    Sha1 out{};
#if defined(_WIN32)
    bcryptDigest(sha1Provider(), data, n, out.data(), 20);
#elif defined(SCACELITH_HAS_OPENSSL)
    unsigned len = 0;
    EVP_Digest(data, n, out.data(), &len, EVP_sha1(), nullptr);
#else
    out = sha1Portable(data, n);
#endif
    return out;
}

bool randomBytes(void* out, size_t n) {
    if (n == 0) return true;
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(SCACELITH_HAS_OPENSSL)
    return RAND_bytes(static_cast<unsigned char*>(out), int(n)) == 1;
#else
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    uint8_t* p = static_cast<uint8_t*>(out);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += size_t(r);
    }
    ::close(fd);
    return got == n;
#endif
}

std::string base64(const void* data, size_t n) { return encode64(data, n, kB64, true); }
std::string base64url(const void* data, size_t n) { return encode64(data, n, kB64Url, false); }
bool base64Decode(const std::string& s, std::vector<uint8_t>& out) { return decode64(s, out, false, true); }
bool base64urlDecode(const std::string& s, std::vector<uint8_t>& out) { return decode64(s, out, true, false); }

std::string hex(const void* data, size_t n) {
    static const char d[] = "0123456789abcdef";
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = d[p[i] >> 4];
        s[2 * i + 1] = d[p[i] & 15];
    }
    return s;
}

bool hexDecode(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) return false;
    out.clear();
    out.reserve(s.size() / 2);
    auto nib = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(uint8_t(a << 4 | b));
    }
    return true;
}

bool constantTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = uint8_t(diff | (uint8_t(a[i]) ^ uint8_t(b[i])));
    return diff == 0;
}

std::string pkceChallenge(const std::string& verifier) {
    Sha256 d = sha256(verifier);
    return base64url(d.data(), d.size());
}

bool makePkce(Pkce& out) {
    uint8_t r[32];
    if (!randomBytes(r, sizeof(r))) return false;
    out.verifier = base64url(r, sizeof(r));
    out.challenge = pkceChallenge(out.verifier);
    std::memset(r, 0, sizeof(r));
    return true;
}

int leadingZeroBits(const Sha256& d) {
    int n = 0;
    for (uint8_t b : d) {
        if (b == 0) { n += 8; continue; }
        for (int bit = 7; bit >= 0 && !(b >> bit & 1); --bit) ++n;
        break;
    }
    return n;
}

bool powCheck(const std::string& challenge, const std::string& nonce, int bits) {
    if (bits < 0 || bits > 256 || nonce.empty() || nonce.size() > 20) return false;
    for (char c : nonce)
        if (c < '0' || c > '9') return false;
    return leadingZeroBits(sha256(challenge + ":" + nonce)) >= bits;
}

bool powSolve(const std::string& challenge, int bits, std::string& nonce, const std::atomic<bool>* cancel, PowStats* stats,
              uint64_t maxHashes, std::atomic<uint64_t>* progress) {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t done = 0;
    auto finish = [&](bool ok) {
        if (stats) {
            stats->hashes = done;
            stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        if (progress) progress->store(done, std::memory_order_relaxed);
        return ok;
    };
    if (bits < 0 || bits > kPowMaxBits) return finish(false);
    PrefixHasher hasher(challenge + ":");
    if (!hasher.ok()) return finish(false);
    // A full leading byte check first, then the partial one: most hashes fail on byte 0.
    const int fullBytes = bits / 8, restBits = bits % 8;
    const uint8_t restMask = uint8_t(0xFF00 >> restBits);
    char buf[24];
    Sha256 d;
    for (uint64_t i = 0; done < maxHashes; ++i) {
        if ((i & 4095) == 0) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return finish(false);
            if (progress) progress->store(done, std::memory_order_relaxed);
        }
        size_t len = decimal(i, buf);
        if (!hasher.hash(buf, len, d)) return finish(false);
        ++done;
        bool ok = true;
        for (int k = 0; k < fullBytes && ok; ++k) ok = d[size_t(k)] == 0;
        if (ok && restBits) ok = (d[size_t(fullBytes)] & restMask) == 0;
        if (ok) {
            nonce.assign(buf, len);
            return finish(true);
        }
    }
    return finish(false);
}

}  // namespace crypto
}  // namespace net

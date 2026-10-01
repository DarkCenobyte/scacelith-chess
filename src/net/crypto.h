// Cryptographic helpers of the online client: SHA-256 (and SHA-1 for the WebSocket handshake),
// secure random bytes, base64 / base64url / hex, PKCE (RFC 7636, S256) and the proof-of-work
// solver of the dedicated server (docs/DESIGN.md section 8).
//
// Backends: BCrypt on Windows, OpenSSL on Linux dev builds; a portable implementation (hashes
// from FIPS 180-4, randomness from the kernel) when a Linux build has no OpenSSL. All functions
// are thread-safe.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace net {
namespace crypto {

using Sha256 = std::array<uint8_t, 32>;
using Sha1 = std::array<uint8_t, 20>;

const char* backendName();                       // "bcrypt", "openssl" or "portable"

Sha256 sha256(const void* data, size_t n);
inline Sha256 sha256(const std::string& s) { return sha256(s.data(), s.size()); }
Sha1 sha1(const void* data, size_t n);           // WebSocket accept key only

// Incremental SHA-256, for data that never sits whole in memory (downloads, model files).
class Sha256Stream {
public:
    Sha256Stream();
    ~Sha256Stream();
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;
    void update(const void* data, size_t n);
    Sha256 finish();                             // the digest of everything so far; starts over
    void reset();
    uint64_t bytes() const { return bytes_; }    // fed since the last reset

private:
    struct State;
    std::unique_ptr<State> st_;
    uint64_t bytes_ = 0;
};

bool randomBytes(void* out, size_t n);           // false when the OS generator failed

std::string base64(const void* data, size_t n);           // RFC 4648 with padding
std::string base64url(const void* data, size_t n);        // RFC 4648 section 5, no padding
bool base64Decode(const std::string& s, std::vector<uint8_t>& out);     // strict, padding required
bool base64urlDecode(const std::string& s, std::vector<uint8_t>& out);  // padding optional
std::string hex(const void* data, size_t n);              // lower-case
bool hexDecode(const std::string& s, std::vector<uint8_t>& out);        // either case, even length
inline std::string hex(const Sha256& d) { return hex(d.data(), d.size()); }

// Compares two byte strings in time independent of where they differ.
bool constantTimeEqual(const std::string& a, const std::string& b);

// PKCE: verifier = base64url of 32 random bytes (43 characters), challenge = base64url(SHA-256).
struct Pkce {
    std::string verifier, challenge;
};
bool makePkce(Pkce& out);
std::string pkceChallenge(const std::string& verifier);

// ---- proof of work (DESIGN 8) ----
// Find a nonce (decimal ASCII) such that SHA-256(challenge + ":" + nonce) starts with 'bits'
// zero bits, most significant bit of the first byte first.
int leadingZeroBits(const Sha256& d);
bool powCheck(const std::string& challenge, const std::string& nonce, int bits);

constexpr int kPowMaxBits = 30;                  // refuse harder puzzles (hours on a slow PC)

struct PowStats {
    uint64_t hashes = 0;
    double seconds = 0;
    double hashesPerSecond() const { return seconds > 0 ? double(hashes) / seconds : 0; }
};

// Solves on the calling thread. Returns false when bits is out of [0, kPowMaxBits], when *cancel
// becomes true (checked every few thousand hashes) or after maxHashes attempts. The hash state
// of the fixed prefix is computed once and copied for every nonce. progress (optional) receives
// the number of hashes done so far, for a progress display.
bool powSolve(const std::string& challenge, int bits, std::string& nonce, const std::atomic<bool>* cancel = nullptr,
              PowStats* stats = nullptr, uint64_t maxHashes = UINT64_MAX, std::atomic<uint64_t>* progress = nullptr);

}  // namespace crypto
}  // namespace net

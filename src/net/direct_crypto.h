// Direct match secure channel (see src/net/direct_match.h and docs/DIRECT_MATCH.md): join codes,
// the handshake that proves both players know the code, and the encrypted frames that carry the
// net::proto messages. Windows: BCrypt only; Linux dev/test builds: OpenSSL libcrypto.
//
// Handshake (TCP, the guest speaks first):
//   guest -> host   GuestHello = "SCDM" | version 1 | 32-byte nonce Ng | P-256 public key Qg (65 bytes, uncompressed)
//   host  -> guest  HostHello  = "SCDM" | version 1 | 32-byte nonce Nh | P-256 public key Qh
//   both            Z   = ECDH(own private key, peer public key), X coordinate, 32 bytes big-endian
//                   OKM = HKDF-SHA256(salt = Ng || Nh, ikm = Z || code (12 ASCII chars),
//                                     info = "scacelith direct match v1", 128 bytes)
//                       = Kg2h | Kh2g | Kconfirm_guest | Kconfirm_host   (4 x 32 bytes, AES-256 keys)
//                   TH  = SHA-256(GuestHello || HostHello)
//   guest -> host   GuestConfirm = AES-256-GCM(Kconfirm_guest, nonce 0, aad "SCDM guest confirm", TH): 48 bytes
//   host            checks it (tag and TH, constant time); on failure closes without a word
//   host  -> guest  HostConfirm  = AES-256-GCM(Kconfirm_host, nonce 0, aad "SCDM host confirm", TH): 48 bytes
//   guest           checks it; the channel is established on both sides.
// Frames (both directions): u16 length (little-endian, = ciphertext + 16) | ciphertext | 16-byte
// tag, AES-256-GCM with the direction's key, nonce = 4 zero bytes || u64 counter (big-endian,
// 0, 1, 2... per direction), aad = the two length bytes. Plaintext = one net::proto message of
// 1..16384 bytes (a GameSnapshot carries 10 bytes per ply, up to 1200 plies: about 12.2 KB; the
// 1024 of the first draft could not hold the snapshot of a long game). Any tag failure, a length
// out of range or a counter overflow fails the channel for good (the connection must be closed):
// no replay, reordering, truncation or splicing.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace net {
namespace direct {

// ---- join codes ----
constexpr const char* kCodeAlphabet = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";   // 31 symbols, no 0/O, 1/I/L
constexpr int kCodeLength = 12;                                            // 12 x log2(31) = 59.4 bits
std::string newJoinCode();                           // normalised ("K7Q2M9XH3PTR"); "" if the RNG failed
// Removes dashes and spaces and upper-cases; false unless exactly 12 alphabet characters remain.
bool normalizeJoinCode(const std::string& in, std::string& out);
std::string formatJoinCode(const std::string& normalised);   // "K7Q2-M9XH-3PTR"

// ---- primitives ----
bool randomBytes(uint8_t* out, size_t n);
bool sha256(const uint8_t* p, size_t n, uint8_t out[32]);
bool hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t n, uint8_t out[32]);
// RFC 5869 over hmacSha256 (outLen <= 255 * 32).
bool hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const uint8_t* info,
                size_t infoLen, uint8_t* out, size_t outLen);
bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n);

// AES-256-GCM with a 12-byte nonce and a 16-byte tag.
class AesGcm {
public:
    AesGcm();
    ~AesGcm();
    AesGcm(const AesGcm&) = delete;
    AesGcm& operator=(const AesGcm&) = delete;
    bool setKey(const uint8_t key[32]);
    // out receives ptLen + 16 bytes (ciphertext then tag).
    bool seal(const uint8_t nonce[12], const uint8_t* aad, size_t aadLen, const uint8_t* pt, size_t ptLen, uint8_t* out);
    // in = ciphertext then tag (inLen >= 16); out receives inLen - 16 bytes. False on a bad tag.
    bool open(const uint8_t nonce[12], const uint8_t* aad, size_t aadLen, const uint8_t* in, size_t inLen, uint8_t* out);
private:
    struct State;
    std::unique_ptr<State> s_;
};

// Ephemeral ECDH on P-256.
class EcdhP256 {
public:
    static constexpr size_t kPublicKeyLen = 65;      // 0x04 || X || Y
    EcdhP256();
    ~EcdhP256();
    EcdhP256(const EcdhP256&) = delete;
    EcdhP256& operator=(const EcdhP256&) = delete;
    bool generate();
    // Known key pair (test vectors): private scalar d and its public key.
    bool setKeyPair(const uint8_t d[32], const uint8_t pub[65]);
    const uint8_t* publicKey() const { return pub_; }
    // Shared secret = X coordinate of d * peer, big-endian. False for an invalid peer key.
    bool agree(const uint8_t peer[65], uint8_t secret[32]);
private:
    struct State;
    std::unique_ptr<State> s_;
    uint8_t pub_[65] = {};
};

// ---- the channel ----
class SecureChannel {
public:
    enum class Role : uint8_t { Guest, Host };
    enum class Status : uint8_t { Handshake, Established, Failed };
    enum class Failure : uint8_t {
        None,
        BadHello,       // not "SCDM", or a malformed key / nonce
        BadVersion,     // "SCDM" with another version byte
        WrongCode,      // the confirmation did not verify (wrong code, or someone in the middle)
        BadFrame,       // frame length out of range
        AuthFailed,     // a frame's tag did not verify (tampered, replayed, reordered, truncated)
        TooLarge,       // send() of a message longer than kMaxPlaintext
        Crypto,         // RNG or crypto library failure
    };
    static constexpr size_t kHelloLen = 4 + 1 + 32 + 65;
    static constexpr size_t kConfirmLen = 32 + 16;
    static constexpr size_t kMaxPlaintext = 16384;
    static constexpr uint8_t kVersion = 1;

    // code: the normalised 12-character join code.
    SecureChannel(Role role, const std::string& code);
    ~SecureChannel();
    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;

    // Generates the ephemeral key; the guest's hello goes to the outbox at once.
    bool start();
    // Feeds bytes received from the peer. False once the channel has failed.
    bool receive(const uint8_t* p, size_t n);
    // Next decrypted message (Established only).
    bool popMessage(std::vector<uint8_t>& out);
    // Encrypts one message into the outbox (Established only).
    bool send(const uint8_t* p, size_t n);
    // Bytes to write to the socket; the caller erases what it wrote.
    std::vector<uint8_t>& outbox() { return out_; }

    Failure failure() const { return failure_; }
    bool established() const { return status_ == Status::Established; }
    bool failed() const { return status_ == Status::Failed; }
    // The guest sent its confirmation and waits for the host's: a connection closed now almost
    // always means the host refused the code.
    bool awaitingHostConfirm() const { return role_ == Role::Guest && step_ == Step::AwaitConfirm; }
    // Bytes of an incomplete handshake message or frame are buffered (a close now truncates it).
    bool hasPartialInput() const { return !in_.empty(); }

private:
    enum class Step : uint8_t { Init, AwaitHello, AwaitConfirm, Done };
    Role role_;
    std::string code_;
    Status status_ = Status::Handshake;
    Failure failure_ = Failure::None;
    Step step_ = Step::Init;
    EcdhP256 ecdh_;
    uint8_t nonce_[32] = {};
    uint8_t guestHello_[kHelloLen] = {}, hostHello_[kHelloLen] = {};
    uint8_t th_[32] = {};
    AesGcm sendKey_, recvKey_;
    uint8_t confirmGuest_[32] = {}, confirmHost_[32] = {};
    uint64_t sendCounter_ = 0, recvCounter_ = 0;
    std::vector<uint8_t> in_, out_;
    std::deque<std::vector<uint8_t>> messages_;

    bool fail(Failure f);
    void makeHello(uint8_t out[kHelloLen]) const;
    bool checkHello(const uint8_t* h);
    bool deriveKeys(const uint8_t* peerHello);
    bool makeConfirm(const uint8_t key[32], const char* label, uint8_t out[kConfirmLen]);
    bool checkConfirm(const uint8_t key[32], const char* label, const uint8_t* in);
    bool processFrames();
    static void frameNonce(uint64_t counter, uint8_t nonce[12]);
};

}  // namespace direct
}  // namespace net

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lunar::net {

// The session crypto of RobloxPlayer 0.742, which is libsodium's (pinned by
// running the client's functions under emulation, see PROTOCOL.md 2.4/3):
// crypto_kx (X25519 + BLAKE2b) for the keys, ChaCha20-Poly1305 IETF or
// AES-256-GCM, detached, for the packets.

using Key = std::array<std::uint8_t, 32>;

struct KeyPair {
    Key publicKey{};
    Key secretKey{};

    static KeyPair generate();
    static KeyPair fromSecret(const Key& secret);
};

// crypto_kx session keys: what this side decrypts with (rx) and encrypts with (tx).
struct SessionKeys {
    Key rx{};
    Key tx{};
};

// 0x101834e6d / 0x101834fb6.
SessionKeys kxClient(const KeyPair& client, const Key& serverPublic);
SessionKeys kxServer(const KeyPair& server, const Key& clientPublic);

// The session cipher number on the wire (RbxOpenRequest2/Reply2).
enum class Cipher : std::uint8_t {
    ChaCha20Poly1305 = 1,
    Aes256Gcm = 2,
};
const char* cipherName(Cipher cipher);
// Whether this machine can do AES-256-GCM (libsodium needs AES-NI); the client
// asks for it when it can (0x102d93bfe).
bool aesGcmAvailable();

// The hard-coded server keys of key exchange version 0 ("local" servers),
// copied from the binary by 0x102d5368e.
namespace localkeys {
extern const Key earlyPublic;
extern const Key earlySecret;
extern const Key ephemeralEarlyPublic;
extern const Key ephemeralEarlySecret;
} // namespace localkeys

// RbxOpenRequest2/Reply2 sealing (0x102d93dba / 0x102d93ea4): ChaCha20-Poly1305
// IETF over `box` with `aad` as additional data. The result is
// aad || ciphertext || nonce(12) || tag(16); the nonce is
// u64le(0x754e657571696e55 + counter) || "mber".
inline constexpr std::size_t kHandshakeOverhead = 28;
std::vector<std::uint8_t> sealHandshake(std::span<const std::uint8_t> aad, std::span<const std::uint8_t> box,
                                        const Key& key, std::uint64_t counter);
// Opens what sealHandshake made: `message` starts with the aad. Returns the box.
std::optional<std::vector<std::uint8_t>> openHandshake(std::span<const std::uint8_t> message, std::size_t aadSize,
                                                       std::size_t boxSize, const Key& key);
// The same with the box starting at `boxOffset` (>= aadSize): bytes between
// the authenticated header and the box are left out of both.
std::optional<std::vector<std::uint8_t>> openHandshakeAt(std::span<const std::uint8_t> message, std::size_t aadSize,
                                                         std::size_t boxOffset, std::size_t boxSize, const Key& key);

// Connected datagrams (0x102d942f0 / 0x102d94458): ciphertext || u16 nonce
// low bits || tag(16); nonce = u64le(counter) || "mbeR", the counter starting
// at 0x754e657571696e55 on both sides.
inline constexpr std::size_t kDatagramOverhead = 18;
inline constexpr std::uint64_t kNonceStart = 0x754e657571696e55ULL; // "UniqueNu"

class DatagramCipher {
public:
    DatagramCipher() = default;
    DatagramCipher(Cipher cipher, const SessionKeys& keys) : cipher_(cipher), keys_(keys) {}

    std::vector<std::uint8_t> seal(std::span<const std::uint8_t> plaintext);
    // Decrypts and advances the receive counter; nullopt when the tag doesn't match.
    std::optional<std::vector<std::uint8_t>> open(std::span<const std::uint8_t> wire);
    // The full nonce counter `open` would use for this packet, without
    // touching the state (for the decoder's printout).
    std::uint64_t rxCounterFor(std::uint16_t low) const;

    // Picks up a session mid-way (tests, decoders joining late).
    void setCounters(std::uint64_t tx, std::uint64_t rx)
    {
        txCounter_ = tx;
        rxCounter_ = rx;
    }

    Cipher cipher() const { return cipher_; }
    const SessionKeys& keys() const { return keys_; }
    std::uint64_t txCounter() const { return txCounter_; }
    std::uint64_t rxCounter() const { return rxCounter_; }

private:
    Cipher cipher_ = Cipher::ChaCha20Poly1305;
    SessionKeys keys_{};
    std::uint64_t txCounter_ = kNonceStart;
    std::uint64_t rxCounter_ = kNonceStart;
};

// RakNet's DataBlockEncryptor, which seals the bodies of some Roblox messages
// (everything after the id byte) with a fixed AES-128 key; the server opens
// it in place and drops the client with 260 when it can't (0.742
// 0x10557cf3a, cipher 0x105798c12/0x105798dfc). The block: u32 LE checksum
// (RakNet CheckSum over the rest), a random byte, (random << 4) | pad, pad
// random bytes, the body; AES-128 on block 0, then on every later block from
// the back, each XORed first with the block encrypted before it. Verified by
// opening a genuine 0.741 0x90 from a capture.
using BlockKey = std::array<std::uint8_t, 16>;
std::vector<std::uint8_t> blockEncrypt(const BlockKey& key, std::span<const std::uint8_t> plain);
std::optional<std::vector<std::uint8_t>> blockDecrypt(const BlockKey& key, std::span<const std::uint8_t> sealed);
// The 0x90 join request's key (the 0.741 client's global 0x10770afa0, built at
// start-up; a 0.742 server opens it) [gdb + capture].
inline constexpr BlockKey kJoinRequestKey{0xfe, 0xf9, 0xf0, 0xeb, 0xe2, 0xdd, 0xd4, 0xcf,
                                          0xc6, 0xc1, 0xb8, 0xb3, 0xaa, 0xa5, 0x9c, 0x97};
// A message with its body sealed: id || blockEncrypt(key, body).
std::vector<std::uint8_t> sealMessageBody(const BlockKey& key, std::span<const std::uint8_t> message);

} // namespace lunar::net

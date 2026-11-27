#include <lunar/net/Crypto.hpp>

#include <openssl/evp.h>
#include <sodium.h>

#include <cstring>
#include <stdexcept>

namespace lunar::net {

namespace {

void initSodium()
{
    static const bool ok = sodium_init() >= 0;
    if (!ok)
        throw std::runtime_error("libsodium failed to initialise");
}

using Nonce = std::array<std::uint8_t, 12>;

Nonce makeNonce(std::uint64_t counter, const char tail[4])
{
    Nonce nonce{};
    for (std::size_t i = 0; i < 8; ++i)
        nonce[i] = static_cast<std::uint8_t>(counter >> (8 * i));
    std::memcpy(nonce.data() + 8, tail, 4);
    return nonce;
}

} // namespace

KeyPair KeyPair::generate()
{
    initSodium();
    KeyPair pair;
    crypto_kx_keypair(pair.publicKey.data(), pair.secretKey.data());
    return pair;
}

KeyPair KeyPair::fromSecret(const Key& secret)
{
    initSodium();
    KeyPair pair;
    pair.secretKey = secret;
    crypto_scalarmult_base(pair.publicKey.data(), secret.data());
    return pair;
}

SessionKeys kxClient(const KeyPair& client, const Key& serverPublic)
{
    initSodium();
    SessionKeys keys;
    if (crypto_kx_client_session_keys(keys.rx.data(), keys.tx.data(), client.publicKey.data(), client.secretKey.data(),
                                      serverPublic.data())
        != 0)
        throw std::runtime_error("crypto_kx_client_session_keys: bad server key");
    return keys;
}

SessionKeys kxServer(const KeyPair& server, const Key& clientPublic)
{
    initSodium();
    SessionKeys keys;
    if (crypto_kx_server_session_keys(keys.rx.data(), keys.tx.data(), server.publicKey.data(), server.secretKey.data(),
                                      clientPublic.data())
        != 0)
        throw std::runtime_error("crypto_kx_server_session_keys: bad client key");
    return keys;
}

const char* cipherName(Cipher cipher)
{
    switch (cipher) {
    case Cipher::ChaCha20Poly1305:
        return "chacha20-poly1305";
    case Cipher::Aes256Gcm:
        return "aes-256-gcm";
    }
    return "unknown";
}

bool aesGcmAvailable()
{
    initSodium();
    return crypto_aead_aes256gcm_is_available() == 1;
}

namespace localkeys {
// Key exchange version 0, from 0x1064317e2..0x106431862 (0x102d5368e).
const Key earlyPublic{0xcb, 0xb1, 0x8f, 0x91, 0x1d, 0x92, 0xb5, 0x5c, 0xd1, 0x8b, 0x8d, 0x70, 0x68, 0x9d, 0xe6, 0x26,
                      0x08, 0xa0, 0xfa, 0x40, 0x17, 0xc8, 0x59, 0xb2, 0x47, 0xd4, 0x67, 0xe2, 0x13, 0x88, 0x2e, 0x61};
const Key earlySecret{0xe3, 0x44, 0x6c, 0x52, 0x05, 0xa5, 0x1d, 0xb9, 0x03, 0x1f, 0x36, 0xa2, 0x59, 0x8d, 0x90, 0x5a,
                      0x35, 0xb7, 0x30, 0xdd, 0x5c, 0xfc, 0x48, 0x4c, 0x86, 0x13, 0x12, 0x81, 0xa8, 0x46, 0x71, 0x43};
const Key ephemeralEarlyPublic{0xd3, 0x71, 0xcb, 0x6e, 0x10, 0x7c, 0xcf, 0xc3, 0xaa, 0xe7, 0xee,
                               0xe4, 0x7b, 0x4b, 0x6d, 0x8b, 0x66, 0x5c, 0x59, 0x43, 0x6e, 0x2f,
                               0x83, 0x41, 0x7b, 0xfa, 0xbe, 0x29, 0xa1, 0xc6, 0x46, 0x3d};
const Key ephemeralEarlySecret{0x3c, 0xdb, 0xcc, 0x14, 0x31, 0xb6, 0x2d, 0xce, 0x8c, 0xe6, 0xf3,
                               0xdb, 0x77, 0xd7, 0x4f, 0xe5, 0xf7, 0x5e, 0x49, 0xba, 0x17, 0x6d,
                               0xe2, 0x17, 0xf7, 0x1e, 0x31, 0x31, 0x2a, 0x2c, 0x45, 0x4e};
} // namespace localkeys

std::vector<std::uint8_t> sealHandshake(std::span<const std::uint8_t> aad, std::span<const std::uint8_t> box,
                                        const Key& key, std::uint64_t counter)
{
    initSodium();
    const Nonce nonce = makeNonce(kNonceStart + counter, "mber");
    std::vector<std::uint8_t> out(aad.size() + box.size() + kHandshakeOverhead);
    std::memcpy(out.data(), aad.data(), aad.size());
    std::uint8_t* ct = out.data() + aad.size();
    std::memcpy(ct + box.size(), nonce.data(), nonce.size());
    unsigned long long macLen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt_detached(ct, ct + box.size() + 12, &macLen, box.data(), box.size(),
                                                       aad.data(), aad.size(), nullptr, nonce.data(), key.data());
    return out;
}

std::optional<std::vector<std::uint8_t>> openHandshake(std::span<const std::uint8_t> message, std::size_t aadSize,
                                                       std::size_t boxSize, const Key& key)
{
    return openHandshakeAt(message, aadSize, aadSize, boxSize, key);
}

std::optional<std::vector<std::uint8_t>> openHandshakeAt(std::span<const std::uint8_t> message, std::size_t aadSize,
                                                         std::size_t boxOffset, std::size_t boxSize, const Key& key)
{
    initSodium();
    if (boxOffset < aadSize || message.size() < boxOffset + boxSize + kHandshakeOverhead)
        return std::nullopt;
    const std::uint8_t* ct = message.data() + boxOffset;
    std::vector<std::uint8_t> box(boxSize);
    if (crypto_aead_chacha20poly1305_ietf_decrypt_detached(box.data(), nullptr, ct, boxSize, ct + boxSize + 12,
                                                           message.data(), aadSize, ct + boxSize, key.data())
        != 0)
        return std::nullopt;
    return box;
}

std::vector<std::uint8_t> DatagramCipher::seal(std::span<const std::uint8_t> plaintext)
{
    initSodium();
    const Nonce nonce = makeNonce(txCounter_++, "mbeR");
    std::vector<std::uint8_t> out(plaintext.size() + kDatagramOverhead);
    // The client writes the 12-byte nonce where the tag goes, encrypts (the
    // nonce is consumed first) and lets the tag overwrite all but its first
    // two bytes, which stay as the counter's low 16 bits.
    out[plaintext.size()] = nonce[0];
    out[plaintext.size() + 1] = nonce[1];
    unsigned long long macLen = 0;
    std::uint8_t* mac = out.data() + plaintext.size() + 2;
    if (cipher_ == Cipher::Aes256Gcm)
        crypto_aead_aes256gcm_encrypt_detached(out.data(), mac, &macLen, plaintext.data(), plaintext.size(), nullptr, 0,
                                               nullptr, nonce.data(), keys_.tx.data());
    else
        crypto_aead_chacha20poly1305_ietf_encrypt_detached(out.data(), mac, &macLen, plaintext.data(), plaintext.size(),
                                                           nullptr, 0, nullptr, nonce.data(), keys_.tx.data());
    return out;
}

std::uint64_t DatagramCipher::rxCounterFor(std::uint16_t low) const
{
    const std::uint64_t candidate = (rxCounter_ & ~std::uint64_t{0xffff}) | low;
    if (candidate >= rxCounter_)
        return candidate - rxCounter_ < 0x8001 ? candidate : candidate - 0x10000;
    return rxCounter_ - candidate >= 0x8001 ? candidate + 0x10000 : candidate;
}

std::optional<std::vector<std::uint8_t>> DatagramCipher::open(std::span<const std::uint8_t> wire)
{
    initSodium();
    if (wire.size() < kDatagramOverhead)
        return std::nullopt;
    const std::size_t len = wire.size() - kDatagramOverhead;
    const auto low = static_cast<std::uint16_t>(wire[len] | wire[len + 1] << 8);
    const std::uint64_t counter = rxCounterFor(low);
    const Nonce nonce = makeNonce(counter, "mbeR");
    std::vector<std::uint8_t> plain(len);
    const std::uint8_t* mac = wire.data() + len + 2;
    const int rc = cipher_ == Cipher::Aes256Gcm
                       ? crypto_aead_aes256gcm_decrypt_detached(plain.data(), nullptr, wire.data(), len, mac, nullptr,
                                                                0, nonce.data(), keys_.rx.data())
                       : crypto_aead_chacha20poly1305_ietf_decrypt_detached(
                             plain.data(), nullptr, wire.data(), len, mac, nullptr, 0, nonce.data(), keys_.rx.data());
    if (rc != 0)
        return std::nullopt;
    if (counter > rxCounter_)
        rxCounter_ = counter;
    return plain;
}

namespace {

// RakNet's CheckSum::Add over every byte; Get() is the sum.
std::uint32_t raknetChecksum(std::span<const std::uint8_t> data)
{
    std::uint16_t r = 55665;
    std::uint32_t sum = 0;
    for (const std::uint8_t v : data) {
        const auto cipher = static_cast<std::uint8_t>(v ^ (r >> 8));
        r = static_cast<std::uint16_t>((cipher + r) * 52845u + 22719u);
        sum += cipher;
    }
    return sum;
}

struct Aes128 {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    Aes128(const BlockKey& key, bool encrypt)
    {
        if (!ctx || EVP_CipherInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key.data(), nullptr, encrypt ? 1 : 0) != 1)
            throw std::runtime_error("AES-128 is unavailable");
        EVP_CIPHER_CTX_set_padding(ctx, 0);
    }
    ~Aes128() { EVP_CIPHER_CTX_free(ctx); }
    Aes128(const Aes128&) = delete;
    Aes128& operator=(const Aes128&) = delete;
    void block(std::uint8_t* b)
    {
        std::uint8_t out[32];
        int n = 0;
        EVP_CipherUpdate(ctx, out, &n, b, 16);
        std::memcpy(b, out, 16);
    }
};

} // namespace

std::vector<std::uint8_t> blockEncrypt(const BlockKey& key, std::span<const std::uint8_t> plain)
{
    initSodium();
    const std::size_t header = 6; // checksum, random byte, pad byte
    const auto pad = static_cast<std::uint8_t>((16 - (plain.size() + header) % 16) % 16);
    std::vector<std::uint8_t> out(header + pad + plain.size());
    std::uint8_t random[2];
    randombytes_buf(random, sizeof random);
    out[4] = random[0];
    out[5] = static_cast<std::uint8_t>((random[1] << 4) | pad);
    randombytes_buf(out.data() + header, pad);
    if (!plain.empty()) // an empty span's data() may be null
        std::memcpy(out.data() + header + pad, plain.data(), plain.size());
    const std::uint32_t sum = raknetChecksum(std::span(out).subspan(4));
    for (int i = 0; i < 4; ++i)
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(sum >> (8 * i));
    Aes128 aes(key, true);
    aes.block(out.data());
    std::size_t last = 0;
    for (std::size_t at = out.size() - 16; at >= 16; at -= 16) {
        for (std::size_t b = 0; b < 16; ++b)
            out[at + b] ^= out[last + b];
        aes.block(out.data() + at);
        last = at;
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> blockDecrypt(const BlockKey& key, std::span<const std::uint8_t> sealed)
{
    if (sealed.size() < 16 || sealed.size() % 16 != 0)
        return std::nullopt;
    std::vector<std::uint8_t> out(sealed.begin(), sealed.end());
    Aes128 aes(key, false);
    // Undo the back-to-front pass front to back: block `at` was XORed with
    // the ciphertext of the block after it (block 0 for the last one).
    for (std::size_t at = 16; at < out.size(); at += 16) {
        aes.block(out.data() + at);
        const std::size_t previous = at + 16 < out.size() ? at + 16 : 0;
        for (std::size_t b = 0; b < 16; ++b)
            out[at + b] ^= out[previous + b];
    }
    aes.block(out.data());
    const std::uint32_t stored = static_cast<std::uint32_t>(out[0]) | static_cast<std::uint32_t>(out[1]) << 8
                                 | static_cast<std::uint32_t>(out[2]) << 16 | static_cast<std::uint32_t>(out[3]) << 24;
    const std::size_t pad = out[5] & 0x0f;
    if (stored != raknetChecksum(std::span(out).subspan(4)) || 6 + pad > out.size())
        return std::nullopt;
    return std::vector<std::uint8_t>(out.begin() + static_cast<std::ptrdiff_t>(6 + pad), out.end());
}

std::vector<std::uint8_t> sealMessageBody(const BlockKey& key, std::span<const std::uint8_t> message)
{
    if (message.empty())
        return {};
    std::vector<std::uint8_t> out{message[0]};
    const auto sealed = blockEncrypt(key, message.subspan(1));
    out.insert(out.end(), sealed.begin(), sealed.end());
    return out;
}

} // namespace lunar::net

// The console's crypt table (Console.cpp opens it): base64, AES, random bytes
// and keys, and the message digests. The executors document the shapes this
// follows -- keys and ciphertexts travel as base64 text, generatebytes and
// generatekey hand back random material the same way, and encrypt/decrypt
// take the mode by name.
//
// The ciphers and digests are libcrypto's, which the network side already
// links (net/src/Crypto.cpp uses EVP for RakNet's AES-128-ECB).

#include "Console.hpp"
#include "Runtime.hpp"

#include <lapi.h>
#include <lobject.h>
#include <lstate.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <format>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lunar::lua {

namespace {

constexpr std::string_view alphabet
    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// base64(text): the bytes of `text` in the standard alphabet, padded to a
// multiple of four. What the whole family of these functions carries data as,
// so generatebytes and encrypt hand back text.
std::string base64(std::string_view bytes)
{
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const auto group = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 16)
                           | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 1])) << 8)
                           | static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 2]));
        for (int shift = 18; shift >= 0; shift -= 6)
            out.push_back(alphabet[(group >> shift) & 0x3f]);
    }
    if (const std::size_t left = bytes.size() - i; left == 1) {
        const auto group = static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 16;
        out.push_back(alphabet[(group >> 18) & 0x3f]);
        out.push_back(alphabet[(group >> 12) & 0x3f]);
        out.append("==");
    } else if (left == 2) {
        const auto group = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 16)
                           | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 1])) << 8);
        out.push_back(alphabet[(group >> 18) & 0x3f]);
        out.push_back(alphabet[(group >> 12) & 0x3f]);
        out.push_back(alphabet[(group >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

// The bytes of base64 text, as engine/src/Base64.hpp decodes them: the
// standard alphabet, "=" padding and whitespace anywhere.
std::optional<std::string> unbase64(std::string_view text)
{
    std::array<int, 256> values{};
    values.fill(-1);
    for (std::size_t i = 0; i < alphabet.size(); ++i)
        values[static_cast<std::uint8_t>(alphabet[i])] = static_cast<int>(i);

    std::string bytes;
    std::uint32_t group = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
            continue;
        const int value = values[static_cast<std::uint8_t>(c)];
        if (value < 0)
            return std::nullopt;
        group = (group << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<char>((group >> bits) & 0xff));
        }
    }
    return bytes;
}

int base64Encode(lua_State* L)
{
    const std::string text = base64(checkString(L, 1));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int base64Decode(lua_State* L)
{
    auto bytes = unbase64(checkString(L, 1));
    if (!bytes)
        raise(L, "the text is not base64");
    lua_pushlstring(L, bytes->data(), bytes->size());
    return 1;
}

// The key sizes AES has, and the IV is one block for all of them.
constexpr std::size_t ivSize = 16;

const EVP_CIPHER* cipherFor(std::string_view key, std::string_view mode)
{
    std::string_view name;
    if (mode.empty() || mode == "CBC" || mode == "AES")
        name = "CBC";
    else if (mode == "ECB")
        name = "ECB";
    else
        return nullptr;
    switch (key.size()) {
    case 16:
        return name == "ECB" ? EVP_aes_128_ecb() : EVP_aes_128_cbc();
    case 24:
        return name == "ECB" ? EVP_aes_192_ecb() : EVP_aes_192_cbc();
    case 32:
        return name == "ECB" ? EVP_aes_256_ecb() : EVP_aes_256_cbc();
    default:
        return nullptr;
    }
}

// The EVP_CIPHER_CTX the operations below own.
struct Cipher {
    EVP_CIPHER_CTX* context = nullptr;

    Cipher() : context(EVP_CIPHER_CTX_new()) {}
    Cipher(const Cipher&) = delete;
    Cipher& operator=(const Cipher&) = delete;
    ~Cipher()
    {
        if (context)
            EVP_CIPHER_CTX_free(context);
    }
    explicit operator bool() const { return context != nullptr; }
};

// The key and IV both travel as base64, so they are decoded here: a key of 16,
// 24 or 32 bytes picks the AES size, anything else is no AES key. An IV that
// isn't a block long is cut to one, which is what the executors do with a
// short one.
struct Key {
    std::string bytes;
    std::string iv;
};

Key checkKey(lua_State* L, const char* what)
{
    auto bytes = unbase64(checkString(L, 2));
    if (!bytes || (bytes->size() != 16 && bytes->size() != 24 && bytes->size() != 32))
        luaL_argerror(L, 2, what);
    Key key{std::move(*bytes), {}};
    if (lua_type(L, 3) == LUA_TSTRING)
        if (auto iv = unbase64(checkString(L, 3)))
            key.iv = std::move(*iv).substr(0, ivSize);
    return key;
}

// One AES pass. `encrypt` decides the direction; the padding is the PKCS#7
// the block ciphers need and is removed again when decrypting.
std::optional<std::string> aes(const EVP_CIPHER* cipher, const Key& key, std::string_view data, bool encrypt)
{
    Cipher context;
    if (!context)
        return std::nullopt;
    const unsigned char* iv = reinterpret_cast<const unsigned char*>(key.iv.data());
    if (EVP_CipherInit_ex(context.context, cipher, nullptr,
                          reinterpret_cast<const unsigned char*>(key.bytes.data()), iv, encrypt ? 1 : 0) != 1)
        return std::nullopt;
    EVP_CIPHER_CTX_set_padding(context.context, 1);
    std::string out(data.size() + ivSize * 2, '\0');
    int written = 0;
    unsigned char* outBytes = reinterpret_cast<unsigned char*>(out.data());
    if (EVP_CipherUpdate(context.context, outBytes, &written, reinterpret_cast<const unsigned char*>(data.data()),
                         static_cast<int>(data.size())) != 1)
        return std::nullopt;
    int extra = 0;
    if (EVP_CipherFinal_ex(context.context, outBytes + written, &extra) != 1)
        return std::nullopt;
    out.resize(static_cast<std::size_t>(written + extra));
    return out;
}

// crypt.encrypt(data, key, iv, mode): the data as base64 ciphertext, and the
// IV used -- a random one when the caller gave none, which is what makes the
// same plaintext come out differently each time.
int cryptEncrypt(lua_State* L)
{
    const std::string_view data = checkString(L, 1);
    const Key key = checkKey(L, "is not a 16, 24 or 32 byte key");
    const std::string_view mode = lua_type(L, 4) == LUA_TSTRING ? checkString(L, 4) : std::string_view{"CBC"};
    const EVP_CIPHER* cipher = cipherFor(key.bytes, mode);
    if (!cipher)
        luaL_argerror(L, 4, "is not an AES mode");
    Key withIv = key;
    if (withIv.iv.empty()) {
        std::array<unsigned char, ivSize> random{};
        if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
            raise(L, "no random bytes for the IV");
        withIv.iv.assign(reinterpret_cast<const char*>(random.data()), random.size());
    }
    auto encrypted = aes(cipher, withIv, data, true);
    if (!encrypted)
        raise(L, "the data could not be encrypted");
    const std::string text = base64(*encrypted);
    lua_pushlstring(L, text.data(), text.size());
    const std::string iv = base64(withIv.iv);
    lua_pushlstring(L, iv.data(), iv.size());
    return 2;
}

// crypt.decrypt(data, key, iv, mode): the plaintext of what encrypt made.
int cryptDecrypt(lua_State* L)
{
    auto bytes = unbase64(checkString(L, 1));
    if (!bytes)
        luaL_argerror(L, 1, "is not base64");
    const Key key = checkKey(L, "is not a 16, 24 or 32 byte key");
    const std::string_view mode = lua_type(L, 4) == LUA_TSTRING ? checkString(L, 4) : std::string_view{"CBC"};
    const EVP_CIPHER* cipher = cipherFor(key.bytes, mode);
    if (!cipher)
        luaL_argerror(L, 4, "is not an AES mode");
    auto decrypted = aes(cipher, key, *bytes, false);
    if (!decrypted)
        raise(L, "the data could not be decrypted");
    lua_pushlstring(L, decrypted->data(), decrypted->size());
    return 1;
}

// Bytes from the system's random source, as the executors' generatebytes:
// `count` of them, in base64.
int generateBytes(lua_State* L)
{
    const lua_Integer count = luaL_checkinteger(L, 1);
    if (count < 0 || count > (1 << 20))
        luaL_argerror(L, 1, "is not a byte count");
    std::string bytes(static_cast<std::size_t>(count), '\0');
    if (count > 0
        && RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), static_cast<int>(count)) != 1)
        raise(L, "no random bytes");
    const std::string text = base64(bytes);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// crypt.generatekey(): 32 random bytes, which is an AES-256 key, in base64.
int generateKey(lua_State* L)
{
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        raise(L, "no random bytes");
    const std::string text = base64(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// crypt.hash(data, algorithm): the digest of the data, as the hex text the
// executors' returns. The names are OpenSSL's, lowercased.
const EVP_MD* digestFor(std::string_view algorithm)
{
    struct Name {
        std::string_view name;
        const EVP_MD* (*digest)();
    };
    for (const Name& candidate : {Name{"md5", EVP_md5},
                                  Name{"sha1", EVP_sha1},
                                  Name{"sha224", EVP_sha224},
                                  Name{"sha256", EVP_sha256},
                                  Name{"sha384", EVP_sha384},
                                  Name{"sha512", EVP_sha512},
                                  Name{"sha3-224", EVP_sha3_224},
                                  Name{"sha3-256", EVP_sha3_256},
                                  Name{"sha3-384", EVP_sha3_384},
                                  Name{"sha3-512", EVP_sha3_512}}) {
        if (candidate.name == algorithm)
            return candidate.digest();
    }
    return nullptr;
}

// The digest of the data as lowercase hex, empty when it can't be made.
std::string hexDigest(std::string_view data, const EVP_MD* digest)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned length = 0;
    if (EVP_Digest(data.data(), data.size(), bytes.data(), &length, digest, nullptr) != 1)
        return {};
    static constexpr std::string_view hex = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (unsigned i = 0; i < length; ++i) {
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 0x0f]);
    }
    return out;
}

void pushDigest(lua_State* L, std::string_view data, const EVP_MD* digest)
{
    const std::string out = hexDigest(data, digest);
    if (out.empty())
        raise(L, "the data could not be hashed");
    lua_pushlstring(L, out.data(), out.size());
}

int cryptHash(lua_State* L)
{
    const std::string_view data = checkString(L, 1);
    const std::string_view algorithm = checkString(L, 2);
    const EVP_MD* digest = digestFor(algorithm);
    if (!digest)
        raise(L, std::format("'{}' is not a digest this knows", algorithm));
    pushDigest(L, data, digest);
    return 1;
}

// crypt.hmac(key, data, algorithm): the HMAC of the data under the key with
// one of crypt.hash's digests, in base64.
int cryptHmac(lua_State* L)
{
    const std::string_view key = checkString(L, 1);
    const std::string_view data = checkString(L, 2);
    const std::string_view algorithm = checkString(L, 3);
    const EVP_MD* digest = digestFor(algorithm);
    if (!digest)
        raise(L, std::format("'{}' is not a digest this knows", algorithm));
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned length = 0;
    if (!HMAC(digest, key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(data.data()),
              data.size(), bytes.data(), &length))
        raise(L, "the data could not be hashed");
    const std::string text = base64(std::string_view(reinterpret_cast<const char*>(bytes.data()), length));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// crypt.random(size): that many random characters, letters, digits and the
// printable ones before them; up to 1024 of them.
int cryptRandom(lua_State* L)
{
    const lua_Integer size = luaL_checkinteger(L, 1);
    if (size < 0 || size > 1024)
        raise(L, std::format("Expected number within range (0, 1024), got: {}", size));
    static constexpr std::string_view charset = "!\"#$%&'()*+,-./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size > 0 && RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), static_cast<int>(size)) != 1)
        raise(L, "no random bytes");
    for (char& c : bytes)
        c = charset[static_cast<unsigned char>(c) % charset.size()];
    lua_pushlstring(L, bytes.data(), bytes.size());
    return 1;
}

// getscripthash(script): the SHA-384 of the bytecode the script runs from
// (getscriptbytecode's), as hex: the same for the same code, another once its
// Source changes. Nothing for a script with no code at all.
int getScriptHash(lua_State* L)
{
    const Instance& script = checkInstance(L, 1);
    const auto* container = dynamic_cast<const LuaSourceContainer*>(&script);
    if (!container)
        luaL_typeerror(L, 1, "LuaSourceContainer");
    if (container->source().empty())
        return 0;
    auto bytecode = Runtime::of(L).codeOf(container->source(), script.fullName());
    if (!bytecode)
        raise(L, bytecode.error());
    pushDigest(L, *bytecode, EVP_sha384());
    return 1;
}

// getfunctionhash(f): functionHash of a Luau function; a C function has no
// code to hash ["lua function expected", as the sUNC docs word it].
int getFunctionHash(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    const Closure* closure = clvalue(luaA_toobject(L, 1));
    if (closure->isC)
        luaL_argerror(L, 1, "lua function expected");
    const std::string hash = functionHash(closure->l.p);
    lua_pushlstring(L, hash.data(), hash.size());
    return 1;
}

} // namespace

std::string functionHash(const Proto* proto)
{
    // The constants first, each as text, then the instructions as they are
    // in memory: the layout the sUNC docs point to (Dottik's and Ragnar's),
    // so one function hashes the same across executors.
    std::string data;
    for (int i = 0; i < proto->sizek; ++i) {
        const TValue* k = &proto->k[i];
        switch (ttype(k)) {
        case LUA_TSTRING:
            data.append(svalue(k), tsvalue(k)->len);
            break;
        case LUA_TNUMBER:
            data += std::to_string(nvalue(k));
            break;
        case LUA_TBOOLEAN:
            data += bvalue(k) ? "true" : "false";
            break;
        case LUA_TNIL:
            data += "nil";
            break;
        default:
            break;
        }
    }
    data.append(reinterpret_cast<const char*>(proto->code), static_cast<std::size_t>(proto->sizecode) * sizeof(Instruction));
    return hexDigest(data, EVP_sha384());
}

void openConsoleCrypt(lua_State* thread)
{
    lua_newtable(thread);
    const int crypt = lua_gettop(thread);
    for (const auto& [name, function] :
         {std::pair{"base64encode", base64Encode},
          std::pair{"base64decode", base64Decode},
          std::pair{"encrypt", cryptEncrypt},
          std::pair{"decrypt", cryptDecrypt},
          std::pair{"generatebytes", generateBytes},
          std::pair{"generatekey", generateKey},
          std::pair{"hash", cryptHash},
          std::pair{"hmac", cryptHmac},
          std::pair{"random", cryptRandom}}) {
        lua_pushcfunction(thread, function, name);
        lua_setfield(thread, crypt, name);
    }

    // The other spellings the executors give the same two functions: crypt
    // .base64.encode and crypt.base64_encode beside them, and a base64 table
    // of its own with base64encode, base64decode, base64_encode and
    // base64_decode as globals (sUNC tests the first two, not crypt's).
    lua_newtable(thread);   // crypt.base64
    const int nested = lua_gettop(thread);
    lua_newtable(thread);   // the base64 global
    const int plain = lua_gettop(thread);
    for (const std::string_view name : {"encode", "decode"}) {
        const std::string underscored = std::format("base64_{}", name);
        lua_getfield(thread, crypt, std::format("base64{}", name).c_str());
        lua_pushvalue(thread, -1);
        lua_setfield(thread, plain, name.data());
        lua_pushvalue(thread, -1);
        lua_setfield(thread, nested, name.data());
        lua_pushvalue(thread, -1);
        lua_setfield(thread, crypt, underscored.c_str());
        lua_pushvalue(thread, -1);
        lua_setglobal(thread, std::format("base64{}", name).c_str());
        lua_setglobal(thread, underscored.c_str());
    }
    lua_setglobal(thread, "base64");
    lua_setfield(thread, crypt, "base64");
    lua_setglobal(thread, "crypt");

    lua_pushcfunction(thread, getScriptHash, "getscripthash");
    lua_setglobal(thread, "getscripthash");
    lua_pushcfunction(thread, getFunctionHash, "getfunctionhash");
    lua_setglobal(thread, "getfunctionhash");
}

} // namespace lunar::lua

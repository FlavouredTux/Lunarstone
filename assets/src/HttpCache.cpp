#include <lunar/assets/DerivedCache.hpp>
#include <lunar/assets/HttpCache.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lunar::assets {

namespace {

constexpr std::string_view magic = "lunarstone-http 1\n";

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

bool equalsLower(std::string_view text, std::string_view lower)
{
    return std::ranges::equal(text, lower, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    });
}

std::optional<std::int64_t> number(std::string_view text)
{
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

std::int64_t nowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Everything the answer may depend on, which is what a file is found by.
std::string keyOf(const HttpRequest& request)
{
    std::string key = std::format("{} {}\nredirects {} plain {}\n", request.method, request.url,
                                  request.followRedirects, request.plainHttp);
    for (const std::string& header : request.headers)
        key += header + '\n';
    return key;
}

std::filesystem::path fileOf(const std::filesystem::path& dir, std::string_view key)
{
    std::uint64_t hash = 14695981039346656037ull; // FNV-1a; the key in the file settles a clash
    for (const char c : key)
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return dir / std::format("{:016x}.http", hash);
}

// A kept answer: the magic, then a line each for when it goes stale (Unix
// seconds), its status, the key's length and the headers' count, then the
// key, a line per header and the body.
std::string encode(std::int64_t expires, std::string_view key, const HttpResponse& response)
{
    std::string out(magic);
    out += std::format("{}\n{}\n{}\n{}\n", expires, response.status, key.size(), response.headers.size());
    out += key;
    for (const auto& [name, value] : response.headers)
        out += std::format("{}: {}\n", name, value);
    return out;
}

struct Cursor {
    std::string_view text;
    std::optional<std::string_view> line()
    {
        const std::size_t end = text.find('\n');
        if (end == std::string_view::npos)
            return std::nullopt;
        const std::string_view out = text.substr(0, end);
        text.remove_prefix(end + 1);
        return out;
    }
    std::optional<std::int64_t> numberLine()
    {
        const auto l = line();
        return l ? number(*l) : std::nullopt;
    }
};

// When the file at path goes stale, from its first lines alone.
std::optional<std::int64_t> expiryOf(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::array<char, 64> head{};
    file.read(head.data(), head.size());
    Cursor cursor{std::string_view(head.data(), static_cast<std::size_t>(file.gcount()))};
    if (!cursor.text.starts_with(magic))
        return std::nullopt;
    cursor.text.remove_prefix(magic.size());
    return cursor.numberLine();
}

std::optional<HttpResponse> read(const std::filesystem::path& path, std::string_view key)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return std::nullopt;
    const std::streamoff size = file.tellg();
    if (size <= 0)
        return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    if (!file.read(bytes.data(), size))
        return std::nullopt;
    Cursor cursor{bytes};
    if (!cursor.text.starts_with(magic))
        return std::nullopt;
    cursor.text.remove_prefix(magic.size());
    const auto expires = cursor.numberLine();
    const auto status = cursor.numberLine();
    const auto keySize = cursor.numberLine();
    const auto headerCount = cursor.numberLine();
    if (!expires || !status || !keySize || !headerCount || *expires <= nowSeconds() || *keySize < 0
        || static_cast<std::size_t>(*keySize) > cursor.text.size()
        || cursor.text.substr(0, static_cast<std::size_t>(*keySize)) != key)
        return std::nullopt;
    cursor.text.remove_prefix(static_cast<std::size_t>(*keySize));
    HttpResponse response;
    response.status = static_cast<long>(*status);
    for (std::int64_t i = 0; i < *headerCount; ++i) {
        const auto header = cursor.line();
        const std::size_t colon = header ? header->find(": ") : std::string_view::npos;
        if (colon == std::string_view::npos)
            return std::nullopt;
        response.headers.emplace_back(std::string(header->substr(0, colon)), std::string(header->substr(colon + 2)));
    }
    response.body = std::string(cursor.text);
    return response;
}

std::atomic<bool> prunedOnce = false; // by this process, so a long-lived cache doesn't only grow

} // namespace

HttpCache::HttpCache(std::filesystem::path dir, std::uint64_t maxBytes) : dir_(std::move(dir)), maxBytes_(maxBytes) {}

std::optional<std::chrono::seconds> HttpCache::freshness(const HttpResponse& response)
{
    if (response.status != 200)
        return std::nullopt;
    std::optional<std::int64_t> maxAge;
    for (const auto& [name, value] : response.headers) {
        if (name == "vary" && trim(value) == "*")
            return std::nullopt;
        if (name != "cache-control")
            continue;
        std::string_view rest = value;
        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            const std::string_view directive = trim(rest.substr(0, comma));
            rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
            if (equalsLower(directive, "no-store") || equalsLower(directive, "no-cache"))
                return std::nullopt;
            if (directive.size() > 8 && equalsLower(directive.substr(0, 8), "max-age=")) {
                std::string_view seconds = trim(directive.substr(8));
                if (seconds.size() >= 2 && seconds.front() == '"' && seconds.back() == '"')
                    seconds = seconds.substr(1, seconds.size() - 2);
                maxAge = number(seconds);
            }
        }
    }
    if (!maxAge)
        return std::nullopt;
    std::int64_t age = 0;
    if (const std::string* header = response.header("age"))
        age = number(trim(*header)).value_or(0);
    if (*maxAge - age <= 0)
        return std::nullopt;
    return std::chrono::seconds(*maxAge - age);
}

std::expected<HttpResponse, std::string> HttpCache::get(const HttpTransport& transport, const HttpRequest& request,
                                                        bool reload) const
{
    if (dir_.empty() || request.method != "GET")
        return transport(request);
    const std::string key = keyOf(request);
    const std::filesystem::path path = fileOf(dir_, key);
    if (!reload)
        if (std::optional<HttpResponse> kept = read(path, key))
            return std::move(*kept);
    std::expected<HttpResponse, std::string> response = transport(request);
    if (!response)
        return response;
    const std::optional<std::chrono::seconds> fresh = freshness(*response);
    if (!fresh || response->body.size() > maxBytes_ / 4)
        return response;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    const std::string head = encode(nowSeconds() + fresh->count(), key, *response);
    const std::array<std::span<const std::uint8_t>, 2> parts{
        std::span(reinterpret_cast<const std::uint8_t*>(head.data()), head.size()),
        std::span(reinterpret_cast<const std::uint8_t*>(response->body.data()), response->body.size())};
    (void)writeWhole(path, parts); // a cache that can't be written only costs the next request
    if (!prunedOnce.exchange(true))
        prune();
    return response;
}

void HttpCache::prune() const
{
    struct Kept {
        std::filesystem::path path;
        std::uint64_t size;
        std::filesystem::file_time_type written;
    };
    std::vector<Kept> kept;
    std::uint64_t total = 0;
    const std::int64_t now = nowSeconds();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (entry.path().extension() != ".http" || !entry.is_regular_file(ec))
            continue;
        const std::optional<std::int64_t> expires = expiryOf(entry.path());
        if (!expires || *expires <= now) {
            std::filesystem::remove(entry.path(), ec);
            continue;
        }
        kept.push_back({entry.path(), entry.file_size(ec), entry.last_write_time(ec)});
        total += kept.back().size;
    }
    std::ranges::sort(kept, {}, &Kept::written);
    for (const Kept& file : kept) {
        if (total <= maxBytes_)
            break;
        std::filesystem::remove(file.path, ec);
        total -= file.size;
    }
}

} // namespace lunar::assets

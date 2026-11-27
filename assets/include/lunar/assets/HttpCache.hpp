#pragma once

#include <lunar/assets/Web.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace lunar::assets {

// A private HTTP cache, as WinInet's was under Roblox's game:HttpGet (its
// nocache argument, which the executors' copies kept, is for getting past
// it): a GET's 200 is kept on disk for as long as its Cache-Control max-age
// says, less its Age, and the same request made again within that is
// answered from there. An answer that says no-store or no-cache or varies
// on everything isn't kept, nor one that doesn't say for how long (no
// guessing from Last-Modified). A file per request, named
// after it (method, URL, headers); at most maxBytes of them stay, the
// expired going first, then the oldest.
class HttpCache {
public:
    explicit HttpCache(std::filesystem::path dir, std::uint64_t maxBytes = std::uint64_t{256} << 20);

    // request's answer: the kept one while it's fresh, unless reload, else
    // transport's, kept when it may be. Only GETs are kept; others go
    // straight through. The first answer this process keeps prunes the
    // directory.
    std::expected<HttpResponse, std::string> get(const HttpTransport& transport, const HttpRequest& request,
                                                 bool reload = false) const;

    // Takes out the expired files, then the oldest until the rest fit.
    void prune() const;

    // How long from now response may be kept, or nothing when it may not.
    static std::optional<std::chrono::seconds> freshness(const HttpResponse& response);

private:
    std::filesystem::path dir_;
    std::uint64_t maxBytes_;
};

} // namespace lunar::assets

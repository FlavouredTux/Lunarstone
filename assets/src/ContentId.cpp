#include <lunar/assets/ContentId.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace lunar::assets {

namespace {

std::string lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view text)
{
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && space(text.back()))
        text.remove_suffix(1);
    return text;
}

std::expected<std::uint64_t, std::string> parseId(std::string_view digits, std::string_view uri)
{
    std::uint64_t id = 0;
    auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (ec != std::errc{} || end != digits.data() + digits.size() || id == 0)
        return std::unexpected(std::format("\"{}\" has no valid asset id", uri));
    return id;
}

// One query parameter, case-insensitively. The key includes its '='.
std::string_view queryValue(std::string_view query, std::string_view lowered, std::string_view key)
{
    std::size_t at = 0;
    while (at < lowered.size()) {
        const std::size_t end = std::min(lowered.find('&', at), lowered.size());
        if (lowered.substr(at, end - at).starts_with(key))
            return query.substr(at + key.size(), end - at - key.size());
        at = end + 1;
    }
    return {};
}

} // namespace

std::string ContentId::key() const
{
    switch (kind) {
    case Kind::Builtin:
        return "rbxasset://" + path;
    case Kind::Asset:
        return std::format("rbxassetid://{}", id);
    case Kind::File:
    case Kind::Cdn:
        return path;
    case Kind::Thumbnail:
        return std::format("rbxthumb://type={}&id={}&w={}&h={}{}", path, id, width, height,
                           circular ? "&filters=circular" : "");
    }
    return path;
}

std::expected<ContentId, std::string> parseContentId(std::string_view uri)
{
    uri = trim(uri);
    if (uri.empty())
        return std::unexpected("empty content id");
    const std::string lowered = lower(uri);
    const std::string_view low = lowered;

    if (low.starts_with("rbxassetid://")) {
        // Games also append cache-busting parameters directly with '&'
        // (the game's ItemModule), without the leading '?'. The numeric asset
        // still uses the same cache entry as its other URI spellings.
        std::string_view digits = uri.substr(13);
        digits = digits.substr(0, digits.find_first_of("?&/#"));
        auto id = parseId(digits, uri);
        if (!id)
            return std::unexpected(id.error());
        return ContentId{.kind = ContentId::Kind::Asset, .id = *id, .path = {}};
    }

    if (low.starts_with("rbxthumb://")) {
        // type=T&id=N&w=W&h=H, other parameters (timestamp, includeBackground)
        // aside; filters=circular rounds it.
        ContentId thumb{.kind = ContentId::Kind::Thumbnail, .id = 0, .path = {}};
        std::string_view query = uri.substr(11);
        bool haveId = false;
        while (!query.empty()) {
            const std::string_view pair = query.substr(0, query.find('&'));
            query.remove_prefix(std::min(query.size(), pair.size() + 1));
            const std::size_t equals = pair.find('=');
            const std::string_view key = pair.substr(0, equals);
            const std::string_view value
                = equals == std::string_view::npos ? std::string_view() : pair.substr(equals + 1);
            auto number = [&](auto& out) {
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
                return ec == std::errc() && end == value.data() + value.size();
            };
            if (key == "type")
                thumb.path = std::string(value);
            else if (key == "id")
                haveId = number(thumb.id);
            else if ((key == "w" && !number(thumb.width)) || (key == "h" && !number(thumb.height)))
                return std::unexpected(std::format("\"{}\" has a bad size", uri));
            else if (key == "filters")
                thumb.circular = value == "circular";
        }
        if (thumb.path.empty() || !haveId || thumb.width == 0 || thumb.height == 0 || thumb.width > 4096
            || thumb.height > 4096)
            return std::unexpected(std::format("\"{}\" needs a type, an id and a size", uri));
        return thumb;
    }

    if (low.starts_with("rbxasset://")) {
        std::string path(uri.substr(11));
        std::ranges::replace(path, '\\', '/');
        while (path.starts_with('/'))
            path.erase(0, 1);
        if (path.empty())
            return std::unexpected(std::format("\"{}\" names no file", uri));
        // Built-in content must stay inside the content folder.
        if (path == ".." || path.starts_with("../") || path.contains("/../") || path.ends_with("/.."))
            return std::unexpected(std::format("\"{}\" leaves the content folder", uri));
        return ContentId{.kind = ContentId::Kind::Builtin, .id = 0, .path = std::move(path)};
    }

    if (low.starts_with("file://"))
        return ContentId{.kind = ContentId::Kind::File, .id = 0, .path = std::string(uri.substr(7))};

    for (std::string_view scheme : {"http://", "https://"}) {
        if (!low.starts_with(scheme))
            continue;
        const std::string_view rest = low.substr(scheme.size());
        const std::string_view host = rest.substr(0, rest.find_first_of("/?#"));
        // Roblox's CDN, over https only: the URLs the thumbnails API gives.
        if (scheme == "https://" && host.ends_with(".rbxcdn.com") && !host.contains('@') && !host.contains(':'))
            return ContentId{.kind = ContentId::Kind::Cdn, .id = 0, .path = std::string(uri)};
        if (host != "roblox.com" && !host.ends_with(".roblox.com"))
            return std::unexpected(std::format("\"{}\" isn't a Roblox asset URL", uri));
        const std::size_t question = low.find('?');
        if (question == std::string_view::npos)
            return std::unexpected(std::format("\"{}\" has no asset id", uri));
        const std::string_view path = low.substr(scheme.size() + host.size(), question - scheme.size() - host.size());
        if (path == "/thumbs/asset.ashx") {
            // Legacy inventory modules request rendered asset previews through
            // this URL. It denotes a thumbnail, rather than the asset's model.
            const auto query = uri.substr(question + 1);
            const auto names = low.substr(question + 1);
            const auto id = parseId(queryValue(query, names, "assetid="), uri);
            if (!id)
                return std::unexpected(id.error());
            return parseContentId(std::format("rbxthumb://type=Asset&id={}&w={}&h={}", *id,
                                              queryValue(query, names, "width="), queryValue(query, names, "height=")));
        }
        if (!path.contains("asset"))
            return std::unexpected(std::format("\"{}\" isn't a Roblox asset URL", uri));
        auto id = parseId(queryValue(uri.substr(question + 1), low.substr(question + 1), "id="), uri);
        if (!id)
            return std::unexpected(id.error());
        return ContentId{.kind = ContentId::Kind::Asset, .id = *id, .path = {}};
    }

    if (low.contains("://"))
        return std::unexpected(std::format("\"{}\" isn't a mesh location this client understands", uri));
    return ContentId{.kind = ContentId::Kind::File, .id = 0, .path = std::string(uri)};
}

} // namespace lunar::assets

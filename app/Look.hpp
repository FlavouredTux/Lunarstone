#pragma once

#include <lunar/Math.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// "x,y,z,x,y,z" (the --look option) -> eye and target.
inline std::optional<std::pair<lunar::Vector3, lunar::Vector3>> parseLook(std::string_view text)
{
    float v[6]{};
    std::size_t at = 0;
    for (float& value : v) {
        const std::size_t comma = std::min(text.find(',', at), text.size());
        const std::string part(text.substr(at, comma - at));
        if (part.empty())
            return std::nullopt;
        value = std::stof(part);
        at = comma + 1;
    }
    return std::pair{lunar::Vector3{v[0], v[1], v[2]}, lunar::Vector3{v[3], v[4], v[5]}};
}

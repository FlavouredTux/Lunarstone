#pragma once

// A small JSON reader for Roblox's web API replies. Builds a tree; integers
// stay exact (user and asset ids go past what a float holds), other numbers
// are doubles.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lunar::assets::json {

struct Value;
using Object = std::map<std::string, Value, std::less<>>;
using Array = std::vector<Value>;

struct Value {
    std::variant<std::monostate, bool, std::int64_t, double, std::string, std::shared_ptr<Array>,
                 std::shared_ptr<Object>>
        data;

    // The member called `key`, or null when this isn't an object or has none.
    const Value* operator[](std::string_view key) const
    {
        if (const auto* object = std::get_if<std::shared_ptr<Object>>(&data)) {
            auto it = (*object)->find(key);
            if (it != (*object)->end())
                return &it->second;
        }
        return nullptr;
    }
    const Object* object() const
    {
        const auto* o = std::get_if<std::shared_ptr<Object>>(&data);
        return o ? o->get() : nullptr;
    }
    const Array* array() const
    {
        const auto* a = std::get_if<std::shared_ptr<Array>>(&data);
        return a ? a->get() : nullptr;
    }
    const std::string* string() const { return std::get_if<std::string>(&data); }
    std::optional<bool> boolean() const
    {
        if (const auto* b = std::get_if<bool>(&data))
            return *b;
        return std::nullopt;
    }
    std::optional<double> number() const
    {
        if (const auto* d = std::get_if<double>(&data))
            return *d;
        if (const auto* i = std::get_if<std::int64_t>(&data))
            return static_cast<double>(*i);
        return std::nullopt;
    }
    // A whole number, written as one (12) or as a float with nothing after
    // the point (12.0).
    std::optional<std::int64_t> integer() const
    {
        if (const auto* i = std::get_if<std::int64_t>(&data))
            return *i;
        if (const auto* d = std::get_if<double>(&data);
            d && *d == static_cast<double>(static_cast<std::int64_t>(*d)) && *d > -9.2e18 && *d < 9.2e18)
            return static_cast<std::int64_t>(*d);
        return std::nullopt;
    }
};

// Null for anything that isn't one whole JSON document.
std::optional<Value> parse(std::string_view text);

} // namespace lunar::assets::json

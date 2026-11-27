#pragma once

// A small JSON reader for the client's font family files and similar
// configuration. Builds a tree; numbers are doubles.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lunar::ui::json {

struct Value;
using Object = std::map<std::string, Value, std::less<>>;
using Array = std::vector<Value>;

struct Value {
    std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> data;

    const Value* operator[](std::string_view key) const
    {
        if (const auto* object = std::get_if<std::shared_ptr<Object>>(&data)) {
            auto it = (*object)->find(key);
            if (it != (*object)->end())
                return &it->second;
        }
        return nullptr;
    }
    const Array* array() const
    {
        const auto* a = std::get_if<std::shared_ptr<Array>>(&data);
        return a ? a->get() : nullptr;
    }
    const std::string* string() const { return std::get_if<std::string>(&data); }
    std::optional<double> number() const
    {
        if (const auto* d = std::get_if<double>(&data))
            return *d;
        return std::nullopt;
    }
};

std::optional<Value> parse(std::string_view text);

} // namespace lunar::ui::json

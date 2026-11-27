#pragma once

#include <expected>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Just enough XML for Roblox files: elements, attributes, text, CDATA,
// comments and the standard entities. No DTDs or namespaces.
namespace lunar::xml {

struct Element {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<Element> children;
    std::string text; // character data directly inside, entities decoded, CDATA included

    const std::string* attribute(std::string_view key) const;
    const Element* child(std::string_view childName) const;

    auto childrenNamed(std::string_view childName) const
    {
        return children | std::views::filter([childName](const Element& e) { return e.name == childName; });
    }
};

struct ParseError {
    std::string message;
    int line = 0;
};

// Returns the root element.
std::expected<Element, ParseError> parse(std::string_view document);

} // namespace lunar::xml

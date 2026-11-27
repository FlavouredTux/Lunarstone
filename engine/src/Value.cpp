#include <lunar/Value.hpp>

#include <lunar/Error.hpp>
#include <lunar/Instance.hpp>

#include <array>
#include <format>

namespace lunar {

namespace {

template <class... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};

std::string udimString(UDim u)
{
    return std::format("{}, {}", u.scale, u.offset);
}

std::string facesString(const Faces& f)
{
    const std::array<std::pair<bool, std::string_view>, 6> names{{{f.right, "Right"},
                                                                  {f.top, "Top"},
                                                                  {f.back, "Back"},
                                                                  {f.left, "Left"},
                                                                  {f.bottom, "Bottom"},
                                                                  {f.front, "Front"}}};
    std::string out;
    for (auto [set, name] : names)
        if (set)
            out += out.empty() ? std::string(name) : std::format(", {}", name);
    return out;
}

std::string axesString(const Axes& a)
{
    std::string out;
    for (auto [set, name] : {std::pair{a.x, "X"}, std::pair{a.y, "Y"}, std::pair{a.z, "Z"}})
        if (set)
            out += out.empty() ? std::string(name) : std::format(", {}", name);
    return out;
}

} // namespace

void detail::throwBadEnum(std::string_view property, const EnumInfo& info, const Value& value)
{
    std::string shown
        = std::holds_alternative<std::string>(value) ? std::format("\"{}\"", toString(value)) : toString(value);
    throw Error(std::format("Unable to assign property {}. Invalid value {} for enum {}", property, shown, info.name));
}

std::string_view typeName(const Value& value)
{
    return std::visit(Overloaded{
                          [](std::monostate) { return std::string_view("nil"); },
                          [](bool) { return std::string_view("boolean"); },
                          [](double) { return std::string_view("number"); },
                          [](const std::string&) { return std::string_view("string"); },
                          [](const Vector3&) { return std::string_view("Vector3"); },
                          [](const Color3&) { return std::string_view("Color3"); },
                          [](const CFrame&) { return std::string_view("CFrame"); },
                          [](const InstancePtr&) { return std::string_view("Instance"); },
                          [](const Vector2&) { return std::string_view("Vector2"); },
                          [](const UDim&) { return std::string_view("UDim"); },
                          [](const UDim2&) { return std::string_view("UDim2"); },
                          [](const Rect&) { return std::string_view("Rect"); },
                          [](const NumberRange&) { return std::string_view("NumberRange"); },
                          [](const NumberSequence&) { return std::string_view("NumberSequence"); },
                          [](const ColorSequence&) { return std::string_view("ColorSequence"); },
                          [](const BrickColor&) { return std::string_view("BrickColor"); },
                          [](const EnumItem&) { return std::string_view("EnumItem"); },
                          [](const Faces&) { return std::string_view("Faces"); },
                          [](const Axes&) { return std::string_view("Axes"); },
                          [](const PhysicalProperties&) { return std::string_view("PhysicalProperties"); },
                          [](const Vector3int16&) { return std::string_view("Vector3int16"); },
                          [](const Font&) { return std::string_view("Font"); },
                          [](const Buffer&) { return std::string_view("buffer"); },
                      },
                      value);
}

std::string toString(const Value& value)
{
    return std::visit(
        Overloaded{
            [](std::monostate) { return std::string("nil"); },
            [](bool b) { return std::string(b ? "true" : "false"); },
            [](double d) { return std::format("{}", d); },
            [](const std::string& s) { return s; },
            [](const Vector3& v) { return std::format("{}, {}, {}", v.x, v.y, v.z); },
            [](const Color3& c) { return std::format("{}, {}, {}", c.r, c.g, c.b); },
            [](const CFrame& cf) {
                const auto& r = cf.r;
                return std::format("{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}", cf.position.x, cf.position.y,
                                   cf.position.z, r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2], r[2][0],
                                   r[2][1], r[2][2]);
            },
            [](const InstancePtr& i) { return i ? i->name() : std::string("nil"); },
            [](const Vector2& v) { return std::format("{}, {}", v.x, v.y); },
            [](const UDim& u) { return udimString(u); },
            [](const UDim2& u) { return std::format("{{{}}}, {{{}}}", udimString(u.x), udimString(u.y)); },
            [](const Rect& r) { return std::format("{}, {}, {}, {}", r.min.x, r.min.y, r.max.x, r.max.y); },
            [](const NumberRange& r) { return std::format("{} {} ", r.min, r.max); },
            [](const NumberSequence& s) {
                std::string out;
                for (const auto& k : s.keypoints)
                    out += std::format("{} {} {} ", k.time, k.value, k.envelope);
                return out;
            },
            [](const ColorSequence& s) {
                std::string out;
                for (const auto& k : s.keypoints)
                    out += std::format("{} {} {} {} 0 ", k.time, k.value.r, k.value.g, k.value.b);
                return out;
            },
            [](const BrickColor& b) { return std::string(b.name()); },
            [](const EnumItem& e) { return std::format("Enum.{}.{}", e.enumType ? e.enumType->name : "?", e.name); },
            [](const Faces& f) { return facesString(f); },
            [](const Axes& a) { return axesString(a); },
            [](const PhysicalProperties& p) {
                return std::format("{}, {}, {}, {}, {}", p.density, p.friction, p.elasticity, p.frictionWeight,
                                   p.elasticityWeight);
            },
            [](const Vector3int16& v) { return std::format("{}, {}, {}", v.x, v.y, v.z); },
            [](const Font& f) {
                return std::format("Font {{ Family = {}, Weight = {}, Style = {} }}", f.family,
                                   enums::item(f.weight).name, enums::item(f.style).name);
            },
            [](const Buffer& b) { return std::format("buffer: {} bytes", b.bytes.size()); },
        },
        value);
}

} // namespace lunar

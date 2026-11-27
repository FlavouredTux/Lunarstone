#include <lunar/Rbxlx.hpp>

#include "Base64.hpp"
#include "Builder.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Xml.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <memory>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

#if __cpp_impl_reflection
#include <meta>
#endif

namespace lunar::rbxlx {

namespace {

std::string_view trim(std::string_view s)
{
    constexpr std::string_view space = " \t\r\n";
    std::size_t first = s.find_first_not_of(space);
    if (first == std::string_view::npos)
        return {};
    return s.substr(first, s.find_last_not_of(space) - first + 1);
}

template <class T>
std::expected<T, std::string> parseNumber(std::string_view text)
{
    std::string_view s = trim(text);
    T value{};
    auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (s.empty() || error != std::errc{} || end != s.data() + s.size())
        return std::unexpected(std::format("'{}' is not a number", s));
    return value;
}

#if __cpp_impl_reflection
consteval std::string_view upperName(std::meta::info member)
{
    std::string name(std::meta::identifier_of(member));
    name[0] = detail::toUpper(name[0]);
    return std::define_static_string(name);
}
#endif

// <Vector3><X/><Y/><Z/></Vector3>, <Color3><R/><G/><B/></Color3>: one child per
// struct member, named after it in upper case.
template <class T>
std::expected<T, std::string> decodeFields([[maybe_unused]] const xml::Element& element)
{
    T result{};
#if __cpp_impl_reflection
    constexpr auto ctx = std::meta::access_context::unchecked();
    static constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx));
    // clang-format doesn't know expansion statements yet.
    // clang-format off
    template for (constexpr std::meta::info member : members) {
        constexpr std::string_view tag = upperName(member);
        const xml::Element* field = element.child(tag);
        if (!field)
            return std::unexpected(std::format("missing <{}>", tag));
        auto number = parseNumber<typename[:std::meta::type_of(member):]>(field->text);
        if (!number)
            return std::unexpected(number.error());
        result.[:member:] = *number;
    }
    // clang-format on
#endif
    return result;
}

std::expected<CFrame, std::string> decodeCFrame(const xml::Element& element)
{
    static constexpr std::array<std::string_view, 12> tags{"X",   "Y",   "Z",   "R00", "R01", "R02",
                                                           "R10", "R11", "R12", "R20", "R21", "R22"};
    CFrame cf;
    std::array<float*, 12> targets{&cf.position.x, &cf.position.y, &cf.position.z, &cf.r[0][0],
                                   &cf.r[0][1],    &cf.r[0][2],    &cf.r[1][0],    &cf.r[1][1],
                                   &cf.r[1][2],    &cf.r[2][0],    &cf.r[2][1],    &cf.r[2][2]};
    for (auto [tag, target] : std::views::zip(tags, targets)) {
        const xml::Element* field = element.child(tag);
        if (!field)
            return std::unexpected(std::format("missing <{}>", tag));
        auto number = parseNumber<float>(field->text);
        if (!number)
            return std::unexpected(number.error());
        *target = *number;
    }
    return cf;
}

using Expected = std::expected<Value, std::string>;
using SharedStrings = std::unordered_map<std::string, SharedBytes>;

template <class T>
std::expected<T, std::string> childNumber(const xml::Element& element, std::string_view tag)
{
    const xml::Element* child = element.child(tag);
    if (!child)
        return std::unexpected(std::format("missing <{}>", tag));
    return parseNumber<T>(child->text);
}

// "1 2 3 " style lists, used by NumberRange and the sequences.
std::expected<std::vector<float>, std::string> numberList(std::string_view text)
{
    std::vector<float> numbers;
    for (auto word : text | std::views::split(' ')) {
        std::string_view token = trim(std::string_view(word));
        if (token.empty())
            continue;
        auto number = parseNumber<float>(token);
        if (!number)
            return std::unexpected(number.error());
        numbers.push_back(*number);
    }
    return numbers;
}

Expected decodeUDim(const xml::Element& element, std::string_view scaleTag, std::string_view offsetTag, UDim& out)
{
    auto scale = childNumber<float>(element, scaleTag);
    auto offset = childNumber<std::int32_t>(element, offsetTag);
    if (!scale)
        return std::unexpected(scale.error());
    if (!offset)
        return std::unexpected(offset.error());
    out = {*scale, *offset};
    return Value();
}

Expected decodeRect(const xml::Element& element)
{
    const xml::Element* min = element.child("min");
    const xml::Element* max = element.child("max");
    if (!min || !max)
        return std::unexpected("missing <min> or <max>");
    auto lo = decodeFields<Vector2>(*min);
    auto hi = decodeFields<Vector2>(*max);
    if (!lo)
        return std::unexpected(lo.error());
    if (!hi)
        return std::unexpected(hi.error());
    return Value(Rect{*lo, *hi});
}

Expected decodePhysicalProperties(const xml::Element& element)
{
    const xml::Element* custom = element.child("CustomPhysics");
    if (!custom || trim(custom->text) != "true")
        return Value(); // nil: the material's defaults
    PhysicalProperties p;
    struct Field {
        std::string_view tag;
        float* target;
        bool required;
    };
    for (auto [tag, target, required] :
         {Field{"Density", &p.density, true}, Field{"Friction", &p.friction, true},
          Field{"Elasticity", &p.elasticity, true}, Field{"FrictionWeight", &p.frictionWeight, true},
          Field{"ElasticityWeight", &p.elasticityWeight, true},
          Field{"AcousticAbsorption", &p.acousticAbsorption, false}}) {
        if (!required && !element.child(tag))
            continue;
        auto number = childNumber<float>(element, tag);
        if (!number)
            return std::unexpected(number.error());
        *target = *number;
    }
    return Value(p);
}

// <Content><url>rbxassetid://1</url></Content>, or <uri>, or <null/> for none.
std::string contentText(const xml::Element& element)
{
    for (std::string_view tag : {"url", "uri"})
        if (const xml::Element* url = element.child(tag))
            return url->text;
    return std::string(trim(element.text));
}

Expected decodeFont(const xml::Element& element)
{
    Font font;
    if (const xml::Element* family = element.child("Family"))
        font.family = contentText(*family);
    if (const xml::Element* weight = element.child("Weight")) {
        auto number = parseNumber<std::uint32_t>(weight->text);
        if (!number || !enums::info<Enum::FontWeight>().find(*number))
            return std::unexpected(std::format("bad font weight '{}'", trim(weight->text)));
        font.weight = static_cast<Enum::FontWeight>(*number);
    }
    if (const xml::Element* style = element.child("Style")) {
        const EnumItem* item = enums::info<Enum::FontStyle>().find(trim(style->text));
        if (!item)
            return std::unexpected(std::format("bad font style '{}'", trim(style->text)));
        font.style = static_cast<Enum::FontStyle>(item->value);
    }
    return Value(std::move(font));
}

Expected decodeColor3(const xml::Element& element)
{
    if (element.children.empty()) // old files: packed 0xAARRGGBB
        return parseNumber<std::uint32_t>(element.text).transform([](std::uint32_t argb) {
            auto channel = [argb](int shift) { return static_cast<int>((argb >> shift) & 0xFF); };
            return Value(Color3::fromRGB(channel(16), channel(8), channel(0)));
        });
    return decodeFields<Color3>(element).transform([](Color3 c) { return Value(c); });
}

Expected decodeValue(const xml::Element& element, const SharedStrings& shared)
{
    const std::string& tag = element.name;
    auto toValue = [](auto v) { return Value(std::move(v)); };

    if (tag == "string" || tag == "ProtectedString")
        return Value(element.text);
    if (tag == "bool") {
        std::string_view text = trim(element.text);
        if (text == "true" || text == "false")
            return Value(text == "true");
        return std::unexpected(std::format("'{}' is not a bool", text));
    }
    if (tag == "float" || tag == "double" || tag == "int" || tag == "int64" || tag == "token"
        || tag == "SecurityCapabilities")
        return parseNumber<double>(element.text).transform(toValue);
    if (tag == "BinaryString") {
        auto bytes = detail::decodeBase64(element.text);
        if (!bytes)
            return std::unexpected("bad base64 data");
        return Value(std::move(*bytes));
    }
    if (tag == "SharedString") {
        auto it = shared.find(std::string(trim(element.text)));
        if (it == shared.end())
            return std::unexpected(std::format("unknown shared string '{}'", trim(element.text)));
        return Value(*it->second);
    }
    if (tag == "Content")
        return Value(contentText(element));
    if (tag == "UniqueId")
        return Value(std::string(trim(element.text)));
    if (tag == "Vector3")
        return decodeFields<Vector3>(element).transform(toValue);
    if (tag == "Vector2")
        return decodeFields<Vector2>(element).transform(toValue);
    if (tag == "Vector3int16")
        return decodeFields<Vector3int16>(element).transform(toValue);
    if (tag == "Color3")
        return decodeColor3(element);
    if (tag == "Color3uint8") // 0xAARRGGBB
        return parseNumber<std::uint32_t>(element.text).transform([](std::uint32_t argb) {
            auto channel = [argb](int shift) { return static_cast<int>((argb >> shift) & 0xFF); };
            return Value(Color3::fromRGB(channel(16), channel(8), channel(0)));
        });
    if (tag == "BrickColor")
        return parseNumber<int>(element.text).transform([](int n) { return Value(BrickColor::fromNumber(n)); });
    if (tag == "CoordinateFrame")
        return decodeCFrame(element).transform(toValue);
    if (tag == "OptionalCoordinateFrame") {
        const xml::Element* cframe = element.child("CFrame");
        return cframe ? decodeCFrame(*cframe).transform(toValue) : Value();
    }
    if (tag == "UDim") {
        UDim u;
        return decodeUDim(element, "S", "O", u).transform([&](const Value&) { return Value(u); });
    }
    if (tag == "UDim2") {
        UDim2 u;
        auto x = decodeUDim(element, "XS", "XO", u.x);
        if (!x)
            return x;
        return decodeUDim(element, "YS", "YO", u.y).transform([&](const Value&) { return Value(u); });
    }
    if (tag == "Rect2D")
        return decodeRect(element);
    if (tag == "NumberRange")
        return numberList(element.text).and_then([](const std::vector<float>& n) -> Expected {
            if (n.size() < 2)
                return std::unexpected("a NumberRange needs two numbers");
            return Value(NumberRange{n[0], n[1]});
        });
    if (tag == "NumberSequence")
        return numberList(element.text).and_then([](const std::vector<float>& n) -> Expected {
            if (n.size() < 6 || n.size() % 3 != 0)
                return std::unexpected("a NumberSequence needs time/value/envelope triples");
            std::vector<NumberSequenceKeypoint> points;
            for (std::size_t i = 0; i < n.size(); i += 3)
                points.push_back({n[i], n[i + 1], n[i + 2]});
            return Value(NumberSequence(std::move(points)));
        });
    if (tag == "ColorSequence")
        return numberList(element.text).and_then([](const std::vector<float>& n) -> Expected {
            if (n.size() < 10 || n.size() % 5 != 0)
                return std::unexpected("a ColorSequence needs time/r/g/b/envelope groups");
            std::vector<ColorSequenceKeypoint> points;
            for (std::size_t i = 0; i < n.size(); i += 5)
                points.push_back({n[i], {n[i + 1], n[i + 2], n[i + 3]}});
            return Value(ColorSequence(std::move(points)));
        });
    if (tag == "Faces")
        return childNumber<std::uint8_t>(element, "faces").transform([](std::uint8_t bits) {
            return Value(Faces::fromBits(bits));
        });
    if (tag == "Axes")
        return childNumber<std::uint8_t>(element, "axes").transform([](std::uint8_t bits) {
            return Value(Axes::fromBits(bits));
        });
    if (tag == "PhysicalProperties")
        return decodePhysicalProperties(element);
    if (tag == "Font")
        return decodeFont(element);
    return std::unexpected(std::format("<{}> values aren't supported yet", tag));
}

class Loader {
public:
    detail::Builder builder;
    SharedStrings sharedStrings; // md5 key -> contents, from <SharedStrings>

    // <SharedStrings> sits after the items, but values refer to it.
    void readSharedStrings(const xml::Element& root)
    {
        for (const xml::Element& list : root.childrenNamed("SharedStrings"))
            for (const xml::Element& entry : list.childrenNamed("SharedString")) {
                const std::string* key = entry.attribute("md5");
                auto bytes = detail::decodeBase64(entry.text);
                if (key && bytes)
                    sharedStrings[*key] = std::make_shared<const std::string>(std::move(*bytes));
                else
                    builder.warnOnce("a malformed <SharedString> entry was skipped");
            }
    }

    // Builds the item and its subtree, unparented.
    InstancePtr build(const xml::Element& item)
    {
        const std::string* className = item.attribute("class");
        if (!className) {
            builder.warnOnce("an <Item> without a class was skipped");
            return nullptr;
        }
        InstancePtr instance = builder.create(*className);
        fill(*instance, item);
        return instance;
    }

    // Applies the item's properties and children to an existing instance.
    void fill(Instance& instance, const xml::Element& item)
    {
        if (const std::string* referent = item.attribute("referent"))
            byReferent_[*referent] = instance.shared_from_this();
        if (const xml::Element* properties = item.child("Properties"))
            for (const xml::Element& property : properties->children)
                applyProperty(instance, property);
        for (const xml::Element& childItem : item.childrenNamed("Item"))
            if (InstancePtr child = build(childItem))
                child->setParent(&instance);
    }

    // Ref properties can point anywhere in the file, so they go last.
    void resolveRefs()
    {
        for (const PendingRef& ref : pendingRefs_) {
            auto target = byReferent_.find(ref.referent);
            if (target == byReferent_.end())
                builder.warnOnce(std::format("{}.{} points at missing referent {}", ref.instance->className(),
                                             ref.property->name, ref.referent));
            else
                builder.assign(*ref.instance, *ref.property, Value(target->second));
        }
    }

private:
    struct PendingRef {
        InstancePtr instance;
        const Property* property;
        std::string referent;
    };

    void applyProperty(Instance& instance, const xml::Element& element)
    {
        const std::string* name = element.attribute("name");
        if (!name)
            return;
        if (element.name == "SharedString")
            if (auto set = detail::Builder::sharedSetter(instance, *name)) {
                auto it = sharedStrings.find(std::string(trim(element.text)));
                if (it == sharedStrings.end())
                    builder.warnOnce(std::format("{}.{}: unknown shared string '{}'", instance.className(), *name,
                                                 trim(element.text)));
                else
                    set(instance, it->second);
                return;
            }
        const Property* property = builder.settableProperty(instance, *name);
        if (!property)
            return;

        if (element.name == "Ref") {
            std::string_view referent = trim(element.text);
            if (!referent.empty() && referent != "null")
                pendingRefs_.push_back({instance.shared_from_this(), property, std::string(referent)});
            return;
        }

        auto value = decodeValue(element, sharedStrings);
        if (!value)
            builder.warnOnce(std::format("{}.{}: {}", instance.className(), *name, value.error()));
        else
            builder.assign(instance, *property, *value);
    }

    std::unordered_map<std::string, InstancePtr> byReferent_;
    std::vector<PendingRef> pendingRefs_;
};

std::expected<xml::Element, std::string> parseDocument(std::string_view text)
{
    if (text.starts_with("<roblox!"))
        return std::unexpected("this is a binary file, only the XML format is supported so far");
    auto document = xml::parse(text);
    if (!document)
        return std::unexpected(
            std::format("XML error on line {}: {}", document.error().line, document.error().message));
    if (document->name != "roblox")
        return std::unexpected(std::format("expected a <roblox> root element, got <{}>", document->name));
    return std::move(*document);
}

} // namespace

std::expected<LoadedPlace, std::string> loadPlace(std::string_view xml)
{
    return parseDocument(xml).transform([](const xml::Element& root) {
        auto game = DataModel::create();
        Loader loader;
        loader.readSharedStrings(root);
        for (const xml::Element& item : root.childrenNamed("Item")) {
            const std::string* className = item.attribute("class");
            const ClassInfo* info = className ? ClassRegistry::find(*className) : nullptr;
            if (info && info->kind == ClassKind::Service)
                loader.fill(*game->getService(*className), item);
            else if (InstancePtr instance = loader.build(item))
                instance->setParent(game);
        }
        loader.resolveRefs();
        return LoadedPlace{std::move(game), std::move(loader.builder.warnings)};
    });
}

std::expected<LoadedModel, std::string> loadModel(std::string_view xml)
{
    return parseDocument(xml).transform([](const xml::Element& root) {
        Loader loader;
        loader.readSharedStrings(root);
        std::vector<InstancePtr> roots;
        for (const xml::Element& item : root.childrenNamed("Item"))
            if (InstancePtr instance = loader.build(item))
                roots.push_back(std::move(instance));
        loader.resolveRefs();
        return LoadedModel{std::move(roots), std::move(loader.builder.warnings)};
    });
}

} // namespace lunar::rbxlx

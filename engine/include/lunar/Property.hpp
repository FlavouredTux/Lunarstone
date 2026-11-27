#pragma once

#include <lunar/Error.hpp>
#include <lunar/Value.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#if __cpp_impl_reflection
#include <meta>
#endif

namespace lunar {

class Instance;

namespace detail {
std::string_view valueClassName(const Instance& instance);
}

// One Roblox property of a class, reachable by name.
struct Property {
    std::string_view name;
    std::string_view type; // Luau type name of the value
    Value (*get)(const Instance&) = nullptr;
    void (*set)(Instance&, const Value&) = nullptr; // null when read-only
    bool isRef = false;                             // holds an Instance (Clone remaps these)

    bool readOnly() const { return set == nullptr; }
};

// Annotation that turns a getter into a property; write it as LUNAR_PROPERTY() or
// LUNAR_PROPERTY("CFrame") so clangd, which can't parse annotations yet, copes.
// The name defaults to the getter's name with a capital first letter. The setter
// is the member function named set<getter> or set<name> (any case) taking the
// getter's type; without one the property is read-only.
struct prop {
    char name[64]{};

    consteval prop() = default;
    consteval prop(const char* n)
    {
        for (std::size_t i = 0; n[i]; ++i)
            name[i] = n[i];
    }
};

#if __cpp_impl_reflection
#define LUNAR_PROPERTY(...) [[= ::lunar::prop(__VA_ARGS__)]]
#else
#define LUNAR_PROPERTY(...)
#endif

namespace detail {

// Unqualified name of T. The real build uses reflection; the fallback only
// exists so clangd (no reflection support yet) can still parse the headers.
template <class T>
consteval std::string_view className()
{
#if __cpp_impl_reflection
    return std::meta::identifier_of(^^T);
#else
    std::string_view signature = __PRETTY_FUNCTION__; // "... [T = lunar::Part]"
    std::string_view name = signature.substr(signature.find("T = ") + 4);
    name = name.substr(0, name.find_first_of(";]"));
    return name.substr(name.rfind("::") == std::string_view::npos ? 0 : name.rfind("::") + 2);
#endif
}

template <class V, class Variant>
struct IsAlternative : std::false_type {};
template <class V, class... Ts>
struct IsAlternative<V, std::variant<Ts...>> : std::bool_constant<(std::is_same_v<V, Ts> || ...)> {};

template <class V>
struct IsOptional : std::false_type {};
template <class V>
struct IsOptional<std::optional<V>> : std::true_type {};

template <class E>
consteval std::string_view enumTypeName()
{
#if __cpp_impl_reflection
    return std::define_static_string("Enum." + std::string(std::meta::identifier_of(^^E)));
#else
    return "EnumItem";
#endif
}

template <class V>
constexpr std::string_view valueTypeName();

// "CFrame?" for std::optional<CFrame>: the value or nil.
template <class V>
struct OptionalTypeName {
    static constexpr auto storage = [] {
        constexpr std::string_view inner = valueTypeName<V>();
        std::array<char, inner.size() + 1> name{};
        inner.copy(name.data(), inner.size());
        name.back() = '?';
        return name;
    }();
    static constexpr std::string_view value{storage.data(), storage.size()};
};

// The type a property reports: Luau type names for values ("number", "Vector3",
// "BrickColor"), "Enum.<Name>" for enums, the class name for Instance
// references and a trailing '?' for optional values.
template <class V>
constexpr std::string_view valueTypeName()
{
    if constexpr (std::is_same_v<V, bool>)
        return "boolean";
    else if constexpr (std::is_enum_v<V>)
        return enumTypeName<V>();
    else if constexpr (std::is_arithmetic_v<V>)
        return "number";
    else if constexpr (std::is_same_v<V, std::string> || std::is_same_v<V, std::string_view>)
        return "string";
    else if constexpr (IsOptional<V>::value)
        return OptionalTypeName<typename V::value_type>::value;
    else if constexpr (std::is_class_v<V> && IsAlternative<V, Value>::value)
        return className<V>();
    else if constexpr (std::is_pointer_v<V>)
        return className<std::remove_cv_t<std::remove_pointer_t<V>>>();
    else
        static_assert(false, "this type can't be used as a property");
}

template <class V>
Value toValue(const V& v)
{
    if constexpr (std::is_enum_v<V>)
        return enums::item(v);
    else if constexpr (std::is_arithmetic_v<V> && !std::is_same_v<V, bool>)
        return static_cast<double>(v);
    else if constexpr (std::is_same_v<V, std::string_view>)
        return std::string(v);
    else if constexpr (IsOptional<V>::value)
        return v ? toValue(*v) : Value();
    else if constexpr (std::is_pointer_v<V>)
        return v ? Value(InstancePtr(v->shared_from_this())) : Value();
    else
        return v;
}

[[noreturn]] void throwBadEnum(std::string_view property, const EnumInfo& info, const Value& value);

// Enums take their EnumItem, the item's name or its number, like Roblox does.
template <class E>
std::optional<E> enumFromValue(const Value& value, std::string_view property)
{
    const EnumInfo& info = enums::info<E>();
    const EnumItem* item = nullptr;
    if (auto* e = std::get_if<EnumItem>(&value)) {
        if (e->enumType != &info)
            throwBadEnum(property, info, value);
        item = e;
    } else if (auto* d = std::get_if<double>(&value)) {
        if (*d >= 0 && *d <= 4294967295.0)
            item = info.find(static_cast<std::uint32_t>(*d));
        if (!item)
            throwBadEnum(property, info, value);
    } else if (auto* s = std::get_if<std::string>(&value)) {
        item = info.find(*s);
        if (!item)
            throwBadEnum(property, info, value);
    } else {
        return std::nullopt;
    }
    return static_cast<E>(item->value);
}

template <class V>
V fromValue(const Value& value, std::string_view property)
{
    if constexpr (std::is_same_v<V, bool>) {
        if (auto* b = std::get_if<bool>(&value))
            return *b;
    } else if constexpr (std::is_enum_v<V>) {
        if (auto e = enumFromValue<V>(value, property))
            return *e;
    } else if constexpr (std::is_arithmetic_v<V>) {
        if (auto* d = std::get_if<double>(&value))
            return static_cast<V>(*d);
    } else if constexpr (std::is_same_v<V, std::string> || std::is_same_v<V, std::string_view>) {
        if (auto* s = std::get_if<std::string>(&value))
            return V(*s);
    } else if constexpr (IsOptional<V>::value) {
        if (std::holds_alternative<std::monostate>(value))
            return std::nullopt;
        return fromValue<typename V::value_type>(value, property);
    } else if constexpr (std::is_pointer_v<V>) {
        if (std::holds_alternative<std::monostate>(value))
            return nullptr;
        if (auto* p = std::get_if<InstancePtr>(&value))
            if (auto* instance = dynamic_cast<V>(p->get()))
                return instance;
    } else {
        if (auto* v = std::get_if<V>(&value))
            return *v;
        // Place files store BrickColors as palette numbers.
        if constexpr (std::is_same_v<V, BrickColor>)
            if (auto* d = std::get_if<double>(&value))
                return BrickColor::fromNumber(static_cast<int>(*d));
    }
    auto* instance = std::get_if<InstancePtr>(&value);
    throw Error(std::format("Unable to assign property {}. {} expected, got {}", property, valueTypeName<V>(),
                            instance && *instance ? valueClassName(**instance) : typeName(value)));
}

#if __cpp_impl_reflection

consteval char toLower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

consteval char toUpper(char c)
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

consteval bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (toLower(a[i]) != toLower(b[i]))
            return false;
    return true;
}

consteval bool isMemberFunction(std::meta::info m)
{
    return std::meta::is_function(m) && !std::meta::is_static_member(m) && std::meta::has_identifier(m);
}

template <class T>
consteval std::vector<std::meta::info> propertyGetters()
{
    std::vector<std::meta::info> getters;
    for (std::meta::info m : std::meta::members_of(^^T, std::meta::access_context::unchecked()))
        if (isMemberFunction(m) && !std::meta::annotations_of_with_type(m, ^^prop).empty())
            getters.push_back(m);
    return getters;
}

consteval std::string_view propertyName(std::meta::info getter)
{
    prop annotation = std::meta::extract<prop>(std::meta::annotations_of_with_type(getter, ^^prop)[0]);
    std::string name
        = annotation.name[0] ? std::string(annotation.name) : std::string(std::meta::identifier_of(getter));
    name[0] = toUpper(name[0]);
    return std::define_static_string(name);
}

// The setter is set<getter> or set<property name>, compared ignoring case, taking V.
consteval std::meta::info findSetter(std::meta::info cls, std::meta::info getter, std::string_view property,
                                     std::meta::info valueType)
{
    std::string byGetter = "set" + std::string(std::meta::identifier_of(getter));
    std::string byProperty = "set";
    for (char c : property) // std::string(property) isn't constant for static strings in GCC 16
        byProperty += c;
    for (std::meta::info m : std::meta::members_of(cls, std::meta::access_context::unchecked())) {
        if (!isMemberFunction(m)
            || !(equalsIgnoreCase(std::meta::identifier_of(m), byGetter)
                 || equalsIgnoreCase(std::meta::identifier_of(m), byProperty)))
            continue;
        auto params = std::meta::parameters_of(m);
        if (params.size() == 1
            && std::meta::dealias(std::meta::remove_cvref(std::meta::type_of(params[0])))
                   == std::meta::dealias(valueType))
            return m;
    }
    return std::meta::info{};
}

template <class T, std::meta::info Getter>
Property makeProperty()
{
    using V = typename[:std::meta::remove_cvref(std::meta::return_type_of(Getter)):];
    static constexpr std::string_view name = propertyName(Getter);
    static constexpr std::meta::info setter = findSetter(^^T, Getter, name, ^^V);

    Property property{name, valueTypeName<V>()};
    property.isRef = std::is_pointer_v<V>;
    property.get
        = [](const Instance& instance) -> Value { return toValue<V>((static_cast<const T&>(instance).[:Getter:])()); };
    if constexpr (setter != std::meta::info{})
        property.set = [](Instance& instance, const Value& value) {
            (static_cast<T&>(instance).[:setter:])(fromValue<V>(value, name));
        };
    return property;
}

// The properties T itself declares (not its bases), in declaration order.
template <class T>
std::span<const Property> ownProperties()
{
    static constexpr auto getters = std::define_static_array(propertyGetters<T>());
    static const auto properties = [] {
        std::array<Property, getters.size()> result{};
        [[maybe_unused]] std::size_t i = 0;
        // clang-format doesn't know expansion statements yet.
        // clang-format off
        template for (constexpr std::meta::info getter : getters)
            result[i++] = makeProperty<T, getter>();
        // clang-format on
        return result;
    }();
    return properties;
}

#else

// No reflection (clangd): parse fine, expose nothing.
template <class T>
std::span<const Property> ownProperties()
{
    return {};
}

#endif

} // namespace detail

} // namespace lunar

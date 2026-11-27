#include <lunar/Enums.hpp>

#include <atomic>

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

namespace lunar {

const EnumItem* EnumInfo::find(std::string_view itemName) const
{
    auto it = std::ranges::find(items, itemName, &EnumItem::name);
    return it == items.end() ? nullptr : &*it;
}

const EnumItem* EnumInfo::find(std::uint32_t value) const
{
    auto it = std::ranges::find(items, value, &EnumItem::value);
    return it == items.end() ? nullptr : &*it;
}

namespace enums {

namespace {

#if __cpp_impl_reflection
consteval std::vector<std::meta::info> declaredEnums()
{
    std::vector<std::meta::info> result;
    for (std::meta::info m : std::meta::members_of(^^Enum, std::meta::access_context::unchecked()))
        if (std::meta::is_enum_type(m))
            result.push_back(m);
    return result;
}

constexpr auto declared = std::define_static_array(declaredEnums());

template <std::size_t... I>
std::vector<const EnumInfo*> infos(std::index_sequence<I...>)
{
    return {&info<typename[:declared[I]:]>()...};
}

std::vector<const EnumInfo*> collect()
{
    std::vector<const EnumInfo*> result = infos(std::make_index_sequence<declared.size()>());
    std::ranges::sort(result, {}, &EnumInfo::name);
    return result;
}
#else
std::vector<const EnumInfo*> collect()
{
    return {};
}
#endif

const std::vector<const EnumInfo*>& registry()
{
    static const std::vector<const EnumInfo*> enums = collect();
    return enums;
}

} // namespace

const EnumInfo* find(std::string_view name)
{
    const auto& all = registry();
    auto it = std::ranges::lower_bound(all, name, {}, &EnumInfo::name);
    return it != all.end() && (*it)->name == name ? *it : nullptr;
}

namespace {
std::atomic<const EnumInfo* (*)(std::string_view)> catalogLookup{nullptr};
} // namespace

const EnumInfo* findAny(std::string_view name)
{
    if (const EnumInfo* info = find(name))
        return info;
    const auto lookup = catalogLookup.load();
    return lookup ? lookup(name) : nullptr;
}

void setCatalog(const EnumInfo* (*lookup)(std::string_view name))
{
    catalogLookup.store(lookup);
}

std::span<const EnumInfo* const> all()
{
    return registry();
}

} // namespace enums

} // namespace lunar

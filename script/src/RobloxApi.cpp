#include "RobloxApi.hpp"

#include <algorithm>
#include <vector>

namespace lunar::lua::api {

const EnumData* findEnum(std::string_view name)
{
    auto enums = allEnums();
    auto it = std::ranges::lower_bound(enums, name, {}, &EnumData::name);
    return it != enums.end() && it->name == name ? &*it : nullptr;
}

const ClassData* findClass(std::string_view name)
{
    auto classes = allClasses();
    auto it = std::ranges::lower_bound(classes, name, {}, &ClassData::name);
    return it != classes.end() && it->name == name ? &*it : nullptr;
}

namespace {

// Each class's base as an index into allClasses() (-1 at the root), looked up
// once: instance member lookups walk the chain on every miss.
std::span<const int> baseIndices()
{
    static const std::vector<int> bases = [] {
        auto classes = allClasses();
        std::vector<int> found(classes.size(), -1);
        for (std::size_t i = 0; i < classes.size(); ++i)
            if (const ClassData* base = classes[i].base.empty() ? nullptr : findClass(classes[i].base))
                found[i] = static_cast<int>(base - classes.data());
        return found;
    }();
    return bases;
}

} // namespace

std::string_view capabilityName(std::uint8_t capability)
{
    static constexpr std::string_view names[]
        = {"Plugin", "LocalUser", "WritePlayer", "RobloxScript", "RobloxEngine", "NotAccessible"};
    return capability >= 1 && capability <= std::size(names) ? names[capability - 1] : "Unknown";
}

Capabilities capabilitiesOf(Identity identity)
{
    constexpr Capabilities plugin = 1u << 0, localUser = 1u << 1, robloxScript = 1u << 3;
    switch (static_cast<int>(identity)) {
    case 3:
    case 6:
        return plugin | localUser | robloxScript;
    case 4:
        return plugin | localUser;
    case 5:
        return plugin;
    case 7:
        return allCapabilities & ~(1u << 5);
    case 8:
        return allCapabilities;
    default:
        return 0;
    }
}

std::string_view declaringClass(const MemberData& member)
{
    for (const ClassData& data : allClasses()) {
        auto members = membersOf(data);
        if (!members.empty() && &member >= members.data() && &member < members.data() + members.size())
            return data.name;
    }
    return {};
}

bool isA(std::string_view className, std::string_view base)
{
    auto classes = allClasses();
    auto bases = baseIndices();
    const ClassData* data = findClass(className);
    for (int depth = 0; depth < 32 && data; ++depth) {
        if (data->name == base)
            return true;
        const int index = bases[static_cast<std::size_t>(data - classes.data())];
        data = index < 0 ? nullptr : &classes[static_cast<std::size_t>(index)];
    }
    return false;
}

const MemberData* findMember(std::string_view className, std::string_view member)
{
    auto classes = allClasses();
    auto bases = baseIndices();
    const ClassData* data = findClass(className);
    // Bounded, in case the data ever had a cycle.
    for (int depth = 0; depth < 32 && data; ++depth) {
        auto members = membersOf(*data);
        auto it = std::ranges::lower_bound(members, member, {}, &MemberData::name);
        if (it != members.end() && it->name == member)
            return &*it;
        const int base = bases[static_cast<std::size_t>(data - classes.data())];
        data = base < 0 ? nullptr : &classes[static_cast<std::size_t>(base)];
    }
    return nullptr;
}

} // namespace lunar::lua::api

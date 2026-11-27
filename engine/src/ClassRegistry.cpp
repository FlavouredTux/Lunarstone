#include <lunar/ClassRegistry.hpp>

#include "ClassRegistrySlice.hpp"

#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lunar {

namespace {

using Registry = detail::ClassTable;

template <std::size_t... Slice>
Registry builtins(std::index_sequence<Slice...>)
{
    Registry r;
    (detail::addClassSlice<Slice>(r), ...);
    return r;
}

Registry& registry()
{
    static Registry classes = builtins(std::make_index_sequence<detail::classSlices>());
    return classes;
}

} // namespace

const ClassInfo* ClassRegistry::find(std::string_view className)
{
    auto it = registry().find(className);
    return it == registry().end() ? nullptr : it->second;
}

InstancePtr ClassRegistry::create(std::string_view className)
{
    const ClassInfo* info = find(className);
    if (!info || info->kind != ClassKind::Creatable)
        throw Error(std::format("Unable to create an Instance of type \"{}\"", className));
    return info->factory();
}

void ClassRegistry::add(const ClassInfo& info)
{
    registry().insert_or_assign(info.name, &info);
}

std::vector<const ClassInfo*> ClassRegistry::all()
{
    std::vector<const ClassInfo*> result;
    for (const auto& [name, info] : registry())
        result.push_back(info);
    std::ranges::sort(result, {}, &ClassInfo::name);
    return result;
}

const ClassInfo& ClassRegistry::placeholder(std::string_view className)
{
    struct Placeholder {
        std::string name;
        ClassInfo info;
    };
    static std::map<std::string, std::unique_ptr<Placeholder>, std::less<>> placeholders;
    // Models load on worker threads too (the avatar's ClipLoader), so two
    // unknown classes can turn up at once; a race here crashed a Ball Tower
    // Defense join. The entries never move, so the reference outlives the lock.
    static std::mutex mutex;
    const std::scoped_lock lock(mutex);

    if (auto it = placeholders.find(className); it != placeholders.end())
        return it->second->info;
    auto entry = std::make_unique<Placeholder>();
    entry->name = className;
    entry->info = ClassInfo{entry->name, &Instance::staticClassInfo(), ClassKind::NotCreatable, nullptr, {}};
    return placeholders.emplace(entry->name, std::move(entry)).first->second->info;
}

} // namespace lunar

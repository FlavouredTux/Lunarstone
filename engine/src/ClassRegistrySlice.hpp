#pragma once

// The built-in class table, found by reflection. Instantiating every
// class's ClassInfo is most of the engine's compile time, so the table is
// filled in slices: engine/CMakeLists.txt generates one source per slice
// from ClassRegistrySlice.cpp.in, and they compile in parallel.

#include <lunar/Classes.hpp>

#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if __cpp_impl_reflection
#include <meta>
#endif

namespace lunar::detail {

using ClassTable = std::unordered_map<std::string_view, const ClassInfo*>;

// Keep in step with LUNAR_CLASS_SLICES in engine/CMakeLists.txt.
inline constexpr std::size_t classSlices = 8;

#if __cpp_impl_reflection

// An engine class derives directly from InstanceImpl<itself, ...>.
consteval bool isEngineClass(std::meta::info type)
{
    if (type == ^^Instance)
        return true;
    for (std::meta::info base : std::meta::bases_of(type, std::meta::access_context::unchecked())) {
        std::meta::info baseType = std::meta::type_of(base);
        if (std::meta::has_template_arguments(baseType)
            && std::meta::template_of(baseType) == ^^InstanceImpl&& std::meta::template_arguments_of(baseType)[0]
                   == type)
            return true;
    }
    return false;
}

// Every engine class declared in namespace lunar.
consteval std::vector<std::meta::info> engineClasses()
{
    std::vector<std::meta::info> result;
    for (std::meta::info m : std::meta::members_of(^^lunar, std::meta::access_context::unchecked()))
        if (std::meta::is_type(m) && std::meta::is_class_type(m) && std::meta::is_complete_type(m) && isEngineClass(m))
            result.push_back(m);
    return result;
}

inline constexpr auto engineClassList = std::define_static_array(engineClasses());

template <class T>
void addClass(ClassTable& table)
{
    table.emplace(T::staticClassInfo().name, &T::staticClassInfo());
}

template <std::size_t First, std::size_t... I>
void addClasses(ClassTable& table, std::index_sequence<I...>)
{
    (addClass<typename[:engineClassList[First + I]:]>(table), ...);
}

// Adds the Slice-th of classSlices equal runs of engineClassList.
template <std::size_t Slice>
void addClassSlice(ClassTable& table)
{
    constexpr std::size_t first = engineClassList.size() * Slice / classSlices;
    constexpr std::size_t last = engineClassList.size() * (Slice + 1) / classSlices;
    addClasses<first>(table, std::make_index_sequence<last - first>());
}

#else

template <std::size_t Slice>
void addClassSlice(ClassTable&)
{
}

#endif

// Instantiated only by the generated slice sources.
extern template void addClassSlice<0>(ClassTable&);
extern template void addClassSlice<1>(ClassTable&);
extern template void addClassSlice<2>(ClassTable&);
extern template void addClassSlice<3>(ClassTable&);
extern template void addClassSlice<4>(ClassTable&);
extern template void addClassSlice<5>(ClassTable&);
extern template void addClassSlice<6>(ClassTable&);
extern template void addClassSlice<7>(ClassTable&);

} // namespace lunar::detail

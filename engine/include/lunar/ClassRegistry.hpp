#pragma once

#include <lunar/Instance.hpp>

#include <string_view>
#include <vector>

namespace lunar {

// Name -> class lookup. Every built-in class is registered on first use.
class ClassRegistry {
public:
    static const ClassInfo* find(std::string_view className);

    // Instance.new: only works for Creatable classes.
    static InstancePtr create(std::string_view className);

    static void add(const ClassInfo& info);

    // Every registered class, sorted by name.
    static std::vector<const ClassInfo*> all();

    // Made-up ClassInfo for a class the engine doesn't know, used by
    // UnknownInstance. Not registered, so find() and create() still fail.
    static const ClassInfo& placeholder(std::string_view className);

    template <class T>
    static void add()
    {
        add(T::staticClassInfo());
    }
};

} // namespace lunar

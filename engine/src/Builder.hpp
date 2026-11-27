#pragma once

#include <lunar/Instance.hpp>

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace lunar::detail {

// Shared by the XML and binary loaders: creates instances by class name and
// applies serialized properties, collecting warnings instead of failing.
class Builder {
public:
    std::vector<std::string> warnings;

    void warnOnce(std::string message);

    // Known classes through their factory, anything else as an UnknownInstance.
    InstancePtr create(std::string_view className);

    // The settable property behind a serialized name, Studio's legacy names
    // included. Null if there's none; warns unless the instance is a placeholder.
    const Property* settableProperty(const Instance& instance, std::string_view serializedName);

    // Sets the value, turning a rejected one into a warning.
    void assign(Instance& instance, const Property& property, const Value& value);

    // For serialized blobs kept as shared bytes (PhysicalConfigData): the
    // setter that takes a place file's shared string without copying it, or
    // null when the property is an ordinary one.
    using SharedSetter = void (*)(Instance&, SharedBytes);
    static SharedSetter sharedSetter(const Instance& instance, std::string_view serializedName);

private:
    std::unordered_set<std::string> warned_;
};

} // namespace lunar::detail

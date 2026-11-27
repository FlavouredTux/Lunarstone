#pragma once

// Value objects: an instance holding one Value property. Included through
// <lunar/Classes.hpp>.

#include <lunar/DataTypes.hpp>
#include <lunar/Instance.hpp>

#include <cstdint>
#include <string>

namespace lunar {

class ValueBase : public InstanceImpl<ValueBase, Instance, ClassKind::Abstract> {};

class StringValue final : public InstanceImpl<StringValue, ValueBase> {
public:
    LUNAR_FIELD(std::string, value, Value)
};

// Raw bytes rather than text.
class BinaryStringValue final : public InstanceImpl<BinaryStringValue, ValueBase> {
public:
    LUNAR_FIELD(std::string, value, Value)
};

class IntValue final : public InstanceImpl<IntValue, ValueBase> {
public:
    LUNAR_FIELD(std::int64_t, value, Value, 0)
};

class NumberValue final : public InstanceImpl<NumberValue, ValueBase> {
public:
    LUNAR_FIELD(double, value, Value, 0.0)
};

class BoolValue final : public InstanceImpl<BoolValue, ValueBase> {
public:
    LUNAR_FIELD(bool, value, Value, false)
};

class ObjectValue final : public InstanceImpl<ObjectValue, ValueBase> {
public:
    LUNAR_REF(Instance, value, Value)
};

class Vector3Value final : public InstanceImpl<Vector3Value, ValueBase> {
public:
    LUNAR_FIELD(Vector3, value, Value)
};

class CFrameValue final : public InstanceImpl<CFrameValue, ValueBase> {
public:
    LUNAR_FIELD(CFrame, value, Value)
};

class Color3Value final : public InstanceImpl<Color3Value, ValueBase> {
public:
    LUNAR_FIELD(Color3, value, Value)
};

class BrickColorValue final : public InstanceImpl<BrickColorValue, ValueBase> {
public:
    LUNAR_FIELD(BrickColor, value, Value)
};

} // namespace lunar

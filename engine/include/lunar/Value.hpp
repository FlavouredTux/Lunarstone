#pragma once

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/Math.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace lunar {

class Instance;
using InstancePtr = std::shared_ptr<Instance>;

// A dynamically typed value, the C++ side of a Luau value. std::monostate is nil.
//
// The alternatives, in index order (new ones are only ever appended, so
// indices stay stable; visitors over Value must handle all of them):
//    0 nil (std::monostate)   8 Vector2         16 EnumItem
//    1 boolean                9 UDim            17 Faces
//    2 number (double)       10 UDim2           18 Axes
//    3 string                11 Rect            19 PhysicalProperties
//    4 Vector3               12 NumberRange     20 Vector3int16
//    5 Color3                13 NumberSequence  21 Font
//    6 CFrame                14 ColorSequence        22 buffer (Buffer)
//    7 Instance (or nil)     15 BrickColor
//
// Not separate alternatives, because Luau doesn't have them either: integers
// (int32/int64 are numbers), Content/SharedString/BinaryString (strings),
// Ref (Instance), OptionalCoordinateFrame (CFrame or nil) and enum values read
// from place files before they meet a property (numbers; the property turns
// them into its enum).
using Value = std::variant<std::monostate, bool, double, std::string, Vector3, Color3, CFrame, InstancePtr, Vector2,
                           UDim, UDim2, Rect, NumberRange, NumberSequence, ColorSequence, BrickColor, EnumItem, Faces,
                           Axes, PhysicalProperties, Vector3int16, Font, Buffer>;

// Bytes many instances share: the SharedString blobs of place files, kept
// once however many instances refer to them (thousands of MeshParts can share
// one collision blob). Not a Value: scripts never see these.
using SharedBytes = std::shared_ptr<const std::string>;

// Luau typeof() name: "nil", "boolean", "number", "string", "Vector3", "Instance",
// "EnumItem", ...
std::string_view typeName(const Value& value);

// What Luau's tostring() gives for the value.
std::string toString(const Value& value);

} // namespace lunar

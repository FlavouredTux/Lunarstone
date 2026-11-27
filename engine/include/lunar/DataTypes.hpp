#pragma once

// Roblox data types beyond the basic math ones in Math.hpp. They mirror the
// Luau types of the same name; fields use Roblox's names in lower case.

#include <lunar/Enums.hpp>
#include <lunar/Math.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lunar {

struct UDim {
    float scale = 0.0f;
    std::int32_t offset = 0;

    friend constexpr bool operator==(const UDim&, const UDim&) = default;
};

struct UDim2 {
    UDim x;
    UDim y;

    static constexpr UDim2 fromScale(float x, float y) { return {{x, 0}, {y, 0}}; }
    static constexpr UDim2 fromOffset(std::int32_t x, std::int32_t y) { return {{0.0f, x}, {0.0f, y}}; }

    friend constexpr bool operator==(const UDim2&, const UDim2&) = default;
};

struct Rect {
    Vector2 min;
    Vector2 max;

    constexpr float width() const { return max.x - min.x; }
    constexpr float height() const { return max.y - min.y; }

    friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

struct NumberRange {
    float min = 0.0f;
    float max = 0.0f;

    friend constexpr bool operator==(const NumberRange&, const NumberRange&) = default;
};

struct NumberSequenceKeypoint {
    float time = 0.0f;
    float value = 0.0f;
    float envelope = 0.0f;

    friend constexpr bool operator==(const NumberSequenceKeypoint&, const NumberSequenceKeypoint&) = default;
};

// Keypoints sorted by time, from 0 to 1.
struct NumberSequence {
    std::vector<NumberSequenceKeypoint> keypoints;

    NumberSequence() : NumberSequence(0.0f) {}
    explicit NumberSequence(float value) : keypoints{{0.0f, value, 0.0f}, {1.0f, value, 0.0f}} {}
    explicit NumberSequence(std::vector<NumberSequenceKeypoint> points) : keypoints(std::move(points)) {}

    friend bool operator==(const NumberSequence&, const NumberSequence&) = default;
};

struct ColorSequenceKeypoint {
    float time = 0.0f;
    Color3 value;

    friend constexpr bool operator==(const ColorSequenceKeypoint&, const ColorSequenceKeypoint&) = default;
};

struct ColorSequence {
    std::vector<ColorSequenceKeypoint> keypoints;

    ColorSequence() : ColorSequence(Color3{1.0f, 1.0f, 1.0f}) {}
    explicit ColorSequence(Color3 color) : keypoints{{0.0f, color}, {1.0f, color}} {}
    explicit ColorSequence(std::vector<ColorSequenceKeypoint> points) : keypoints(std::move(points)) {}

    friend bool operator==(const ColorSequence&, const ColorSequence&) = default;
};

// A set of NormalIds. Stored as bits in NormalId order: Right, Top, Back, Left, Bottom, Front.
struct Faces {
    bool right = false;
    bool top = false;
    bool back = false;
    bool left = false;
    bool bottom = false;
    bool front = false;

    static constexpr Faces fromBits(std::uint8_t bits)
    {
        auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
        return {bit(0), bit(1), bit(2), bit(3), bit(4), bit(5)};
    }
    constexpr std::uint8_t bits() const
    {
        return static_cast<std::uint8_t>(right | top << 1 | back << 2 | left << 3 | bottom << 4 | front << 5);
    }

    friend constexpr bool operator==(const Faces&, const Faces&) = default;
};

// A set of axes, stored as bits X, Y, Z.
struct Axes {
    bool x = false;
    bool y = false;
    bool z = false;

    static constexpr Axes fromBits(std::uint8_t bits) { return {(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0}; }
    constexpr std::uint8_t bits() const { return static_cast<std::uint8_t>(x | y << 1 | z << 2); }

    friend constexpr bool operator==(const Axes&, const Axes&) = default;
};

// Custom physics of a part. Defaults are Plastic's.
struct PhysicalProperties {
    float density = 0.7f;
    float friction = 0.3f;
    float elasticity = 0.5f;
    float frictionWeight = 1.0f;
    float elasticityWeight = 1.0f;
    float acousticAbsorption = 1.0f;

    friend constexpr bool operator==(const PhysicalProperties&, const PhysicalProperties&) = default;
};

// A text font: a font family asset plus weight and style.
struct Font {
    std::string family = "rbxasset://fonts/families/SourceSansPro.json";
    Enum::FontWeight weight = Enum::FontWeight::Regular;
    Enum::FontStyle style = Enum::FontStyle::Normal;

    friend bool operator==(const Font&, const Font&) = default;
};

// A Luau buffer's bytes: what a RemoteEvent carries as a buffer arrives as
// one (not a string, which scripts can't hand to buffer.len).
struct Buffer {
    std::string bytes;

    friend bool operator==(const Buffer&, const Buffer&) = default;
};

// A color from the classic Roblox palette, identified by its palette number.
struct BrickColor {
    struct Entry {
        std::uint16_t number;
        std::string_view name;
        std::uint8_t r, g, b;
    };

    std::uint16_t number = 194; // Medium stone grey

    // BrickColor.new(number): numbers outside the palette give the default.
    static BrickColor fromNumber(int number);
    // BrickColor.new(name): exact name, or the default if there's none.
    static BrickColor fromName(std::string_view name);
    // BrickColor.new(Color3): the closest palette color.
    static BrickColor nearest(Color3 color);
    static bool isValid(int number);

    std::string_view name() const;
    Color3 color() const;

    static std::span<const Entry> palette();

    friend constexpr bool operator==(const BrickColor&, const BrickColor&) = default;
};

} // namespace lunar

#pragma once

// Typed userdata for the Roblox datatypes: one tag and metatable per C++
// type, push/to/check helpers, and field access generated from the struct's
// members by reflection (UDim::scale reads as UDim.Scale).

#include "Runtime.hpp"

#include <lunar/DataTypes.hpp>
#include <lunar/Enums.hpp>
#include <lunar/Math.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if __cpp_impl_reflection
#include <meta>
#endif

namespace lunar::lua {

// --- Script-only datatypes -----------------------------------------------------

// Enum.Material and friends: the userdata wraps the catalog entry.
struct EnumCatalogEntry;
struct EnumType {
    const EnumCatalogEntry* entry = nullptr;
};
// The Enum global itself.
struct EnumsType {};

struct TweenInfo {
    double time = 1.0;
    EnumItem easingStyle;
    EnumItem easingDirection;
    double repeatCount = 0.0;
    bool reverses = false;
    double delayTime = 0.0;

    friend bool operator==(const TweenInfo&, const TweenInfo&) = default;
};

struct Ray {
    Vector3 origin;
    Vector3 direction;

    friend constexpr bool operator==(const Ray&, const Ray&) = default;
};

struct Region3 {
    Vector3 min;
    Vector3 max;

    friend constexpr bool operator==(const Region3&, const Region3&) = default;
};

struct Region3int16 {
    Vector3int16 min;
    Vector3int16 max;

    friend constexpr bool operator==(const Region3int16&, const Region3int16&) = default;
};

struct DateTime {
    std::int64_t unixTimestampMillis = 0;

    friend constexpr bool operator==(const DateTime&, const DateTime&) = default;
};

// Roblox's Random: a PCG32 generator (64-bit state, XSH-RR output).
struct RandomState {
    std::uint64_t state = 0;
    std::uint64_t inc = 0;
};

struct RaycastParams {
    std::vector<std::weak_ptr<Instance>> filterDescendantsInstances;
    EnumItem filterType;
    bool ignoreWater = false;
    std::string collisionGroup = "Default";
    bool respectCanCollide = false;
    bool bruteForceAllSlow = false;
};

struct OverlapParams {
    std::vector<std::weak_ptr<Instance>> filterDescendantsInstances;
    EnumItem filterType;
    int maxParts = 0;
    std::string collisionGroup = "Default";
    bool respectCanCollide = false;
    bool bruteForceAllSlow = false;
    float tolerance = 0.0f;
};

// A Path2D's control point: where it is and its two tangents, as UDim2s.
struct Path2DControlPoint {
    UDim2 position;
    UDim2 leftTangent;
    UDim2 rightTangent;

    friend bool operator==(const Path2DControlPoint&, const Path2DControlPoint&) = default;
};

struct Vector2int16 {
    std::int16_t x = 0;
    std::int16_t y = 0;

    friend constexpr bool operator==(const Vector2int16&, const Vector2int16&) = default;
};

// A point of a computed path: where, how to get there (Enum.PathWaypointAction)
// and the label of the PathfindingModifier it is in.
struct PathWaypoint {
    Vector3 position;
    EnumItem action;
    std::string label;

    friend bool operator==(const PathWaypoint&, const PathWaypoint&) = default;
};

// A RotationCurve's key: a CFrame's rotation at a time, and how it eases to the
// next (Enum.KeyInterpolationMode).
struct RotationCurveKey {
    float time = 0.0f;
    CFrame value;
    EnumItem interpolation;

    friend bool operator==(const RotationCurveKey&, const RotationCurveKey&) = default;
};

// Where an image or other asset comes from: nothing, a URI, or an object
// (Enum.ContentSourceType).
struct Content {
    EnumItem sourceType;
    std::string uri;
    std::weak_ptr<Instance> object;

    friend bool operator==(const Content& a, const Content& b)
    {
        return a.sourceType == b.sourceType && a.uri == b.uri && a.object.lock() == b.object.lock();
    }
};

// What a plugin drags: who started it, the data and its MIME type, and the
// icons with the hot spot.
struct PluginDrag {
    std::string sender;
    std::string mimeType;
    std::string data;
    std::string mouseIcon;
    std::string dragIcon;
    Vector2 hotSpot;

    friend bool operator==(const PluginDrag&, const PluginDrag&) = default;
};

struct RaycastResult {
    std::weak_ptr<Instance> instance;
    Vector3 position;
    Vector3 normal;
    EnumItem material;
    float distance = 0.0f;
};

// --- Type table ------------------------------------------------------------------

template <class T>
struct TypeTraits;

#define LUNAR_USERDATA(Type, TagValue, Name)                                                                           \
    template <>                                                                                                        \
    struct TypeTraits<Type> {                                                                                          \
        static constexpr int tag = TagValue;                                                                           \
        static constexpr const char* name = Name;                                                                      \
    };

LUNAR_USERDATA(Vector2, Vector2Tag, "Vector2")
LUNAR_USERDATA(UDim, UDimTag, "UDim")
LUNAR_USERDATA(UDim2, UDim2Tag, "UDim2")
LUNAR_USERDATA(Rect, RectTag, "Rect")
LUNAR_USERDATA(NumberRange, NumberRangeTag, "NumberRange")
LUNAR_USERDATA(NumberSequence, NumberSequenceTag, "NumberSequence")
LUNAR_USERDATA(NumberSequenceKeypoint, NumberSequenceKeypointTag, "NumberSequenceKeypoint")
LUNAR_USERDATA(ColorSequence, ColorSequenceTag, "ColorSequence")
LUNAR_USERDATA(ColorSequenceKeypoint, ColorSequenceKeypointTag, "ColorSequenceKeypoint")
LUNAR_USERDATA(BrickColor, BrickColorTag, "BrickColor")
LUNAR_USERDATA(EnumItem, EnumItemTag, "EnumItem")
LUNAR_USERDATA(EnumType, EnumTag, "Enum")
LUNAR_USERDATA(EnumsType, EnumsTag, "Enums")
LUNAR_USERDATA(Faces, FacesTag, "Faces")
LUNAR_USERDATA(Axes, AxesTag, "Axes")
LUNAR_USERDATA(PhysicalProperties, PhysicalPropertiesTag, "PhysicalProperties")
LUNAR_USERDATA(Vector3int16, Vector3int16Tag, "Vector3int16")
LUNAR_USERDATA(Font, FontTag, "Font")
LUNAR_USERDATA(TweenInfo, TweenInfoTag, "TweenInfo")
LUNAR_USERDATA(Ray, RayTag, "Ray")
LUNAR_USERDATA(Region3, Region3Tag, "Region3")
LUNAR_USERDATA(Region3int16, Region3int16Tag, "Region3int16")
LUNAR_USERDATA(DateTime, DateTimeTag, "DateTime")
LUNAR_USERDATA(RandomState, RandomTag, "Random")
LUNAR_USERDATA(RaycastParams, RaycastParamsTag, "RaycastParams")
LUNAR_USERDATA(OverlapParams, OverlapParamsTag, "OverlapParams")
LUNAR_USERDATA(RaycastResult, RaycastResultTag, "RaycastResult")
LUNAR_USERDATA(Path2DControlPoint, Path2DControlPointTag, "Path2DControlPoint")
LUNAR_USERDATA(Vector2int16, Vector2int16Tag, "Vector2int16")
LUNAR_USERDATA(PathWaypoint, PathWaypointTag, "PathWaypoint")
LUNAR_USERDATA(RotationCurveKey, RotationCurveKeyTag, "RotationCurveKey")
LUNAR_USERDATA(Content, ContentTag, "Content")
LUNAR_USERDATA(PluginDrag, PluginDragTag, "PluginDrag")

#undef LUNAR_USERDATA

template <class T>
concept Userdata = requires { TypeTraits<T>::tag; };

template <Userdata T>
T& push(lua_State* L, T value)
{
    void* memory = lua_newuserdatataggedwithmetatable(L, sizeof(T), TypeTraits<T>::tag);
    return *new (memory) T(std::move(value));
}

template <Userdata T>
T* to(lua_State* L, int idx)
{
    return static_cast<T*>(lua_touserdatatagged(L, idx, TypeTraits<T>::tag));
}

template <Userdata T>
T& check(lua_State* L, int idx)
{
    if (T* value = to<T>(L, idx))
        return *value;
    luaL_typeerror(L, idx, TypeTraits<T>::name);
}

template <Userdata T>
T opt(lua_State* L, int idx, T fallback)
{
    return lua_isnoneornil(L, idx) ? fallback : check<T>(L, idx);
}

// --- Generic metatables ---------------------------------------------------------

// Pushes a property of the value under `key` and returns true, or false if there's none.
template <class T>
using FieldFn = bool (*)(lua_State*, const T&, std::string_view);

void pushNumber(lua_State* L, double value);
void pushString(lua_State* L, std::string_view text);
void pushEnumItem(lua_State* L, const EnumItem& item);

inline void pushAny(lua_State* L, bool b)
{
    lua_pushboolean(L, b);
}
inline void pushAny(lua_State* L, std::string_view s)
{
    pushString(L, s);
}
inline void pushAny(lua_State* L, const std::string& s)
{
    pushString(L, s);
}
inline void pushAny(lua_State* L, Vector3 v)
{
    pushVector3(L, v);
}
inline void pushAny(lua_State* L, Color3 c)
{
    pushColor3(L, c);
}
inline void pushAny(lua_State* L, const EnumItem& e)
{
    pushEnumItem(L, e);
}
template <class E>
    requires std::is_enum_v<E>
void pushAny(lua_State* L, E e)
{
    pushEnumItem(L, enums::item(e));
}
template <class N>
    requires(std::is_arithmetic_v<N> && !std::is_same_v<N, bool>)
void pushAny(lua_State* L, N n)
{
    pushNumber(L, static_cast<double>(n));
}
template <Userdata T>
void pushAny(lua_State* L, const T& value)
{
    push<T>(L, value);
}

#if __cpp_impl_reflection
consteval std::string_view capitalized(std::meta::info member)
{
    std::string name(std::meta::identifier_of(member));
    if (!name.empty() && name[0] >= 'a' && name[0] <= 'z')
        name[0] = static_cast<char>(name[0] - 'a' + 'A');
    return std::define_static_string(name);
}

template <class T>
consteval std::vector<std::meta::info> publicFields()
{
    std::vector<std::meta::info> fields;
    for (std::meta::info m : std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()))
        if (std::meta::is_public(m))
            fields.push_back(m);
    return fields;
}
#endif

// The struct's public fields, readable under their capitalized names: UDim's
// scale is UDim.Scale, PhysicalProperties' frictionWeight FrictionWeight.
template <class T>
bool reflectedField(lua_State* L, const T& value, std::string_view key)
{
#if __cpp_impl_reflection
    bool found = false;
    static constexpr auto fields = std::define_static_array(publicFields<T>());
    // clang-format off
    template for (constexpr std::meta::info field : fields) {
        if (!found && key == capitalized(field)) {
            pushAny(L, value.[:field:]);
            found = true;
        }
    }
    // clang-format on
    return found;
#else
    (void)L;
    (void)value;
    (void)key;
    return false;
#endif
}

// __index: computed properties first, then reflected fields, then the methods
// table (upvalue 1).
template <class T, FieldFn<T> Extra = nullptr, bool Reflect = true>
int indexOf(lua_State* L)
{
    const T& value = check<T>(L, 1);
    std::string_view key = checkString(L, 2);
    if constexpr (Extra != nullptr)
        if (Extra(L, value, key))
            return 1;
    if constexpr (Reflect)
        if (reflectedField(L, value, key))
            return 1;
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    if (!lua_isnil(L, -1))
        return 1;
    raise(L, std::format("{} is not a valid member of {}", key, TypeTraits<T>::name));
}

template <class T>
int eqOf(lua_State* L)
{
    const T* a = to<T>(L, 1);
    const T* b = to<T>(L, 2);
    lua_pushboolean(L, a && b && *a == *b);
    return 1;
}

// tostring() for Value alternatives: the engine's formatting.
template <class T>
int toStringOf(lua_State* L)
{
    pushString(L, lunar::toString(Value(check<T>(L, 1))));
    return 1;
}

// Builds the metatable for T: __index with the given methods, __type, a locked
// __metatable, the extra metamethods, and a destructor when T needs one.
// Leaves nothing on the stack.
template <class T>
void registerUserdata(lua_State* L, lua_CFunction index, std::span<const luaL_Reg> methods,
                      std::span<const luaL_Reg> metamethods)
{
    lua_newtable(L);
    lua_createtable(L, 0, static_cast<int>(methods.size()));
    for (const luaL_Reg& m : methods) {
        lua_pushcfunction(L, m.func, m.name);
        lua_setfield(L, -2, m.name);
    }
    lua_setreadonly(L, -1, true);
    lua_pushvalue(L, -1); // the methods again, for __namecall
    pushMetamethod(L, index, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, -4, "__index");
    pushNamecall(L);
    lua_setfield(L, -2, "__namecall");
    for (const luaL_Reg& m : metamethods) {
        pushMetamethod(L, m.func);
        lua_setfield(L, -2, m.name);
    }
    lua_Destructor dtor = nullptr;
    if constexpr (!std::is_trivially_destructible_v<T>)
        dtor = [](lua_State*, void* memory) { static_cast<T*>(memory)->~T(); };
    registerType(L, TypeTraits<T>::tag, TypeTraits<T>::name, dtor);
}

// A global table of constructors, plus whatever the caller sets before calling.
void setGlobalTable(lua_State* L, const char* name, std::span<const luaL_Reg> functions);

// --- Enums -------------------------------------------------------------------------

// One enum of the real client, with every item. `info` is the engine's
// EnumInfo when the engine knows the enum (so properties accept its items),
// otherwise one owned by the catalog.
struct EnumCatalogEntry {
    const EnumInfo* info = nullptr;
    std::string_view name;
    std::vector<EnumItem> items; // sorted by value, the GetEnumItems order
};

const EnumCatalogEntry* findEnumEntry(std::string_view name);
// The EnumInfo of any enum of the client, by name (enums::setCatalog's lookup).
const EnumInfo* catalogEnum(std::string_view name);
const EnumCatalogEntry* findEnumEntry(const EnumInfo* info);
std::span<const EnumCatalogEntry> enumCatalog();
// The item of an enum by name, null if there's none. Names must exist in the
// real client: used by the script library for its defaults.
const EnumItem* findEnumItem(std::string_view enumName, std::string_view itemName);
EnumItem enumItem(std::string_view enumName, std::string_view itemName);
// Accepts an EnumItem of the enum, its name or its number, like property
// assignment; only the EnumItem itself when strict.
std::optional<EnumItem> toEnumItem(lua_State* L, int idx, std::string_view enumName, bool strict = false);

} // namespace lunar::lua

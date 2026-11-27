// Vector3 (Luau's native vector), Color3 and CFrame with Roblox's APIs.

#include "Runtime.hpp"
#include "Userdata.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <new>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <type_traits>

namespace lunar::lua {

namespace {

float checkFloat(lua_State* L, int idx)
{
    return static_cast<float>(luaL_checknumber(L, idx));
}

template <std::size_t N>
std::array<float, N> checkFloats(lua_State* L, int first)
{
    std::array<float, N> values{};
    for (std::size_t i = 0; i < N; ++i)
        values[i] = checkFloat(L, first + static_cast<int>(i));
    return values;
}

float optFloat(lua_State* L, int idx, float fallback)
{
    return static_cast<float>(luaL_optnumber(L, idx, fallback));
}

void pushNumber(lua_State* L, float value)
{
    lua_pushnumber(L, static_cast<double>(value));
}

void pushString(lua_State* L, const std::string& text)
{
    lua_pushlstring(L, text.data(), text.size());
}

// __index for a datatype: Fields pushes a property and returns true, or
// returns false for anything else. Methods sit in upvalue 1, the type name in 2.
template <bool (*Fields)(lua_State*, std::string_view)>
int indexWith(lua_State* L)
{
    std::string_view key = checkString(L, 2);
    if (Fields(L, key))
        return 1;
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    if (!lua_isnil(L, -1))
        return 1;
    raise(L, std::format("{} is not a valid member of {}", key, lua_tostring(L, lua_upvalueindex(2))));
}

void setFunctions(lua_State* L, std::span<const luaL_Reg> functions)
{
    for (const luaL_Reg& f : functions) {
        lua_pushcfunction(L, f.func, f.name);
        lua_setfield(L, -2, f.name);
    }
}

// Sets __index (and the __namecall that goes with it) on the metatable on top
// of the stack.
void setIndex(lua_State* L, lua_CFunction index, std::span<const luaL_Reg> methods, const char* typeName)
{
    lua_createtable(L, 0, static_cast<int>(methods.size()));
    setFunctions(L, methods);
    lua_setreadonly(L, -1, true);
    lua_pushvalue(L, -1); // the methods again, for __namecall
    lua_pushstring(L, typeName);
    pushMetamethod(L, index, 2);
    lua_pushvalue(L, -1);
    lua_setfield(L, -4, "__index");
    pushNamecall(L);
    lua_setfield(L, -2, "__namecall");
}

void setMetamethod(lua_State* L, const char* name, lua_CFunction fn)
{
    pushMetamethod(L, fn);
    lua_setfield(L, -2, name);
}

// --- Vector3 -----------------------------------------------------------------

Vector3 vmin(Vector3 a, Vector3 b)
{
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

Vector3 vmax(Vector3 a, Vector3 b)
{
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

template <float (*F)(float)>
Vector3 apply(Vector3 v)
{
    return {F(v.x), F(v.y), F(v.z)};
}

float sign(float x)
{
    return x > 0.0f ? 1.0f : x < 0.0f ? -1.0f : 0.0f;
}

float absf(float x)
{
    return std::fabs(x);
}

float floorf(float x)
{
    return std::floor(x);
}

float ceilf(float x)
{
    return std::ceil(x);
}

Vector3 lerp(Vector3 a, Vector3 b, float t)
{
    return a + (b - a) * t;
}

std::string formatVector(Vector3 v)
{
    return std::format("{}, {}, {}", v.x, v.y, v.z);
}

int vectorNew(lua_State* L)
{
    pushVector3(L, {optFloat(L, 1, 0.0f), optFloat(L, 2, 0.0f), optFloat(L, 3, 0.0f)});
    return 1;
}

bool vectorFields(lua_State* L, std::string_view key)
{
    Vector3 v = checkVector3(L, 1);
    // The VM reads X, Y and Z (either case) itself when the key is a
    // constant; a key the code computed (v[k]) comes here.
    if (key.size() == 1 && (key[0] | ' ') >= 'x' && (key[0] | ' ') <= 'z') {
        const int axis = (key[0] | ' ') - 'x';
        pushNumber(L, axis == 0 ? v.x : axis == 1 ? v.y : v.z);
        return true;
    }
    if (key == "Magnitude" || key == "magnitude")
        pushNumber(L, v.magnitude());
    else if (key == "Unit" || key == "unit")
        pushVector3(L, v.unit());
    else
        return false;
    return true;
}

int vectorDot(lua_State* L)
{
    pushNumber(L, checkVector3(L, 1).dot(checkVector3(L, 2)));
    return 1;
}

int vectorCross(lua_State* L)
{
    pushVector3(L, checkVector3(L, 1).cross(checkVector3(L, 2)));
    return 1;
}

int vectorLerp(lua_State* L)
{
    const Vector3 a = checkVector3(L, 1);
    const Vector3 b = checkVector3(L, 2);
    pushVector3(L, lerp(a, b, checkFloat(L, 3)));
    return 1;
}

template <Vector3 (*F)(Vector3, Vector3)>
int vectorFold(lua_State* L)
{
    Vector3 result = checkVector3(L, 1);
    for (int i = 2; i <= lua_gettop(L); ++i)
        result = F(result, checkVector3(L, i));
    pushVector3(L, result);
    return 1;
}

template <float (*F)(float)>
int vectorApply(lua_State* L)
{
    pushVector3(L, apply<F>(checkVector3(L, 1)));
    return 1;
}

int vectorFuzzyEq(lua_State* L)
{
    Vector3 a = checkVector3(L, 1);
    Vector3 b = checkVector3(L, 2);
    const float epsilon = optFloat(L, 3, 1e-5f);
    lua_pushboolean(L, std::fabs(a.x - b.x) <= epsilon && std::fabs(a.y - b.y) <= epsilon
                           && std::fabs(a.z - b.z) <= epsilon);
    return 1;
}

int vectorAngle(lua_State* L)
{
    Vector3 a = checkVector3(L, 1);
    Vector3 b = checkVector3(L, 2);
    float angle = std::atan2(a.cross(b).magnitude(), a.dot(b));
    if (!lua_isnoneornil(L, 3) && checkVector3(L, 3).dot(a.cross(b)) < 0.0f)
        angle = -angle;
    pushNumber(L, angle);
    return 1;
}

// Vector3.FromNormalId(Enum.NormalId.X): the face's outward unit vector.
// Like any enum argument, the item's name or value does too.
int vectorFromNormalId(lua_State* L)
{
    if (lua_isnoneornil(L, 1))
        luaL_error(L, "Argument 1 missing or nil");
    const std::optional<EnumItem> item = toEnumItem(L, 1, "NormalId");
    if (!item || item->value > 5)
        luaL_error(L, "Unable to cast %s to NormalId", luaL_typename(L, 1));
    static constexpr Vector3 normals[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    pushVector3(L, normals[item->value]);
    return 1;
}

// Vector3.FromAxis(Enum.Axis.X): the axis' unit vector. Also from the
// axis' name or a number below 3 (cut down to a whole one), and from a
// face's name for the axis through it [Studio: "Top" and "Bottom" give
// 0, 1, 0, 1.7 gives 0, 1, 0; "x" and Enum.NormalId.Top don't cast].
int vectorFromAxis(lua_State* L)
{
    if (lua_isnoneornil(L, 1))
        luaL_error(L, "Argument 1 missing or nil");
    int axis = -1;
    if (const EnumItem* item = to<EnumItem>(L, 1)) {
        if (item->enumType && item->enumType->name == "Axis")
            axis = static_cast<int>(item->value);
        else
            luaL_error(L, "Unable to cast %s to Axis", item->enumType ? std::string(item->enumType->name).c_str() : "EnumItem");
    } else if (lua_type(L, 1) == LUA_TNUMBER) {
        const double value = lua_tonumber(L, 1);
        if (value >= 0 && value < 3)
            axis = static_cast<int>(value);
    } else if (lua_type(L, 1) == LUA_TSTRING) {
        static constexpr std::pair<std::string_view, int> names[]
            = {{"X", 0}, {"Y", 1}, {"Z", 2},      {"Left", 0},  {"Right", 0},
               {"Top", 1}, {"Bottom", 1}, {"Front", 2}, {"Back", 2}};
        const std::string_view name = lua_tostring(L, 1);
        for (const auto& [n, a] : names)
            if (n == name)
                axis = a;
    }
    if (axis < 0)
        luaL_error(L, "Unable to cast %s to Axis", lua_type(L, 1) == LUA_TNUMBER ? "double" : luaL_typename(L, 1));
    static constexpr Vector3 axes[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    pushVector3(L, axes[axis]);
    return 1;
}

int vectorToString(lua_State* L)
{
    pushString(L, formatVector(checkVector3(L, 1)));
    return 1;
}

constexpr auto vectorMethods = std::to_array<luaL_Reg>({
    {"Abs", vectorApply<absf>},
    {"Angle", vectorAngle},
    {"Ceil", vectorApply<ceilf>},
    {"Cross", vectorCross},
    {"Dot", vectorDot},
    {"Floor", vectorApply<floorf>},
    {"FuzzyEq", vectorFuzzyEq},
    {"isClose", vectorFuzzyEq},
    {"lerp", vectorLerp},
    {"Lerp", vectorLerp},
    {"Max", vectorFold<vmax>},
    {"Min", vectorFold<vmin>},
    {"Sign", vectorApply<sign>},
});

void openVector3(lua_State* L)
{
    lua_newtable(L);
    lua_pushcfunction(L, vectorNew, "new");
    lua_setfield(L, -2, "new");
    for (const char* name : {"FromNormalId", "fromNormalId"}) {
        lua_pushcfunction(L, vectorFromNormalId, name);
        lua_setfield(L, -2, name);
    }
    for (const char* name : {"FromAxis", "fromAxis"}) {
        lua_pushcfunction(L, vectorFromAxis, name);
        lua_setfield(L, -2, name);
    }
    const std::pair<const char*, Vector3> constants[] = {
        {"zero", {0, 0, 0}}, {"one", {1, 1, 1}}, {"xAxis", {1, 0, 0}}, {"yAxis", {0, 1, 0}}, {"zAxis", {0, 0, 1}},
    };
    for (const auto& [name, value] : constants) {
        pushVector3(L, value);
        lua_setfield(L, -2, name);
    }
    lua_setglobal(L, "Vector3");

    // Native vectors share one metatable; X/Y/Z are handled by the VM itself.
    pushVector3(L, {});
    lua_newtable(L);
    setIndex(L, indexWith<vectorFields>, vectorMethods, "Vector3");
    setMetamethod(L, "__tostring", vectorToString);
    lua_pushliteral(L, "Vector3");
    lua_setfield(L, -2, "__type");
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, true);
    lua_setmetatable(L, -2);
    lua_pop(L, 1);
}

// --- Color3 ------------------------------------------------------------------

Color3 fromHSV(float h, float s, float v)
{
    h = h - std::floor(h);
    const float sector = h * 6.0f;
    const int i = static_cast<int>(sector) % 6;
    const float f = sector - std::floor(sector);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));
    switch (i) {
    case 0:
        return {v, t, p};
    case 1:
        return {q, v, p};
    case 2:
        return {p, v, t};
    case 3:
        return {p, q, v};
    case 4:
        return {t, p, v};
    default:
        return {v, p, q};
    }
}

std::array<float, 3> toHSV(Color3 c)
{
    const float max = std::max({c.r, c.g, c.b});
    const float min = std::min({c.r, c.g, c.b});
    const float delta = max - min;
    float h = 0.0f;
    if (delta > 0.0f) {
        if (max == c.r)
            h = (c.g - c.b) / delta;
        else if (max == c.g)
            h = (c.b - c.r) / delta + 2.0f;
        else
            h = (c.r - c.g) / delta + 4.0f;
        h /= 6.0f;
        if (h < 0.0f)
            h += 1.0f;
    }
    return {h, max > 0.0f ? delta / max : 0.0f, max};
}

int toByte(float channel)
{
    return static_cast<int>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
}

int colorNew(lua_State* L)
{
    pushColor3(L, {optFloat(L, 1, 0.0f), optFloat(L, 2, 0.0f), optFloat(L, 3, 0.0f)});
    return 1;
}

int colorFromRGB(lua_State* L)
{
    pushColor3(L, {optFloat(L, 1, 0.0f) / 255.0f, optFloat(L, 2, 0.0f) / 255.0f, optFloat(L, 3, 0.0f) / 255.0f});
    return 1;
}

int colorFromHSV(lua_State* L)
{
    const auto [h, s, v] = checkFloats<3>(L, 1);
    pushColor3(L, fromHSV(h, s, v));
    return 1;
}

int colorToHSV(lua_State* L)
{
    for (float component : toHSV(checkColor3(L, 1)))
        pushNumber(L, component);
    return 3;
}

int colorFromHex(lua_State* L)
{
    std::string_view hex = checkString(L, 1);
    if (hex.starts_with('#'))
        hex.remove_prefix(1);
    unsigned value = 0;
    auto [end, error] = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
    if (error != std::errc() || end != hex.data() + hex.size() || (hex.size() != 6 && hex.size() != 3))
        raise(L, "Unable to convert characters to hex value");
    if (hex.size() == 3) // #rgb -> #rrggbb
        value = ((value >> 8) & 0xFu) * 0x110000u + ((value >> 4) & 0xFu) * 0x1100u + (value & 0xFu) * 0x11u;
    pushColor3(L, Color3::fromRGB(static_cast<int>(value >> 16), static_cast<int>((value >> 8) & 0xFFu),
                                  static_cast<int>(value & 0xFFu)));
    return 1;
}

bool colorFields(lua_State* L, std::string_view key)
{
    Color3 c = checkColor3(L, 1);
    if (key == "R" || key == "r")
        pushNumber(L, c.r);
    else if (key == "G" || key == "g")
        pushNumber(L, c.g);
    else if (key == "B" || key == "b")
        pushNumber(L, c.b);
    else
        return false;
    return true;
}

int colorLerp(lua_State* L)
{
    Color3 a = checkColor3(L, 1);
    Color3 b = checkColor3(L, 2);
    const float t = checkFloat(L, 3);
    pushColor3(L, {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t});
    return 1;
}

int colorToHex(lua_State* L)
{
    Color3 c = checkColor3(L, 1);
    pushString(L, std::format("{:02x}{:02x}{:02x}", toByte(c.r), toByte(c.g), toByte(c.b)));
    return 1;
}

int colorEq(lua_State* L)
{
    lua_pushboolean(L, checkColor3(L, 1) == checkColor3(L, 2));
    return 1;
}

int colorToString(lua_State* L)
{
    Color3 c = checkColor3(L, 1);
    pushString(L, std::format("{}, {}, {}", c.r, c.g, c.b));
    return 1;
}

constexpr auto colorMethods = std::to_array<luaL_Reg>({
    {"Lerp", colorLerp},
    {"lerp", colorLerp},
    {"ToHSV", colorToHSV},
    {"ToHex", colorToHex},
});

void openColor3(lua_State* L)
{
    lua_newtable(L);
    setFunctions(L, std::to_array<luaL_Reg>({
                        {"new", colorNew},
                        {"fromRGB", colorFromRGB},
                        {"fromHSV", colorFromHSV},
                        {"toHSV", colorToHSV},
                        {"fromHex", colorFromHex},
                    }));
    lua_setglobal(L, "Color3");

    lua_newtable(L);
    setIndex(L, indexWith<colorFields>, colorMethods, "Color3");
    setMetamethod(L, "__eq", colorEq);
    setMetamethod(L, "__tostring", colorToString);
    registerType(L, Color3Tag, "Color3");
}

// --- CFrame ------------------------------------------------------------------

struct Quaternion {
    float x, y, z, w;
};

Quaternion toQuaternion(const CFrame& cf)
{
    const auto& m = cf.r;
    const float trace = m[0][0] + m[1][1] + m[2][2];
    Quaternion q{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25f * s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q = {0.25f * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q = {(m[0][1] + m[1][0]) / s, 0.25f * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
    } else {
        const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25f * s, (m[1][0] - m[0][1]) / s};
    }
    return q;
}

CFrame fromQuaternion(Vector3 position, Quaternion q)
{
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length > 0.0f)
        q = {q.x / length, q.y / length, q.z / length, q.w / length};
    const auto [x, y, z, w] = q;
    CFrame cf(position);
    cf.r[0][0] = 1 - 2 * (y * y + z * z);
    cf.r[0][1] = 2 * (x * y - z * w);
    cf.r[0][2] = 2 * (x * z + y * w);
    cf.r[1][0] = 2 * (x * y + z * w);
    cf.r[1][1] = 1 - 2 * (x * x + z * z);
    cf.r[1][2] = 2 * (y * z - x * w);
    cf.r[2][0] = 2 * (x * z - y * w);
    cf.r[2][1] = 2 * (y * z + x * w);
    cf.r[2][2] = 1 - 2 * (x * x + y * y);
    return cf;
}

Quaternion slerp(Quaternion a, Quaternion b, float t)
{
    float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cosine < 0.0f) { // take the short way round
        b = {-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }
    float wa = 1.0f - t;
    float wb = t;
    if (cosine < 0.9995f) {
        const float angle = std::acos(cosine);
        const float sine = std::sin(angle);
        wa = std::sin((1.0f - t) * angle) / sine;
        wb = std::sin(t * angle) / sine;
    }
    return {wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w};
}

CFrame withRotation(Vector3 position, const CFrame& rotation)
{
    CFrame cf = rotation;
    cf.position = position;
    return cf;
}

CFrame fromColumns(Vector3 position, Vector3 right, Vector3 up, Vector3 back)
{
    CFrame cf(position);
    const Vector3 columns[] = {right, up, back};
    for (int j = 0; j < 3; ++j) {
        cf.r[0][j] = columns[j].x;
        cf.r[1][j] = columns[j].y;
        cf.r[2][j] = columns[j].z;
    }
    return cf;
}

CFrame lookAt(Vector3 at, Vector3 target, Vector3 up)
{
    // Nowhere to look: no turn at all rather than NaN (GUESS: the client's
    // answer; the game looks along a flattened camera direction that can be zero).
    if ((target - at).magnitude() < 1e-6f)
        return CFrame(at);
    const Vector3 look = (target - at).unit();
    Vector3 right = look.cross(up);
    if (right.magnitude() < 1e-6f) // looking along `up`
        right = look.cross(Vector3{0.0f, 0.0f, 1.0f});
    right = right.unit();
    return fromColumns(at, right, right.cross(look), -look);
}

CFrame fromAxisAngle(Vector3 axis, float angle)
{
    const Vector3 unit = axis.unit();
    const float s = std::sin(angle / 2.0f);
    return fromQuaternion({}, {unit.x * s, unit.y * s, unit.z * s, std::cos(angle / 2.0f)});
}

CFrame anglesYXZ(float rx, float ry, float rz)
{
    return CFrame::angles(0.0f, ry, 0.0f) * CFrame::angles(rx, 0.0f, 0.0f) * CFrame::angles(0.0f, 0.0f, rz);
}

float asinClamped(float x)
{
    return std::asin(std::clamp(x, -1.0f, 1.0f));
}

std::array<float, 3> eulerXYZ(const CFrame& cf)
{
    const auto& m = cf.r;
    return {std::atan2(-m[1][2], m[2][2]), asinClamped(m[0][2]), std::atan2(-m[0][1], m[0][0])};
}

std::array<float, 3> eulerYXZ(const CFrame& cf)
{
    const auto& m = cf.r;
    return {asinClamped(-m[1][2]), std::atan2(m[0][2], m[2][2]), std::atan2(m[1][0], m[1][1])};
}

int pushEuler(lua_State* L, std::array<float, 3> angles)
{
    for (float angle : angles)
        pushNumber(L, angle);
    return 3;
}

int cframeNew(lua_State* L)
{
    const int n = lua_gettop(L);
    switch (n) {
    case 0:
        pushCFrame(L, CFrame());
        return 1;
    case 1:
        pushCFrame(L, CFrame(checkVector3(L, 1)));
        return 1;
    case 2: {
        const Vector3 at = checkVector3(L, 1);
        pushCFrame(L, lookAt(at, checkVector3(L, 2), {0.0f, 1.0f, 0.0f}));
        return 1;
    }
    case 3:
        pushCFrame(L, CFrame{checkFloat(L, 1), checkFloat(L, 2), checkFloat(L, 3)});
        return 1;
    case 7:
        pushCFrame(L, fromQuaternion({checkFloat(L, 1), checkFloat(L, 2), checkFloat(L, 3)},
                                     {checkFloat(L, 4), checkFloat(L, 5), checkFloat(L, 6), checkFloat(L, 7)}));
        return 1;
    case 12: {
        CFrame cf{checkFloat(L, 1), checkFloat(L, 2), checkFloat(L, 3)};
        for (int i = 0; i < 9; ++i)
            cf.r[i / 3][i % 3] = checkFloat(L, 4 + i);
        pushCFrame(L, cf);
        return 1;
    }
    default:
        raise(L, std::format("Invalid number of arguments: {}", n));
    }
}

int cframeAngles(lua_State* L)
{
    const float rx = optFloat(L, 1, 0.0f);
    const float ry = optFloat(L, 2, 0.0f);
    pushCFrame(L, CFrame::angles(rx, ry, optFloat(L, 3, 0.0f)));
    return 1;
}

int cframeAnglesYXZ(lua_State* L)
{
    const float rx = optFloat(L, 1, 0.0f);
    const float ry = optFloat(L, 2, 0.0f);
    pushCFrame(L, anglesYXZ(rx, ry, optFloat(L, 3, 0.0f)));
    return 1;
}

int cframeLookAt(lua_State* L)
{
    Vector3 up = lua_isnoneornil(L, 3) ? Vector3{0.0f, 1.0f, 0.0f} : checkVector3(L, 3);
    const Vector3 at = checkVector3(L, 1);
    pushCFrame(L, lookAt(at, checkVector3(L, 2), up));
    return 1;
}

int cframeFromAxisAngle(lua_State* L)
{
    const Vector3 axis = checkVector3(L, 1);
    pushCFrame(L, fromAxisAngle(axis, checkFloat(L, 2)));
    return 1;
}

int cframeFromMatrix(lua_State* L)
{
    Vector3 right = checkVector3(L, 2);
    Vector3 up = checkVector3(L, 3);
    Vector3 back;
    if (!lua_isnoneornil(L, 4)) {
        back = checkVector3(L, 4);
    } else {
        // right x up made a unit, or left zero when it is (zero or parallel
        // vectors): Studio's fromMatrix gives zeros there, not NaN.
        back = right.cross(up);
        if (const float length = back.magnitude(); length > 0.0f)
            back = back / length;
    }
    pushCFrame(L, fromColumns(checkVector3(L, 1), right, up, back));
    return 1;
}

bool cframeFields(lua_State* L, std::string_view key)
{
    const CFrame& cf = checkCFrame(L, 1);
    if (key == "Position" || key == "p")
        pushVector3(L, cf.position);
    else if (key == "X" || key == "x")
        pushNumber(L, cf.position.x);
    else if (key == "Y" || key == "y")
        pushNumber(L, cf.position.y);
    else if (key == "Z" || key == "z")
        pushNumber(L, cf.position.z);
    else if (key == "LookVector" || key == "lookVector")
        pushVector3(L, cf.lookVector());
    else if (key == "RightVector" || key == "XVector" || key == "rightVector")
        pushVector3(L, cf.rightVector());
    else if (key == "UpVector" || key == "YVector" || key == "upVector")
        pushVector3(L, cf.upVector());
    else if (key == "ZVector")
        pushVector3(L, -cf.lookVector());
    else if (key == "Rotation")
        pushCFrame(L, withRotation({}, cf));
    else
        return false;
    return true;
}

int cframeInverse(lua_State* L)
{
    pushCFrame(L, checkCFrame(L, 1).inverse());
    return 1;
}

int cframeLerp(lua_State* L)
{
    const CFrame& a = checkCFrame(L, 1);
    const CFrame& b = checkCFrame(L, 2);
    const float t = checkFloat(L, 3);
    pushCFrame(L, fromQuaternion(lerp(a.position, b.position, t), slerp(toQuaternion(a), toQuaternion(b), t)));
    return 1;
}

// The space conversions take any number of arguments and return as many results.
template <class Arg, auto Convert>
int convertEach(lua_State* L)
{
    const CFrame self = checkCFrame(L, 1);
    const int n = lua_gettop(L);
    for (int i = 2; i <= n; ++i) {
        if constexpr (std::is_same_v<Arg, CFrame>)
            pushCFrame(L, Convert(self, checkCFrame(L, i)));
        else
            pushVector3(L, Convert(self, checkVector3(L, i)));
    }
    return n - 1;
}

constexpr auto toWorldSpace = [](const CFrame& self, const CFrame& cf) { return self * cf; };
constexpr auto toObjectSpace = [](const CFrame& self, const CFrame& cf) { return self.inverse() * cf; };
constexpr auto pointToWorldSpace = [](const CFrame& self, Vector3 v) { return self.pointToWorldSpace(v); };
constexpr auto pointToObjectSpace = [](const CFrame& self, Vector3 v) { return self.pointToObjectSpace(v); };
constexpr auto vectorToWorldSpace = [](const CFrame& self, Vector3 v) { return self.vectorToWorldSpace(v); };
constexpr auto vectorToObjectSpace = [](const CFrame& self, Vector3 v) { return self.vectorToObjectSpace(v); };

int cframeGetComponents(lua_State* L)
{
    const CFrame& cf = checkCFrame(L, 1);
    pushNumber(L, cf.position.x);
    pushNumber(L, cf.position.y);
    pushNumber(L, cf.position.z);
    for (const auto& row : cf.r)
        for (float value : row)
            pushNumber(L, value);
    return 12;
}

int cframeToEulerXYZ(lua_State* L)
{
    return pushEuler(L, eulerXYZ(checkCFrame(L, 1)));
}

int cframeToEulerYXZ(lua_State* L)
{
    return pushEuler(L, eulerYXZ(checkCFrame(L, 1)));
}

int cframeToAxisAngle(lua_State* L)
{
    Quaternion q = toQuaternion(checkCFrame(L, 1));
    const float angle = 2.0f * std::acos(std::clamp(q.w, -1.0f, 1.0f));
    const float s = std::sqrt(std::max(1.0f - q.w * q.w, 0.0f));
    pushVector3(L, s < 1e-6f ? Vector3{1.0f, 0.0f, 0.0f} : Vector3{q.x / s, q.y / s, q.z / s});
    pushNumber(L, angle);
    return 2;
}

int cframeMul(lua_State* L)
{
    const CFrame* cf = toCFrame(L, 1);
    const CFrame* other = toCFrame(L, 2);
    auto v = toVector3(L, 2);
    if (cf && other)
        pushCFrame(L, *cf * *other);
    else if (cf && v)
        pushVector3(L, *cf * *v);
    else
        raise(L, std::format("attempt to perform arithmetic (mul) on {} and {}", luaL_typename(L, 1),
                             luaL_typename(L, 2)));
    return 1;
}

template <bool Add>
int cframeAddSub(lua_State* L)
{
    const CFrame* cf = toCFrame(L, 1);
    auto v = toVector3(L, 2);
    if (!cf || !v)
        raise(L, std::format("attempt to perform arithmetic ({}) on {} and {}", Add ? "add" : "sub",
                             luaL_typename(L, 1), luaL_typename(L, 2)));
    pushCFrame(L, Add ? *cf + *v : *cf - *v);
    return 1;
}

int cframeEq(lua_State* L)
{
    lua_pushboolean(L, checkCFrame(L, 1) == checkCFrame(L, 2));
    return 1;
}

int cframeToString(lua_State* L)
{
    const CFrame& cf = checkCFrame(L, 1);
    std::string text = formatVector(cf.position);
    for (const auto& row : cf.r)
        for (float value : row)
            text += std::format(", {}", value);
    pushString(L, text);
    return 1;
}

// CFrame.lookAlong(at, direction, up).
int cframeLookAlong(lua_State* L)
{
    const Vector3 at = checkVector3(L, 1);
    const Vector3 direction = checkVector3(L, 2);
    const Vector3 up = lua_isnoneornil(L, 3) ? Vector3{0.0f, 1.0f, 0.0f} : checkVector3(L, 3);
    pushCFrame(L, lookAt(at, at + direction, up));
    return 1;
}

// The shortest rotation taking `from` onto `to`.
int cframeFromRotationBetweenVectors(lua_State* L)
{
    const Vector3 from = checkVector3(L, 1).unit();
    const Vector3 to = checkVector3(L, 2).unit();
    const float d = from.dot(to);
    Vector3 axis = from.cross(to);
    if (axis.magnitude() < 1e-6f) {
        if (d > 0.0f) {
            pushCFrame(L, CFrame());
            return 1;
        }
        // Opposite: any perpendicular axis, half a turn.
        axis = std::fabs(from.x) < 0.9f ? from.cross(Vector3{1, 0, 0}) : from.cross(Vector3{0, 1, 0});
        pushCFrame(L, fromAxisAngle(axis, std::numbers::pi_v<float>));
        return 1;
    }
    pushCFrame(L, fromAxisAngle(axis, std::atan2(axis.magnitude(), d)));
    return 1;
}

int cframeFuzzyEq(lua_State* L)
{
    const int n = lua_gettop(L);
    if (n < 2 || n > 3)
        raise(L, std::format("Invalid number of arguments: {}, CFrame.FuzzyEq expects 2 or 3.", n));
    const CFrame& a = checkCFrame(L, 1);
    const CFrame& b = checkCFrame(L, 2);
    const auto eps = static_cast<float>(luaL_optnumber(L, 3, 1e-5));
    bool equal = (a.position - b.position).magnitude() <= eps;
    for (int i = 0; i < 3 && equal; ++i)
        for (int j = 0; j < 3 && equal; ++j)
            equal = std::fabs(a.r[i][j] - b.r[i][j]) <= eps;
    lua_pushboolean(L, equal);
    return 1;
}

int cframeAngleBetween(lua_State* L)
{
    const int n = lua_gettop(L);
    if (n != 2)
        raise(L, std::format("Invalid number of arguments. CFrame:AngleBetween expected 2, got {}.", n));
    const CFrame relative = checkCFrame(L, 1).inverse() * checkCFrame(L, 2);
    const float trace = relative.r[0][0] + relative.r[1][1] + relative.r[2][2];
    pushNumber(L, std::acos(std::clamp((trace - 1.0f) / 2.0f, -1.0f, 1.0f)));
    return 1;
}

int cframeOrthonormalize(lua_State* L)
{
    const CFrame& cf = checkCFrame(L, 1);
    const Vector3 right = cf.rightVector().unit();
    const Vector3 back = right.cross(cf.upVector()).unit();
    pushCFrame(L, fromColumns(cf.position, right, back.cross(right), back));
    return 1;
}

constexpr auto cframeMethods = std::to_array<luaL_Reg>({
    {"AngleBetween", cframeAngleBetween},
    {"FuzzyEq", cframeFuzzyEq},
    {"Orthonormalize", cframeOrthonormalize},
    {"pointToObjectSpace", convertEach<Vector3, pointToObjectSpace>},
    {"pointToWorldSpace", convertEach<Vector3, pointToWorldSpace>},
    {"toAxisAngle", cframeToAxisAngle},
    {"toEulerAngles", cframeToEulerXYZ},
    {"toEulerAnglesXYZ", cframeToEulerXYZ},
    {"toEulerAnglesYXZ", cframeToEulerYXZ},
    {"toObjectSpace", convertEach<CFrame, toObjectSpace>},
    {"toOrientation", cframeToEulerYXZ},
    {"toWorldSpace", convertEach<CFrame, toWorldSpace>},
    {"vectorToObjectSpace", convertEach<Vector3, vectorToObjectSpace>},
    {"vectorToWorldSpace", convertEach<Vector3, vectorToWorldSpace>},
    {"GetComponents", cframeGetComponents},
    {"components", cframeGetComponents},
    {"Inverse", cframeInverse},
    {"inverse", cframeInverse},
    {"Lerp", cframeLerp},
    {"lerp", cframeLerp},
    {"PointToObjectSpace", convertEach<Vector3, pointToObjectSpace>},
    {"PointToWorldSpace", convertEach<Vector3, pointToWorldSpace>},
    {"ToAxisAngle", cframeToAxisAngle},
    {"ToEulerAngles", cframeToEulerXYZ},
    {"ToEulerAnglesXYZ", cframeToEulerXYZ},
    {"ToEulerAnglesYXZ", cframeToEulerYXZ},
    {"ToObjectSpace", convertEach<CFrame, toObjectSpace>},
    {"ToOrientation", cframeToEulerYXZ},
    {"ToWorldSpace", convertEach<CFrame, toWorldSpace>},
    {"VectorToObjectSpace", convertEach<Vector3, vectorToObjectSpace>},
    {"VectorToWorldSpace", convertEach<Vector3, vectorToWorldSpace>},
});

void openCFrame(lua_State* L)
{
    lua_newtable(L);
    setIndex(L, indexWith<cframeFields>, cframeMethods, "CFrame");
    setMetamethod(L, "__mul", cframeMul);
    setMetamethod(L, "__add", cframeAddSub<true>);
    setMetamethod(L, "__sub", cframeAddSub<false>);
    setMetamethod(L, "__eq", cframeEq);
    setMetamethod(L, "__tostring", cframeToString);
    registerType(L, CFrameTag, "CFrame");

    lua_newtable(L);
    setFunctions(L, std::to_array<luaL_Reg>({
                        {"new", cframeNew},
                        {"Angles", cframeAngles},
                        {"fromEulerAnglesXYZ", cframeAngles},
                        {"fromEulerAngles", cframeAngles},
                        {"fromEulerAnglesYXZ", cframeAnglesYXZ},
                        {"fromOrientation", cframeAnglesYXZ},
                        {"lookAt", cframeLookAt},
                        {"fromAxisAngle", cframeFromAxisAngle},
                        {"fromMatrix", cframeFromMatrix},
                        {"lookAlong", cframeLookAlong},
                        {"fromRotationBetweenVectors", cframeFromRotationBetweenVectors},
                    }));
    pushCFrame(L, CFrame());
    lua_setfield(L, -2, "identity");
    lua_setglobal(L, "CFrame");
}

} // namespace

void pushVector3(lua_State* L, Vector3 v)
{
    lua_pushvector(L, v.x, v.y, v.z);
}

std::optional<Vector3> toVector3(lua_State* L, int idx)
{
    const float* v = lua_tovector(L, idx);
    if (!v)
        return std::nullopt;
    return Vector3{v[0], v[1], v[2]};
}

Vector3 checkVector3(lua_State* L, int idx)
{
    if (auto v = toVector3(L, idx))
        return *v;
    luaL_typeerror(L, idx, "Vector3");
}

void pushColor3(lua_State* L, Color3 c)
{
    new (lua_newuserdatataggedwithmetatable(L, sizeof(Color3), Color3Tag)) Color3(c);
}

const Color3* toColor3(lua_State* L, int idx)
{
    return static_cast<const Color3*>(lua_touserdatatagged(L, idx, Color3Tag));
}

Color3 checkColor3(lua_State* L, int idx)
{
    if (const Color3* c = toColor3(L, idx))
        return *c;
    luaL_typeerror(L, idx, "Color3");
}

void pushCFrame(lua_State* L, const CFrame& cf)
{
    new (lua_newuserdatataggedwithmetatable(L, sizeof(CFrame), CFrameTag)) CFrame(cf);
}

const CFrame* toCFrame(lua_State* L, int idx)
{
    return static_cast<const CFrame*>(lua_touserdatatagged(L, idx, CFrameTag));
}

const CFrame& checkCFrame(lua_State* L, int idx)
{
    if (const CFrame* cf = toCFrame(L, idx))
        return *cf;
    luaL_typeerror(L, idx, "CFrame");
}

void openDatatypes(lua_State* L)
{
    openVector3(L);
    openColor3(L);
    openCFrame(L);
    openEnums(L);
    openValueTypes(L);
    openScriptTypes(L);
}

CFrame lookAtCFrame(Vector3 at, Vector3 target, Vector3 up)
{
    return lookAt(at, target, up);
}

} // namespace lunar::lua

// The datatypes that are engine Value alternatives besides Vector3, Color3 and
// CFrame: Vector2, UDim, UDim2, Rect, NumberRange, NumberSequence(+Keypoint),
// ColorSequence(+Keypoint), BrickColor, Faces, Axes, PhysicalProperties,
// Vector3int16 and Font.
//
// Member names, constructor overloads and error messages follow the 0.742
// client: its strings list each type's members next to messages such as
// "NumberSequence must end at time=1.0" or "Faces.new expects Enum.NormalId
// inputs", which are used word for word. tostring() goes through the engine's
// toString(Value), so properties and scripts print the same.

#include "Userdata.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <utility>

namespace lunar::lua {

namespace {

float checkFloat(lua_State* L, int idx)
{
    return static_cast<float>(luaL_checknumber(L, idx));
}

float optFloat(lua_State* L, int idx, float fallback)
{
    return static_cast<float>(luaL_optnumber(L, idx, fallback));
}

// Roblox stores UDim offsets as int32 and truncates like a C cast. (Guess:
// truncation rather than rounding isn't confirmed against the client.)
std::int32_t toOffset(double value)
{
    if (!std::isfinite(value))
        return 0;
    return static_cast<std::int32_t>(std::clamp(value, -2147483648.0, 2147483647.0));
}

// --- Vector2 -------------------------------------------------------------------

bool vector2Extra(lua_State* L, const Vector2& v, std::string_view key)
{
    // The deprecated lower-case spellings too (the game's camera reads .y every frame).
    if (key == "x")
        pushNumber(L, v.x);
    else if (key == "y")
        pushNumber(L, v.y);
    else if (key == "Magnitude" || key == "magnitude")
        pushNumber(L, v.magnitude());
    else if (key == "Unit" || key == "unit")
        push<Vector2>(L, v.unit());
    else
        return false;
    return true;
}

int vector2New(lua_State* L)
{
    push<Vector2>(L, Vector2{optFloat(L, 1, 0.0f), optFloat(L, 2, 0.0f)});
    return 1;
}

int vector2Dot(lua_State* L)
{
    pushNumber(L, check<Vector2>(L, 1).dot(check<Vector2>(L, 2)));
    return 1;
}

int vector2Cross(lua_State* L)
{
    pushNumber(L, check<Vector2>(L, 1).cross(check<Vector2>(L, 2)));
    return 1;
}

int vector2Lerp(lua_State* L)
{
    Vector2 a = check<Vector2>(L, 1);
    Vector2 b = check<Vector2>(L, 2);
    push<Vector2>(L, a + (b - a) * checkFloat(L, 3));
    return 1;
}

template <float (*F)(float)>
int vector2Apply(lua_State* L)
{
    Vector2 v = check<Vector2>(L, 1);
    push<Vector2>(L, Vector2{F(v.x), F(v.y)});
    return 1;
}

float signOf(float x)
{
    return x > 0.0f ? 1.0f : x < 0.0f ? -1.0f : 0.0f;
}
float absOf(float x)
{
    return std::fabs(x);
}
float floorOf(float x)
{
    return std::floor(x);
}
float ceilOf(float x)
{
    return std::ceil(x);
}

template <bool Max>
int vector2Fold(lua_State* L)
{
    Vector2 r = check<Vector2>(L, 1);
    for (int i = 2; i <= lua_gettop(L); ++i) {
        Vector2 v = check<Vector2>(L, i);
        r = Max ? Vector2{std::max(r.x, v.x), std::max(r.y, v.y)} : Vector2{std::min(r.x, v.x), std::min(r.y, v.y)};
    }
    push<Vector2>(L, r);
    return 1;
}

int vector2Angle(lua_State* L)
{
    const int n = lua_gettop(L);
    if (n < 2 || n > 3)
        raise(L, std::format("Invalid number of arguments: {}, Vector2.Angle expects 2 or 3.", n));
    Vector2 a = check<Vector2>(L, 1);
    Vector2 b = check<Vector2>(L, 2);
    const float cross = a.cross(b);
    float angle = std::atan2(std::fabs(cross), a.dot(b));
    if (lua_toboolean(L, 3) && cross < 0.0f)
        angle = -angle;
    pushNumber(L, angle);
    return 1;
}

int vector2FuzzyEq(lua_State* L)
{
    const int n = lua_gettop(L);
    if (n < 2 || n > 3)
        raise(L, std::format("Invalid number of arguments: {}, Vector2.FuzzyEq expects 2 or 3.", n));
    Vector2 a = check<Vector2>(L, 1);
    Vector2 b = check<Vector2>(L, 2);
    const float eps = optFloat(L, 3, 1e-5f);
    if (eps < 0.0f || eps > 0.1f)
        raise(L, "The eps value provided to FuzzyEq should be a small positive value <= 0.1");
    lua_pushboolean(L, std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps);
    return 1;
}

template <char Op>
int vector2Arith(lua_State* L)
{
    const Vector2* a = to<Vector2>(L, 1);
    const Vector2* b = to<Vector2>(L, 2);
    if constexpr (Op == '+' || Op == '-') {
        if (!a || !b)
            raise(L, std::format("attempt to perform arithmetic ({}) on {} and {}", Op == '+' ? "add" : "sub",
                                 luaL_typename(L, 1), luaL_typename(L, 2)));
        push<Vector2>(L, Op == '+' ? *a + *b : *a - *b);
    } else {
        auto scalar = [&](int idx) -> std::optional<float> {
            if (lua_type(L, idx) == LUA_TNUMBER)
                return static_cast<float>(lua_tonumber(L, idx));
            return std::nullopt;
        };
        Vector2 x, y;
        const std::optional<float> s1 = scalar(1), s2 = scalar(2);
        if (a && b) {
            x = *a;
            y = *b;
        } else if (a && s2) {
            x = *a;
            y = Vector2{*s2, *s2};
        } else if (b && s1) {
            x = Vector2{*s1, *s1};
            y = *b;
        } else {
            const char* verb = Op == '*' ? "multiply" : Op == '/' ? "divide" : "floor divide";
            raise(L, std::format("attempt to {} a Vector2 with an incompatible value type or nil", verb));
        }
        Vector2 r;
        if constexpr (Op == '*')
            r = {x.x * y.x, x.y * y.y};
        else if constexpr (Op == '/')
            r = {x.x / y.x, x.y / y.y};
        else
            r = {std::floor(x.x / y.x), std::floor(x.y / y.y)};
        push<Vector2>(L, r);
    }
    return 1;
}

int vector2Unm(lua_State* L)
{
    push<Vector2>(L, -check<Vector2>(L, 1));
    return 1;
}

constexpr luaL_Reg vector2Methods[] = {
    {"Abs", vector2Apply<absOf>},
    {"Angle", vector2Angle},
    {"Ceil", vector2Apply<ceilOf>},
    {"Cross", vector2Cross},
    {"Dot", vector2Dot},
    {"Floor", vector2Apply<floorOf>},
    {"FuzzyEq", vector2FuzzyEq},
    {"Lerp", vector2Lerp},
    {"Max", vector2Fold<true>},
    {"Min", vector2Fold<false>},
    {"Sign", vector2Apply<signOf>},
    {"lerp", vector2Lerp},
};

void openVector2(lua_State* L)
{
    registerUserdata<Vector2>(L, indexOf<Vector2, vector2Extra>, vector2Methods,
                              std::to_array<luaL_Reg>({
                                  {"__add", vector2Arith<'+'>},
                                  {"__sub", vector2Arith<'-'>},
                                  {"__mul", vector2Arith<'*'>},
                                  {"__div", vector2Arith<'/'>},
                                  {"__idiv", vector2Arith<'f'>},
                                  {"__unm", vector2Unm},
                                  {"__eq", eqOf<Vector2>},
                                  {"__tostring", toStringOf<Vector2>},
                              }));
    lua_newtable(L);
    lua_pushcfunction(L, vector2New, "new");
    lua_setfield(L, -2, "new");
    const std::pair<const char*, Vector2> constants[]
        = {{"zero", {0, 0}}, {"one", {1, 1}}, {"xAxis", {1, 0}}, {"yAxis", {0, 1}}};
    for (const auto& [name, value] : constants) {
        push<Vector2>(L, value);
        lua_setfield(L, -2, name);
    }
    lua_setreadonly(L, -1, true);
    lua_setglobal(L, "Vector2");
}

// --- UDim, UDim2 ---------------------------------------------------------------

int udimNew(lua_State* L)
{
    push<UDim>(L, UDim{optFloat(L, 1, 0.0f), toOffset(luaL_optnumber(L, 2, 0.0))});
    return 1;
}

template <bool Add>
int udimArith(lua_State* L)
{
    const UDim* a = to<UDim>(L, 1);
    const UDim* b = to<UDim>(L, 2);
    if (!a || !b)
        raise(L, std::format("attempt to perform arithmetic ({}) on {} and {}", Add ? "add" : "sub",
                             luaL_typename(L, 1), luaL_typename(L, 2)));
    push<UDim>(L, Add ? UDim{a->scale + b->scale, a->offset + b->offset}
                      : UDim{a->scale - b->scale, a->offset - b->offset});
    return 1;
}

int udimUnm(lua_State* L)
{
    const UDim& u = check<UDim>(L, 1);
    push<UDim>(L, UDim{-u.scale, -u.offset});
    return 1;
}

void openUDim(lua_State* L)
{
    registerUserdata<UDim>(L, indexOf<UDim>, {},
                           std::to_array<luaL_Reg>({
                               {"__add", udimArith<true>},
                               {"__sub", udimArith<false>},
                               {"__unm", udimUnm},
                               {"__eq", eqOf<UDim>},
                               {"__tostring", toStringOf<UDim>},
                           }));
    setGlobalTable(L, "UDim", std::to_array<luaL_Reg>({{"new", udimNew}}));
}

bool udim2Extra(lua_State* L, const UDim2& u, std::string_view key)
{
    if (key == "Width")
        push<UDim>(L, u.x);
    else if (key == "Height")
        push<UDim>(L, u.y);
    else
        return false;
    return true;
}

int udim2New(lua_State* L)
{
    if (const UDim* x = to<UDim>(L, 1)) {
        push<UDim2>(L, UDim2{*x, check<UDim>(L, 2)});
        return 1;
    }
    push<UDim2>(L, UDim2{{optFloat(L, 1, 0.0f), toOffset(luaL_optnumber(L, 2, 0.0))},
                         {optFloat(L, 3, 0.0f), toOffset(luaL_optnumber(L, 4, 0.0))}});
    return 1;
}

int udim2FromScale(lua_State* L)
{
    push<UDim2>(L, UDim2::fromScale(optFloat(L, 1, 0.0f), optFloat(L, 2, 0.0f)));
    return 1;
}

int udim2FromOffset(lua_State* L)
{
    push<UDim2>(L, UDim2::fromOffset(toOffset(luaL_optnumber(L, 1, 0.0)), toOffset(luaL_optnumber(L, 2, 0.0))));
    return 1;
}

int udim2Lerp(lua_State* L)
{
    const UDim2& a = check<UDim2>(L, 1);
    const UDim2& b = check<UDim2>(L, 2);
    const double t = luaL_checknumber(L, 3);
    auto mix = [t](const UDim& x, const UDim& y) {
        return UDim{static_cast<float>(static_cast<double>(x.scale) + static_cast<double>(y.scale - x.scale) * t),
                    toOffset(x.offset + (static_cast<double>(y.offset) - x.offset) * t)};
    };
    push<UDim2>(L, UDim2{mix(a.x, b.x), mix(a.y, b.y)});
    return 1;
}

template <bool Add>
int udim2Arith(lua_State* L)
{
    const UDim2* a = to<UDim2>(L, 1);
    const UDim2* b = to<UDim2>(L, 2);
    if (!a || !b)
        raise(L, std::format("attempt to perform arithmetic ({}) on {} and {}", Add ? "add" : "sub",
                             luaL_typename(L, 1), luaL_typename(L, 2)));
    auto op = [](const UDim& x, const UDim& y) {
        return Add ? UDim{x.scale + y.scale, x.offset + y.offset} : UDim{x.scale - y.scale, x.offset - y.offset};
    };
    push<UDim2>(L, UDim2{op(a->x, b->x), op(a->y, b->y)});
    return 1;
}

int udim2Unm(lua_State* L)
{
    const UDim2& u = check<UDim2>(L, 1);
    push<UDim2>(L, UDim2{{-u.x.scale, -u.x.offset}, {-u.y.scale, -u.y.offset}});
    return 1;
}

void openUDim2(lua_State* L)
{
    registerUserdata<UDim2>(L, indexOf<UDim2, udim2Extra>, std::to_array<luaL_Reg>({{"Lerp", udim2Lerp}}),
                            std::to_array<luaL_Reg>({
                                {"__add", udim2Arith<true>},
                                {"__sub", udim2Arith<false>},
                                {"__unm", udim2Unm},
                                {"__eq", eqOf<UDim2>},
                                {"__tostring", toStringOf<UDim2>},
                            }));
    setGlobalTable(L, "UDim2",
                   std::to_array<luaL_Reg>({
                       {"new", udim2New},
                       {"fromScale", udim2FromScale},
                       {"fromOffset", udim2FromOffset},
                   }));
}

// --- Rect ----------------------------------------------------------------------

bool rectExtra(lua_State* L, const Rect& r, std::string_view key)
{
    if (key == "Width")
        pushNumber(L, r.width());
    else if (key == "Height")
        pushNumber(L, r.height());
    else
        return false;
    return true;
}

int rectNew(lua_State* L)
{
    const int n = lua_gettop(L);
    if (n == 0) {
        push<Rect>(L, Rect{});
    } else if (to<Vector2>(L, 1)) {
        push<Rect>(L, Rect{check<Vector2>(L, 1), check<Vector2>(L, 2)});
    } else if (n == 4) {
        push<Rect>(L, Rect{{checkFloat(L, 1), checkFloat(L, 2)}, {checkFloat(L, 3), checkFloat(L, 4)}});
    } else {
        raise(L, std::format("Invalid number of arguments: {}", n));
    }
    return 1;
}

void openRect(lua_State* L)
{
    registerUserdata<Rect>(L, indexOf<Rect, rectExtra>, {},
                           std::to_array<luaL_Reg>({
                               {"__eq", eqOf<Rect>},
                               {"__tostring", toStringOf<Rect>},
                           }));
    setGlobalTable(L, "Rect", std::to_array<luaL_Reg>({{"new", rectNew}}));
}

// --- NumberRange -------------------------------------------------------------------

int numberRangeNew(lua_State* L)
{
    const float min = checkFloat(L, 1);
    const float max = optFloat(L, 2, min);
    if (max < min)
        raise(L, "NumberRange: invalid range");
    push<NumberRange>(L, NumberRange{min, max});
    return 1;
}

void openNumberRange(lua_State* L)
{
    registerUserdata<NumberRange>(L, indexOf<NumberRange>, {},
                                  std::to_array<luaL_Reg>({
                                      {"__eq", eqOf<NumberRange>},
                                      {"__tostring", toStringOf<NumberRange>},
                                  }));
    setGlobalTable(L, "NumberRange", std::to_array<luaL_Reg>({{"new", numberRangeNew}}));
}

// --- Sequences ---------------------------------------------------------------------

// Shared rules for both sequence types, in the order the client checks them.
constexpr std::size_t maxKeypoints = 20;

template <class Keypoint>
void validateKeypoints(lua_State* L, const std::vector<Keypoint>& points, std::string_view type)
{
    if (points.size() < 2)
        raise(L, std::format("{}: requires at least 2 keypoints", type));
    if (points.size() > maxKeypoints)
        raise(L, type == "NumberSequence" ? std::format("{}: max. number of keypoints exceeded.", type)
                                          : std::format("{}: max number of keypoints exceeded.", type));
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!std::isfinite(points[i].time))
            raise(L, std::format("{}: time must be a valid number", type));
        if (i > 0 && points[i].time < points[i - 1].time)
            raise(L, std::format("{}: all keypoints must be ordered by time", type));
    }
    if constexpr (std::is_same_v<Keypoint, NumberSequenceKeypoint>) {
        for (const Keypoint& k : points)
            if (k.envelope < 0.0f)
                raise(L, "NumberSequence: envelope must be non-negative");
    }
    if (points.front().time != 0.0f)
        raise(L, std::format("{} must start at time=0.0", type));
    if (points.back().time != 1.0f)
        raise(L, std::format("{} must end at time=1.0", type));
}

template <class Keypoint>
std::vector<Keypoint> readKeypoints(lua_State* L, std::string_view type)
{
    std::vector<Keypoint> points;
    const int n = lua_objlen(L, 1);
    if (std::cmp_greater(n, maxKeypoints))
        raise(L, std::format("{}.new(): table is too long.", type));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        const Keypoint* k = to<Keypoint>(L, -1);
        if (!k)
            raise(L, std::format("{}.new(): expected '{}Keypoint' at index {}{}", type, type, i,
                                 type == "NumberSequence" ? "" : "."));
        points.push_back(*k);
        lua_pop(L, 1);
    }
    return points;
}

void pushKeypointTable(lua_State* L, const auto& points)
{
    lua_createtable(L, static_cast<int>(points.size()), 0);
    int i = 0;
    for (const auto& k : points) {
        pushAny(L, k);
        lua_rawseti(L, -2, ++i);
    }
}

int numberKeypointNew(lua_State* L)
{
    push<NumberSequenceKeypoint>(L, NumberSequenceKeypoint{checkFloat(L, 1), checkFloat(L, 2), optFloat(L, 3, 0.0f)});
    return 1;
}

int numberSequenceNew(lua_State* L)
{
    std::vector<NumberSequenceKeypoint> points;
    if (lua_istable(L, 1)) {
        points = readKeypoints<NumberSequenceKeypoint>(L, "NumberSequence");
    } else if (lua_type(L, 1) == LUA_TNUMBER) {
        const float a = checkFloat(L, 1);
        if (!lua_isnoneornil(L, 2) && lua_type(L, 2) != LUA_TNUMBER)
            raise(L, "NumberSequence.new() arg #2: Number expected.");
        const float b = optFloat(L, 2, a);
        points = {{0.0f, a, 0.0f}, {1.0f, b, 0.0f}};
    } else {
        raise(L, "NumberSequence.new(): table of NumberSequenceKeypoints expected.");
    }
    validateKeypoints(L, points, "NumberSequence");
    push<NumberSequence>(L, NumberSequence(std::move(points)));
    return 1;
}

bool numberSequenceExtra(lua_State* L, const NumberSequence& s, std::string_view key)
{
    if (key != "Keypoints")
        return false;
    pushKeypointTable(L, s.keypoints);
    return true;
}

int colorKeypointNew(lua_State* L)
{
    const float time = checkFloat(L, 1);
    const Color3* color = toColor3(L, 2);
    if (!color)
        raise(L, "could not parse arg #2 to ColorSequenceKeypoint.new(), Color3 expected.");
    push<ColorSequenceKeypoint>(L, ColorSequenceKeypoint{time, *color});
    return 1;
}

int colorSequenceNew(lua_State* L)
{
    std::vector<ColorSequenceKeypoint> points;
    if (const Color3* a = toColor3(L, 1)) {
        const Color3 b = lua_isnoneornil(L, 2) ? *a : checkColor3(L, 2);
        points = {{0.0f, *a}, {1.0f, b}};
    } else if (lua_istable(L, 1)) {
        points = readKeypoints<ColorSequenceKeypoint>(L, "ColorSequence");
    } else {
        raise(L, "ColorSequence.new(): table expected.");
    }
    validateKeypoints(L, points, "ColorSequence");
    push<ColorSequence>(L, ColorSequence(std::move(points)));
    return 1;
}

bool colorSequenceExtra(lua_State* L, const ColorSequence& s, std::string_view key)
{
    if (key != "Keypoints")
        return false;
    pushKeypointTable(L, s.keypoints);
    return true;
}

// Keypoints print like their sequence's entries. (Guess: formats copied from
// the sequence tostring; not checked against the client.)
int numberKeypointToString(lua_State* L)
{
    const auto& k = check<NumberSequenceKeypoint>(L, 1);
    pushString(L, std::format("{} {} {} ", k.time, k.value, k.envelope));
    return 1;
}

int colorKeypointToString(lua_State* L)
{
    const auto& k = check<ColorSequenceKeypoint>(L, 1);
    pushString(L, std::format("{} {} {} {} 0 ", k.time, k.value.r, k.value.g, k.value.b));
    return 1;
}

void openSequences(lua_State* L)
{
    registerUserdata<NumberSequenceKeypoint>(L, indexOf<NumberSequenceKeypoint>, {},
                                             std::to_array<luaL_Reg>({
                                                 {"__eq", eqOf<NumberSequenceKeypoint>},
                                                 {"__tostring", numberKeypointToString},
                                             }));
    setGlobalTable(L, "NumberSequenceKeypoint", std::to_array<luaL_Reg>({{"new", numberKeypointNew}}));

    registerUserdata<NumberSequence>(L, indexOf<NumberSequence, numberSequenceExtra, false>, {},
                                     std::to_array<luaL_Reg>({
                                         {"__eq", eqOf<NumberSequence>},
                                         {"__tostring", toStringOf<NumberSequence>},
                                     }));
    setGlobalTable(L, "NumberSequence", std::to_array<luaL_Reg>({{"new", numberSequenceNew}}));

    registerUserdata<ColorSequenceKeypoint>(L, indexOf<ColorSequenceKeypoint>, {},
                                            std::to_array<luaL_Reg>({
                                                {"__eq", eqOf<ColorSequenceKeypoint>},
                                                {"__tostring", colorKeypointToString},
                                            }));
    setGlobalTable(L, "ColorSequenceKeypoint", std::to_array<luaL_Reg>({{"new", colorKeypointNew}}));

    registerUserdata<ColorSequence>(L, indexOf<ColorSequence, colorSequenceExtra, false>, {},
                                    std::to_array<luaL_Reg>({
                                        {"__eq", eqOf<ColorSequence>},
                                        {"__tostring", toStringOf<ColorSequence>},
                                    }));
    setGlobalTable(L, "ColorSequence", std::to_array<luaL_Reg>({{"new", colorSequenceNew}}));
}

// --- BrickColor ------------------------------------------------------------------

bool brickColorExtra(lua_State* L, const BrickColor& b, std::string_view key)
{
    const Color3 c = b.color();
    if (key == "Name")
        pushString(L, b.name());
    else if (key == "Number")
        pushNumber(L, b.number);
    else if (key == "Color")
        pushColor3(L, c);
    else if (key == "r")
        pushNumber(L, c.r);
    else if (key == "g")
        pushNumber(L, c.g);
    else if (key == "b")
        pushNumber(L, c.b);
    else
        return false;
    return true;
}

int brickColorNew(lua_State* L)
{
    BrickColor result;
    if (lua_type(L, 1) == LUA_TNUMBER && lua_gettop(L) >= 3) {
        result = BrickColor::nearest(Color3{checkFloat(L, 1), checkFloat(L, 2), checkFloat(L, 3)});
    } else if (lua_type(L, 1) == LUA_TNUMBER) {
        result = BrickColor::fromNumber(static_cast<int>(lua_tonumber(L, 1)));
    } else if (lua_type(L, 1) == LUA_TSTRING) {
        result = BrickColor::fromName(lua_tostring(L, 1));
    } else if (const Color3* c = toColor3(L, 1)) {
        result = BrickColor::nearest(*c);
    } else if (!lua_isnoneornil(L, 1)) {
        luaL_typeerror(L, 1, "number");
    }
    push<BrickColor>(L, result);
    return 1;
}

// The 1-based palette of BrickColor.palette(index). (Guess: the client's own
// 128-entry palette order isn't recovered yet; this indexes the engine's table.)
int brickColorPalette(lua_State* L)
{
    const int index = luaL_checkinteger(L, 1);
    auto palette = BrickColor::palette();
    if (index < 0 || std::cmp_greater_equal(index, palette.size()) || index >= 128)
        raise(L, std::format("palette index out of bounds ({})", index));
    push<BrickColor>(L, BrickColor{palette[static_cast<std::size_t>(index)].number});
    return 1;
}

int brickColorRandom(lua_State* L)
{
    auto palette = BrickColor::palette();
    push<BrickColor>(L, BrickColor{palette[scriptEntropy() % palette.size()].number});
    return 1;
}

template <int Number>
int brickColorNamed(lua_State* L)
{
    push<BrickColor>(L, BrickColor::fromNumber(Number));
    return 1;
}

void openBrickColor(lua_State* L)
{
    registerUserdata<BrickColor>(L, indexOf<BrickColor, brickColorExtra, false>, {},
                                 std::to_array<luaL_Reg>({
                                     {"__eq", eqOf<BrickColor>},
                                     {"__tostring", toStringOf<BrickColor>},
                                 }));
    // The named constructors are the classic ones: White 1, Gray 194,
    // DarkGray 199, Black 26, Red 21, Yellow 24, Green 28, Blue 23.
    setGlobalTable(L, "BrickColor",
                   std::to_array<luaL_Reg>({
                       {"new", brickColorNew},
                       {"palette", brickColorPalette},
                       {"random", brickColorRandom},
                       {"White", brickColorNamed<1>},
                       {"Gray", brickColorNamed<194>},
                       {"DarkGray", brickColorNamed<199>},
                       {"Black", brickColorNamed<26>},
                       {"Red", brickColorNamed<21>},
                       {"Yellow", brickColorNamed<24>},
                       {"Green", brickColorNamed<28>},
                       {"Blue", brickColorNamed<23>},
                   }));
}

// --- Faces, Axes -----------------------------------------------------------------

int facesNew(lua_State* L)
{
    Faces faces;
    for (int i = 1; i <= lua_gettop(L); ++i) {
        auto item = toEnumItem(L, i, "NormalId", true);
        if (!item)
            raise(L, "Faces.new expects Enum.NormalId inputs");
        faces = Faces::fromBits(static_cast<std::uint8_t>(faces.bits() | (1u << item->value)));
    }
    push<Faces>(L, faces);
    return 1;
}

int axesNew(lua_State* L)
{
    Axes axes;
    for (int i = 1; i <= lua_gettop(L); ++i) {
        if (auto axis = toEnumItem(L, i, "Axis", true)) {
            axes = Axes::fromBits(static_cast<std::uint8_t>(axes.bits() | (1u << axis->value)));
        } else if (auto normal = toEnumItem(L, i, "NormalId", true)) {
            // Right/Left are X, Top/Bottom Y, Back/Front Z.
            axes = Axes::fromBits(static_cast<std::uint8_t>(axes.bits() | (1u << (normal->value % 3))));
        } else {
            raise(L, "Axes.new expects Enum.Axis or Enum.NormalId inputs");
        }
    }
    push<Axes>(L, axes);
    return 1;
}

bool axesExtra(lua_State* L, const Axes& a, std::string_view key)
{
    if (key == "Left" || key == "Right")
        lua_pushboolean(L, a.x);
    else if (key == "Top" || key == "Bottom")
        lua_pushboolean(L, a.y);
    else if (key == "Back" || key == "Front")
        lua_pushboolean(L, a.z);
    else
        return false;
    return true;
}

void openFacesAxes(lua_State* L)
{
    registerUserdata<Faces>(L, indexOf<Faces>, {},
                            std::to_array<luaL_Reg>({
                                {"__eq", eqOf<Faces>},
                                {"__tostring", toStringOf<Faces>},
                            }));
    setGlobalTable(L, "Faces", std::to_array<luaL_Reg>({{"new", facesNew}}));
    registerUserdata<Axes>(L, indexOf<Axes, axesExtra>, {},
                           std::to_array<luaL_Reg>({
                               {"__eq", eqOf<Axes>},
                               {"__tostring", toStringOf<Axes>},
                           }));
    setGlobalTable(L, "Axes", std::to_array<luaL_Reg>({{"new", axesNew}}));
}

// --- PhysicalProperties ------------------------------------------------------------

struct MaterialPhysics {
    std::string_view material;
    PhysicalProperties properties;
};

// Default physical properties per material: density, friction, elasticity,
// friction weight, elasticity weight. From Roblox's published material table;
// the densities all appear in the client's float constants, the weights are
// from the documentation. (Guess for the 2022 materials Cardboard..Rubber.)
constexpr MaterialPhysics materialPhysics[] = {
    {"Plastic", {0.7f, 0.3f, 0.5f, 1.0f, 1.0f}},
    {"SmoothPlastic", {0.7f, 0.2f, 0.5f, 1.0f, 1.0f}},
    {"Neon", {0.7f, 0.3f, 0.2f, 1.0f, 1.0f}},
    {"Wood", {0.35f, 0.48f, 0.2f, 1.0f, 1.0f}},
    {"WoodPlanks", {0.35f, 0.48f, 0.2f, 1.0f, 1.0f}},
    {"Marble", {2.563f, 0.2f, 0.17f, 1.0f, 1.0f}},
    {"Basalt", {2.691f, 0.7f, 0.15f, 0.3f, 1.0f}},
    {"Slate", {2.691f, 0.4f, 0.2f, 1.0f, 1.0f}},
    {"CrackedLava", {2.691f, 0.65f, 0.15f, 1.0f, 1.0f}},
    {"Concrete", {2.403f, 0.7f, 0.2f, 0.3f, 1.0f}},
    {"Limestone", {2.691f, 0.5f, 0.15f, 1.0f, 1.0f}},
    {"Granite", {2.691f, 0.4f, 0.2f, 1.0f, 1.0f}},
    {"Pavement", {2.691f, 0.5f, 0.17f, 0.3f, 1.0f}},
    {"Brick", {1.922f, 0.8f, 0.15f, 0.3f, 1.0f}},
    {"Pebble", {2.403f, 0.4f, 0.17f, 1.0f, 1.5f}},
    {"Cobblestone", {2.691f, 0.5f, 0.17f, 1.0f, 1.0f}},
    {"Rock", {2.691f, 0.5f, 0.17f, 1.0f, 1.0f}},
    {"Sandstone", {2.691f, 0.5f, 0.15f, 5.0f, 5.0f}},
    {"CorrodedMetal", {7.85f, 0.7f, 0.2f, 1.0f, 1.0f}},
    {"DiamondPlate", {7.85f, 0.35f, 0.25f, 1.0f, 1.0f}},
    {"Foil", {2.7f, 0.4f, 0.25f, 1.0f, 1.0f}},
    {"Metal", {7.85f, 0.4f, 0.25f, 1.0f, 1.0f}},
    {"Grass", {0.9f, 0.4f, 0.1f, 1.0f, 1.5f}},
    {"LeafyGrass", {0.9f, 0.4f, 0.1f, 2.0f, 2.0f}},
    {"Sand", {1.602f, 0.5f, 0.05f, 5.0f, 2.5f}},
    {"Fabric", {0.7f, 0.35f, 0.05f, 1.0f, 1.0f}},
    {"Snow", {0.9f, 0.3f, 0.03f, 4.0f, 3.0f}},
    {"Mud", {0.9f, 0.3f, 0.07f, 4.0f, 3.0f}},
    {"Ground", {0.9f, 0.45f, 0.1f, 1.0f, 1.0f}},
    {"Asphalt", {2.36f, 0.8f, 0.2f, 0.3f, 1.0f}},
    {"Salt", {2.165f, 0.5f, 0.05f, 1.0f, 1.0f}},
    {"Ice", {0.919f, 0.02f, 0.15f, 3.0f, 1.0f}},
    {"Glacier", {0.919f, 0.05f, 0.15f, 2.0f, 1.0f}},
    {"Glass", {2.4f, 0.25f, 0.2f, 1.0f, 1.0f}},
    {"ForceField", {2.4f, 0.25f, 0.2f, 1.0f, 1.0f}},
    {"Air", {0.01f, 0.01f, 0.01f, 1.0f, 1.0f}},
    {"Water", {1.0f, 0.01f, 0.01f, 1.0f, 1.0f}},
    {"Cardboard", {0.7f, 0.5f, 0.05f, 1.0f, 1.0f}},
    {"Carpet", {1.1f, 0.4f, 0.25f, 1.0f, 1.0f}},
    {"CeramicTiles", {2.4f, 0.51f, 0.2f, 1.0f, 1.0f}},
    {"ClayRoofTiles", {2.0f, 0.51f, 0.2f, 1.0f, 1.0f}},
    {"RoofShingles", {2.36f, 0.8f, 0.2f, 0.3f, 1.0f}},
    {"Leather", {0.86f, 0.35f, 0.25f, 1.0f, 1.0f}},
    {"Plaster", {0.75f, 0.6f, 0.2f, 1.0f, 1.0f}},
    {"Rubber", {1.3f, 1.5f, 0.95f, 3.0f, 2.0f}},
};

int physicalPropertiesNew(lua_State* L)
{
    const int n = lua_gettop(L);
    PhysicalProperties p;
    if (n == 1) {
        auto material = toEnumItem(L, 1, "Material", true);
        if (!material)
            raise(L, "PhysicalProperties.new with 1 argument expects Enum.Material inputs");
        auto it = std::ranges::find(materialPhysics, material->name, &MaterialPhysics::material);
        if (it != std::end(materialPhysics))
            p = it->properties;
    } else if (n == 3 || n == 5 || n == 6) {
        p.density = checkFloat(L, 1);
        p.friction = checkFloat(L, 2);
        p.elasticity = checkFloat(L, 3);
        if (n >= 5) {
            p.frictionWeight = checkFloat(L, 4);
            p.elasticityWeight = checkFloat(L, 5);
        }
        if (n == 6)
            p.acousticAbsorption = checkFloat(L, 6);
    } else {
        raise(L, std::vformat(concat("Invalid number of arguments: {}. Four ways to call ",
                                     "PhysicalProperties.new:\n\t\t\t\t\t1. new(material: Material)  -- ",
                                     "returns default properties for the given material\n\t\t\t\t\t2. ",
                                     "new(density: number, friction: number, elasticity: ",
                                     "number)\n\t\t\t\t\t3. new(density: number, friction: number, ",
                                     "elasticity: number, frictionWeight: number, elasticityWeight: ",
                                     "number)\n\t\t\t\t\t4. new(density: number, friction: number, ",
                                     "elasticity: number, frictionWeight: number, elasticityWeight: number, ",
                                     "acousticAbsorption: number)\n"),
                              std::make_format_args(n)));
    }
    push<PhysicalProperties>(L, p);
    return 1;
}

void openPhysicalProperties(lua_State* L)
{
    registerUserdata<PhysicalProperties>(L, indexOf<PhysicalProperties>, {},
                                         std::to_array<luaL_Reg>({
                                             {"__eq", eqOf<PhysicalProperties>},
                                             {"__tostring", toStringOf<PhysicalProperties>},
                                         }));
    setGlobalTable(L, "PhysicalProperties", std::to_array<luaL_Reg>({{"new", physicalPropertiesNew}}));
}

// --- Vector3int16 ----------------------------------------------------------------

std::int16_t toInt16(double value)
{
    // Wraps like the client's integer conversion. (Guess: wrap vs clamp.)
    return static_cast<std::int16_t>(static_cast<std::int64_t>(value));
}

int vector3int16New(lua_State* L)
{
    push<Vector3int16>(L, Vector3int16{toInt16(luaL_optnumber(L, 1, 0)), toInt16(luaL_optnumber(L, 2, 0)),
                                       toInt16(luaL_optnumber(L, 3, 0))});
    return 1;
}

template <char Op>
int vector3int16Arith(lua_State* L)
{
    const Vector3int16* a = to<Vector3int16>(L, 1);
    const Vector3int16* b = to<Vector3int16>(L, 2);
    auto component = [&](int idx, int axis) -> std::optional<int> {
        if (const Vector3int16* v = to<Vector3int16>(L, idx))
            return axis == 0 ? v->x : axis == 1 ? v->y : v->z;
        if (lua_type(L, idx) == LUA_TNUMBER && (Op == '*' || Op == '/'))
            return static_cast<int>(lua_tonumber(L, idx));
        return std::nullopt;
    };
    if (!a && !b)
        luaL_typeerror(L, 1, "Vector3int16");
    std::array<std::int16_t, 3> r{};
    for (int axis = 0; axis < 3; ++axis) {
        auto x = component(1, axis);
        auto y = component(2, axis);
        if (!x || !y)
            raise(L,
                  std::format("attempt to perform arithmetic on {} and {}", luaL_typename(L, 1), luaL_typename(L, 2)));
        int value = 0;
        if constexpr (Op == '+')
            value = *x + *y;
        else if constexpr (Op == '-')
            value = *x - *y;
        else if constexpr (Op == '*')
            value = *x * *y;
        else {
            if (*y == 0)
                raise(L, "Divide by zero exception");
            value = *x / *y;
        }
        r[static_cast<std::size_t>(axis)] = static_cast<std::int16_t>(value);
    }
    push<Vector3int16>(L, Vector3int16{r[0], r[1], r[2]});
    return 1;
}

int vector3int16Unm(lua_State* L)
{
    const Vector3int16& v = check<Vector3int16>(L, 1);
    push<Vector3int16>(L, Vector3int16{static_cast<std::int16_t>(-v.x), static_cast<std::int16_t>(-v.y),
                                       static_cast<std::int16_t>(-v.z)});
    return 1;
}

void openVector3int16(lua_State* L)
{
    registerUserdata<Vector3int16>(L, indexOf<Vector3int16>, {},
                                   std::to_array<luaL_Reg>({
                                       {"__add", vector3int16Arith<'+'>},
                                       {"__sub", vector3int16Arith<'-'>},
                                       {"__mul", vector3int16Arith<'*'>},
                                       {"__div", vector3int16Arith<'/'>},
                                       {"__unm", vector3int16Unm},
                                       {"__eq", eqOf<Vector3int16>},
                                       {"__tostring", toStringOf<Vector3int16>},
                                   }));
    setGlobalTable(L, "Vector3int16", std::to_array<luaL_Reg>({{"new", vector3int16New}}));
}

// --- Font --------------------------------------------------------------------------

struct EnumFont {
    std::string_view enumName;
    std::string_view family;
    Enum::FontWeight weight = Enum::FontWeight::Regular;
    Enum::FontStyle style = Enum::FontStyle::Normal;
};

// Font.fromEnum's table. The family files are the client's own list of
// "rbxasset://fonts/families/<name>.json" strings, which it keeps in Enum.Font
// order; the weights follow the enum names.
constexpr EnumFont enumFonts[] = {
    {"Legacy", "LegacyArial"},
    {"Arial", "Arial"},
    {"ArialBold", "Arial", Enum::FontWeight::Bold},
    {"SourceSans", "SourceSansPro"},
    {"SourceSansBold", "SourceSansPro", Enum::FontWeight::Bold},
    {"SourceSansLight", "SourceSansPro", Enum::FontWeight::Light},
    {"SourceSansItalic", "SourceSansPro", Enum::FontWeight::Regular, Enum::FontStyle::Italic},
    {"Bodoni", "AccanthisADFStd"},
    {"Garamond", "Guru"},
    {"Cartoon", "ComicNeueAngular"},
    {"Code", "Inconsolata"},
    {"Highway", "HighwayGothic"},
    {"SciFi", "Zekton"},
    {"Arcade", "PressStart2P"},
    {"Fantasy", "Balthazar"},
    {"Antique", "RomanAntique"},
    {"SourceSansSemibold", "SourceSansPro", Enum::FontWeight::SemiBold},
    {"Gotham", "GothamSSm"},
    {"GothamMedium", "GothamSSm", Enum::FontWeight::Medium},
    {"GothamBold", "GothamSSm", Enum::FontWeight::Bold},
    {"GothamBlack", "GothamSSm", Enum::FontWeight::Heavy},
    {"AmaticSC", "AmaticSC"},
    {"Bangers", "Bangers"},
    {"Creepster", "Creepster"},
    {"DenkOne", "DenkOne"},
    {"Fondamento", "Fondamento"},
    {"FredokaOne", "FredokaOne"},
    {"GrenzeGotisch", "GrenzeGotisch"},
    {"IndieFlower", "IndieFlower"},
    {"JosefinSans", "JosefinSans"},
    {"Jura", "Jura"},
    {"Kalam", "Kalam"},
    {"LuckiestGuy", "LuckiestGuy"},
    {"Merriweather", "Merriweather"},
    {"Michroma", "Michroma"},
    {"Nunito", "Nunito"},
    {"Oswald", "Oswald"},
    {"PatrickHand", "PatrickHand"},
    {"PermanentMarker", "PermanentMarker"},
    {"Roboto", "Roboto"},
    {"RobotoCondensed", "RobotoCondensed"},
    {"RobotoMono", "RobotoMono"},
    {"Sarpanch", "Sarpanch"},
    {"SpecialElite", "SpecialElite"},
    {"TitilliumWeb", "TitilliumWeb"},
    {"Ubuntu", "Ubuntu"},
    {"BuilderSans", "BuilderSans"},
    {"BuilderSansMedium", "BuilderSans", Enum::FontWeight::Medium},
    {"BuilderSansBold", "BuilderSans", Enum::FontWeight::Bold},
    {"BuilderSansExtraBold", "BuilderSans", Enum::FontWeight::ExtraBold},
    {"Arimo", "Arimo"},
    {"ArimoBold", "Arimo", Enum::FontWeight::Bold},
};

Enum::FontWeight optWeight(lua_State* L, int idx)
{
    if (lua_isnoneornil(L, idx))
        return Enum::FontWeight::Regular;
    auto item = toEnumItem(L, idx, "FontWeight");
    if (!item)
        luaL_typeerror(L, idx, "Enum.FontWeight");
    return static_cast<Enum::FontWeight>(item->value);
}

Enum::FontStyle optStyle(lua_State* L, int idx)
{
    if (lua_isnoneornil(L, idx))
        return Enum::FontStyle::Normal;
    auto item = toEnumItem(L, idx, "FontStyle");
    if (!item)
        luaL_typeerror(L, idx, "Enum.FontStyle");
    return static_cast<Enum::FontStyle>(item->value);
}

int fontNew(lua_State* L)
{
    push<Font>(L, Font{std::string(checkString(L, 1)), optWeight(L, 2), optStyle(L, 3)});
    return 1;
}

int fontFromName(lua_State* L)
{
    push<Font>(
        L, Font{std::format("rbxasset://fonts/families/{}.json", checkString(L, 1)), optWeight(L, 2), optStyle(L, 3)});
    return 1;
}

int fontFromId(lua_State* L)
{
    push<Font>(L, Font{std::format("rbxassetid://{}", static_cast<std::int64_t>(luaL_checknumber(L, 1))),
                       optWeight(L, 2), optStyle(L, 3)});
    return 1;
}

int fontFromEnum(lua_State* L)
{
    auto item = toEnumItem(L, 1, "Font");
    if (!item)
        luaL_typeerror(L, 1, "Enum.Font");
    if (item->name == "Unknown")
        raise(L, "Enum.Font.Unknown can't be passed to Font.fromEnum");
    auto it = std::ranges::find(enumFonts, item->name, &EnumFont::enumName);
    if (it == std::end(enumFonts)) {
        push<Font>(L, Font{});
    } else {
        push<Font>(L, Font{std::format("rbxasset://fonts/families/{}.json", it->family), it->weight, it->style});
    }
    return 1;
}

bool fontExtra(lua_State* L, const Font& f, std::string_view key)
{
    if (key != "Bold")
        return false;
    lua_pushboolean(L, f.weight >= Enum::FontWeight::SemiBold);
    return true;
}

// Font is the one mutable datatype: scripts assign its Family, Weight, Style
// and Bold (true is Bold weight, false Regular), as the game's
// label segments do to a Font they keep.
int fontNewIndex(lua_State* L)
{
    auto& font = const_cast<Font&>(check<Font>(L, 1));
    const std::string_view key = checkString(L, 2);
    if (key == "Family")
        font.family = std::string(checkString(L, 3));
    else if (key == "Weight")
        font.weight = optWeight(L, 3);
    else if (key == "Style")
        font.style = optStyle(L, 3);
    else if (key == "Bold") {
        luaL_checktype(L, 3, LUA_TBOOLEAN);
        font.weight = lua_toboolean(L, 3) ? Enum::FontWeight::Bold : Enum::FontWeight::Regular;
    } else
        raise(L, std::format("{} is not a valid member of Font", key));
    return 0;
}

void openFont(lua_State* L)
{
    registerUserdata<Font>(L, indexOf<Font, fontExtra>, {},
                           std::to_array<luaL_Reg>({
                               {"__newindex", fontNewIndex},
                               {"__eq", eqOf<Font>},
                               {"__tostring", toStringOf<Font>},
                           }));
    setGlobalTable(L, "Font",
                   std::to_array<luaL_Reg>({
                       {"new", fontNew},
                       {"fromName", fontFromName},
                       {"fromId", fontFromId},
                       {"fromEnum", fontFromEnum},
                   }));
}

} // namespace

void pushNumber(lua_State* L, double value)
{
    lua_pushnumber(L, value);
}

void pushString(lua_State* L, std::string_view text)
{
    lua_pushlstring(L, text.data(), text.size());
}

void setGlobalTable(lua_State* L, const char* name, std::span<const luaL_Reg> functions)
{
    lua_newtable(L);
    for (const luaL_Reg& f : functions) {
        lua_pushcfunction(L, f.func, f.name);
        lua_setfield(L, -2, f.name);
    }
    lua_setreadonly(L, -1, true);
    lua_setglobal(L, name);
}

void openValueTypes(lua_State* L)
{
    openVector2(L);
    openUDim(L);
    openUDim2(L);
    openRect(L);
    openNumberRange(L);
    openSequences(L);
    openBrickColor(L);
    openFacesAxes(L);
    openPhysicalProperties(L);
    openVector3int16(L);
    openFont(L);
}

} // namespace lunar::lua

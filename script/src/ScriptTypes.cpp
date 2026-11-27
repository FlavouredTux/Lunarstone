// Datatypes that only exist on the script side: TweenInfo, Ray, Region3,
// Region3int16, DateTime, Random, RaycastParams, OverlapParams,
// RaycastResult, Path2DControlPoint, Vector2int16, PathWaypoint,
// RotationCurveKey, Content and PluginDrag. Member names and messages are the 0.742 client's ("TweenInfo.new
// second argument expects Enum.EasingStyle input", "Random.new requires 0 or 1
// argument", "RaycastResult{", "; normal = ", "; material = ", ...).

#include "Userdata.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <format>
#include <limits>
#include <numbers>
#include <optional>
#include <string>

namespace lunar::lua {

namespace {

float checkFloat(lua_State* L, int idx)
{
    return static_cast<float>(luaL_checknumber(L, idx));
}

std::string formatVector(Vector3 v)
{
    return std::format("{}, {}, {}", v.x, v.y, v.z);
}

// --- TweenInfo -------------------------------------------------------------------

int tweenInfoNew(lua_State* L)
{
    TweenInfo info;
    info.easingStyle = enumItem("EasingStyle", "Quad");
    info.easingDirection = enumItem("EasingDirection", "Out");
    if (!lua_isnoneornil(L, 1)) {
        if (lua_type(L, 1) != LUA_TNUMBER)
            raise(L, "TweenInfo.new first argument expects a number for time.");
        info.time = lua_tonumber(L, 1);
    }
    if (!lua_isnoneornil(L, 2)) {
        // Accept a numeric easing style as well as an EnumItem.
        auto style = toEnumItem(L, 2, "EasingStyle", lua_type(L, 2) != LUA_TNUMBER);
        if (!style)
            raise(L, "TweenInfo.new second argument expects Enum.EasingStyle input");
        info.easingStyle = *style;
    }
    if (!lua_isnoneornil(L, 3)) {
        auto direction = toEnumItem(L, 3, "EasingDirection", lua_type(L, 3) != LUA_TNUMBER);
        if (!direction)
            raise(L, "TweenInfo.new third argument expects Enum.EasingDirection input");
        info.easingDirection = *direction;
    }
    if (!lua_isnoneornil(L, 4)) {
        if (lua_type(L, 4) != LUA_TNUMBER)
            raise(L, "TweenInfo.new fourth arg should be a number for RepeatCount.");
        info.repeatCount = lua_tonumber(L, 4);
    }
    if (!lua_isnoneornil(L, 5)) {
        if (lua_type(L, 5) != LUA_TBOOLEAN)
            raise(L, "TweenInfo.new fifth arg should be a boolean for Reverses.");
        info.reverses = lua_toboolean(L, 5) != 0;
    }
    if (!lua_isnoneornil(L, 6)) {
        if (lua_type(L, 6) != LUA_TNUMBER)
            raise(L, "TweenInfo.new sixth arg should be a number for DelayTime.");
        info.delayTime = lua_tonumber(L, 6);
    }
    push<TweenInfo>(L, info);
    return 1;
}

// The field labels are the client's ("Time:", " DelayTime:", " RepeatCount:",
// " Reverses:", " EasingDirection:", " EasingStyle:"). (Guess: how the values
// themselves are printed, e.g. "False" for the boolean.)
int tweenInfoToString(lua_State* L)
{
    const TweenInfo& t = check<TweenInfo>(L, 1);
    pushString(L, std::format("Time:{} DelayTime:{} RepeatCount:{} Reverses:{} EasingDirection:{} EasingStyle:{}",
                              t.time, t.delayTime, t.repeatCount, t.reverses ? "True" : "False", t.easingDirection.name,
                              t.easingStyle.name));
    return 1;
}

void openTweenInfo(lua_State* L)
{
    registerUserdata<TweenInfo>(L, indexOf<TweenInfo>, {},
                                std::to_array<luaL_Reg>({
                                    {"__eq", eqOf<TweenInfo>},
                                    {"__tostring", tweenInfoToString},
                                }));
    setGlobalTable(L, "TweenInfo", std::to_array<luaL_Reg>({{"new", tweenInfoNew}}));
}

// --- Ray -----------------------------------------------------------------------------

Vector3 closestPoint(const Ray& ray, Vector3 point)
{
    const Vector3 unit = ray.direction.unit();
    const float t = std::max(0.0f, (point - ray.origin).dot(unit));
    return ray.origin + unit * t;
}

bool rayExtra(lua_State* L, const Ray& ray, std::string_view key)
{
    if (key != "Unit" && key != "unit")
        return false;
    push<Ray>(L, Ray{ray.origin, ray.direction.unit()});
    return true;
}

int rayNew(lua_State* L)
{
    const Vector3 origin = lua_isnoneornil(L, 1) ? Vector3{} : checkVector3(L, 1);
    const Vector3 direction = lua_isnoneornil(L, 2) ? Vector3{} : checkVector3(L, 2);
    push<Ray>(L, Ray{origin, direction});
    return 1;
}

int rayClosestPoint(lua_State* L)
{
    pushVector3(L, closestPoint(check<Ray>(L, 1), checkVector3(L, 2)));
    return 1;
}

int rayDistance(lua_State* L)
{
    const Vector3 point = checkVector3(L, 2);
    pushNumber(L, (point - closestPoint(check<Ray>(L, 1), point)).magnitude());
    return 1;
}

int rayToString(lua_State* L)
{
    const Ray& ray = check<Ray>(L, 1);
    pushString(L, std::format("{{{}}}, {{{}}}", formatVector(ray.origin), formatVector(ray.direction)));
    return 1;
}

void openRay(lua_State* L)
{
    registerUserdata<Ray>(L, indexOf<Ray, rayExtra>,
                          std::to_array<luaL_Reg>({
                              {"ClosestPoint", rayClosestPoint},
                              {"Distance", rayDistance},
                          }),
                          std::to_array<luaL_Reg>({
                              {"__eq", eqOf<Ray>},
                              {"__tostring", rayToString},
                          }));
    setGlobalTable(L, "Ray", std::to_array<luaL_Reg>({{"new", rayNew}}));
}

// --- Region3, Region3int16 ---------------------------------------------------------------

bool region3Extra(lua_State* L, const Region3& r, std::string_view key)
{
    if (key == "CFrame")
        pushCFrame(L, CFrame((r.min + r.max) * 0.5f));
    else if (key == "Size")
        pushVector3(L, r.max - r.min);
    else
        return false;
    return true;
}

int region3New(lua_State* L)
{
    const Vector3 min = lua_isnoneornil(L, 1) ? Vector3{} : checkVector3(L, 1);
    const Vector3 max = lua_isnoneornil(L, 2) ? Vector3{} : checkVector3(L, 2);
    push<Region3>(L, Region3{min, max});
    return 1;
}

int region3ExpandToGrid(lua_State* L)
{
    const Region3& r = check<Region3>(L, 1);
    const float resolution = checkFloat(L, 2);
    if (!(resolution > 0.0f))
        raise(L, "Resolution has to be a positive number");
    auto snap = [resolution](Vector3 v, float (*round)(float)) {
        return Vector3{round(v.x / resolution) * resolution, round(v.y / resolution) * resolution,
                       round(v.z / resolution) * resolution};
    };
    push<Region3>(L, Region3{snap(r.min, std::floor), snap(r.max, std::ceil)});
    return 1;
}

// (Guess: the client prints its CFrame and Size; separator not confirmed.)
int region3ToString(lua_State* L)
{
    const Region3& r = check<Region3>(L, 1);
    const Vector3 center = (r.min + r.max) * 0.5f;
    pushString(L, std::format("{}, 1, 0, 0, 0, 1, 0, 0, 0, 1; {}", formatVector(center), formatVector(r.max - r.min)));
    return 1;
}

int region3int16New(lua_State* L)
{
    const Vector3int16 zero{};
    push<Region3int16>(L, Region3int16{opt<Vector3int16>(L, 1, zero), opt<Vector3int16>(L, 2, zero)});
    return 1;
}

int region3int16ToString(lua_State* L)
{
    const Region3int16& r = check<Region3int16>(L, 1);
    pushString(L, std::format("{}, {}, {}; {}, {}, {}", r.min.x, r.min.y, r.min.z, r.max.x, r.max.y, r.max.z));
    return 1;
}

void openRegions(lua_State* L)
{
    registerUserdata<Region3>(L, indexOf<Region3, region3Extra, false>,
                              std::to_array<luaL_Reg>({{"ExpandToGrid", region3ExpandToGrid}}),
                              std::to_array<luaL_Reg>({
                                  {"__eq", eqOf<Region3>},
                                  {"__tostring", region3ToString},
                              }));
    setGlobalTable(L, "Region3", std::to_array<luaL_Reg>({{"new", region3New}}));
    registerUserdata<Region3int16>(L, indexOf<Region3int16>, {},
                                   std::to_array<luaL_Reg>({
                                       {"__eq", eqOf<Region3int16>},
                                       {"__tostring", region3int16ToString},
                                   }));
    setGlobalTable(L, "Region3int16", std::to_array<luaL_Reg>({{"new", region3int16New}}));
}

// --- DateTime ------------------------------------------------------------------------

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's algorithm).
std::int64_t daysFromCivil(std::int64_t y, std::int64_t m, std::int64_t d)
{
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct Civil {
    std::int64_t year, month, day, hour, minute, second, millisecond;
};

Civil civilFromMillis(std::int64_t ms)
{
    std::int64_t days = ms >= 0 ? ms / 86400000 : (ms - 86399999) / 86400000;
    std::int64_t rest = ms - days * 86400000;
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp + (mp < 10 ? 3 : -9);
    const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    return {y, m, d, rest / 3600000, rest / 60000 % 60, rest / 1000 % 60, rest % 1000};
}

std::int64_t millisFromCivil(const Civil& c)
{
    // Out-of-range fields roll over into the next larger one.
    std::int64_t month0 = c.month - 1;
    std::int64_t year = c.year + (month0 >= 0 ? month0 / 12 : (month0 - 11) / 12);
    month0 -= (year - c.year) * 12;
    const std::int64_t days = daysFromCivil(year, month0 + 1, 1) + c.day - 1;
    return ((days * 24 + c.hour) * 60 + c.minute) * 60000 + c.second * 1000 + c.millisecond;
}

// The local time zone's offset from UTC at a moment, in milliseconds.
std::int64_t localOffsetMillis(std::int64_t utcMillis)
{
    const auto seconds = static_cast<std::time_t>(utcMillis / 1000);
    std::tm local{};
    std::tm utc{};
    localtime_r(&seconds, &local);
    gmtime_r(&seconds, &utc);
    const Civil l{local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, 0};
    const Civil u{utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, 0};
    return millisFromCivil(l) - millisFromCivil(u);
}

int dateTimeNow(lua_State* L)
{
    push<DateTime>(L, DateTime{static_cast<std::int64_t>(std::floor(scriptWallClock(L) * 1000.0))});
    return 1;
}

int dateTimeFromUnixTimestamp(lua_State* L)
{
    push<DateTime>(L, DateTime{static_cast<std::int64_t>(std::floor(luaL_checknumber(L, 1) * 1000.0))});
    return 1;
}

int dateTimeFromUnixTimestampMillis(lua_State* L)
{
    push<DateTime>(L, DateTime{static_cast<std::int64_t>(luaL_checknumber(L, 1))});
    return 1;
}

Civil readCivil(lua_State* L)
{
    auto field = [L](int idx, std::int64_t fallback) {
        return static_cast<std::int64_t>(luaL_optnumber(L, idx, static_cast<double>(fallback)));
    };
    return {field(1, 1970), field(2, 1), field(3, 1), field(4, 0), field(5, 0), field(6, 0), field(7, 0)};
}

int dateTimeFromUniversalTime(lua_State* L)
{
    push<DateTime>(L, DateTime{millisFromCivil(readCivil(L))});
    return 1;
}

int dateTimeFromLocalTime(lua_State* L)
{
    const std::int64_t asUtc = millisFromCivil(readCivil(L));
    push<DateTime>(L, DateTime{asUtc - localOffsetMillis(asUtc)});
    return 1;
}

// ISO 8601: YYYY-MM-DDTHH:MM:SS[.fff](Z|+HH:MM|-HH:MM). Nil when it doesn't parse.
int dateTimeFromIsoDate(lua_State* L)
{
    std::string_view text = checkString(L, 1);
    // Fixed-width digits: "YYYY-MM-DDTHH:MM:SS" is 19 characters.
    auto digits = [&](std::size_t at, std::size_t count) -> int {
        int value = 0;
        for (std::size_t i = at; i < at + count; ++i) {
            if (i >= text.size() || text[i] < '0' || text[i] > '9')
                return -1;
            value = value * 10 + (text[i] - '0');
        }
        return value;
    };
    const int y = digits(0, 4), mo = digits(5, 2), d = digits(8, 2), h = digits(11, 2), mi = digits(14, 2),
              s = digits(17, 2);
    if (text.size() < 19 || text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != ' ') || text[13] != ':'
        || text[16] != ':' || y < 0 || mo < 0 || d < 0 || h < 0 || mi < 0 || s < 0) {
        lua_pushnil(L);
        return 1;
    }
    std::string_view rest = text.substr(19);
    std::int64_t ms = 0;
    if (!rest.empty() && rest[0] == '.') {
        std::size_t i = 1;
        std::int64_t scale = 100;
        while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') {
            ms += (rest[i] - '0') * scale;
            scale /= 10;
            ++i;
        }
        rest.remove_prefix(i);
    }
    std::int64_t offset = 0;
    if (rest == "Z" || rest.empty()) {
        offset = 0;
    } else if ((rest[0] == '+' || rest[0] == '-') && rest.size() >= 6) {
        const std::int64_t oh = (rest[1] - '0') * 10 + (rest[2] - '0');
        const std::int64_t om = (rest[4] - '0') * 10 + (rest[5] - '0');
        offset = (rest[0] == '-' ? -1 : 1) * (oh * 60 + om) * 60000;
    } else {
        lua_pushnil(L);
        return 1;
    }
    push<DateTime>(L, DateTime{millisFromCivil(Civil{y, mo, d, h, mi, s, ms}) - offset});
    return 1;
}

void pushCivilTable(lua_State* L, const Civil& c)
{
    lua_createtable(L, 0, 7);
    const std::pair<const char*, std::int64_t> fields[] = {
        {"Year", c.year},
        {"Month", c.month},
        {"Day", c.day},
        {"Hour", c.hour},
        {"Minute", c.minute},
        {"Second", c.second},
        {"Millisecond", c.millisecond},
    };
    for (const auto& [name, value] : fields) {
        pushNumber(L, static_cast<double>(value));
        lua_setfield(L, -2, name);
    }
}

int dateTimeToUniversalTime(lua_State* L)
{
    pushCivilTable(L, civilFromMillis(check<DateTime>(L, 1).unixTimestampMillis));
    return 1;
}

int dateTimeToLocalTime(lua_State* L)
{
    const std::int64_t ms = check<DateTime>(L, 1).unixTimestampMillis;
    pushCivilTable(L, civilFromMillis(ms + localOffsetMillis(ms)));
    return 1;
}

// (Guess: whole seconds without a fraction, the documented example's form.)
std::string isoDate(std::int64_t ms)
{
    const Civil c = civilFromMillis(ms);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", c.year, c.month, c.day, c.hour, c.minute, c.second);
}

int dateTimeToIsoDate(lua_State* L)
{
    pushString(L, isoDate(check<DateTime>(L, 1).unixTimestampMillis));
    return 1;
}

constexpr std::string_view monthNames[] = {"January", "February", "March",     "April",   "May",      "June",
                                           "July",    "August",   "September", "October", "November", "December"};
constexpr std::string_view dayNames[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// The moment.js-style tokens FormatUniversalTime/FormatLocalTime document, for
// the en-us locale. Text in [brackets] is copied as is.
std::string formatDate(std::int64_t ms, std::string_view format)
{
    const Civil c = civilFromMillis(ms);
    const std::int64_t days = ms >= 0 ? ms / 86400000 : (ms - 86399999) / 86400000;
    const auto weekday = static_cast<std::size_t>(((days % 7) + 7 + 4) % 7); // 1970-01-01 was a Thursday
    const std::string_view month = monthNames[static_cast<std::size_t>(c.month - 1)];
    const std::int64_t hour12 = c.hour % 12 == 0 ? 12 : c.hour % 12;
    static constexpr std::pair<std::string_view, int> tokens[] = {
        {"YYYY", 0}, {"MMMM", 1}, {"dddd", 2}, {"MMM", 3}, {"ddd", 4}, {"SSS", 5}, {"YY", 6}, {"MM", 7},
        {"DD", 8},   {"HH", 9},   {"hh", 10},  {"mm", 11}, {"ss", 12}, {"Do", 13}, {"M", 14}, {"D", 15},
        {"H", 16},   {"h", 17},   {"m", 18},   {"s", 19},  {"A", 20},  {"a", 21},
    };
    std::string out;
    std::size_t i = 0;
    while (i < format.size()) {
        if (format[i] == '[') {
            const std::size_t close = format.find(']', i);
            if (close != std::string_view::npos) {
                out += format.substr(i + 1, close - i - 1);
                i = close + 1;
                continue;
            }
        }
        bool matched = false;
        for (const auto& [token, id] : tokens) {
            if (!format.substr(i).starts_with(token))
                continue;
            switch (id) {
            case 0:
                out += std::format("{:04}", c.year);
                break;
            case 1:
                out += month;
                break;
            case 2:
                out += dayNames[weekday];
                break;
            case 3:
                out += month.substr(0, 3);
                break;
            case 4:
                out += dayNames[weekday].substr(0, 3);
                break;
            case 5:
                out += std::format("{:03}", c.millisecond);
                break;
            case 6:
                out += std::format("{:02}", c.year % 100);
                break;
            case 7:
                out += std::format("{:02}", c.month);
                break;
            case 8:
                out += std::format("{:02}", c.day);
                break;
            case 9:
                out += std::format("{:02}", c.hour);
                break;
            case 10:
                out += std::format("{:02}", hour12);
                break;
            case 11:
                out += std::format("{:02}", c.minute);
                break;
            case 12:
                out += std::format("{:02}", c.second);
                break;
            case 13: {
                const std::int64_t d = c.day;
                const char* suffix = (d % 100 >= 11 && d % 100 <= 13) ? "th"
                                     : d % 10 == 1                    ? "st"
                                     : d % 10 == 2                    ? "nd"
                                     : d % 10 == 3                    ? "rd"
                                                                      : "th";
                out += std::format("{}{}", d, suffix);
                break;
            }
            case 14:
                out += std::to_string(c.month);
                break;
            case 15:
                out += std::to_string(c.day);
                break;
            case 16:
                out += std::to_string(c.hour);
                break;
            case 17:
                out += std::to_string(hour12);
                break;
            case 18:
                out += std::to_string(c.minute);
                break;
            case 19:
                out += std::to_string(c.second);
                break;
            case 20:
                out += c.hour < 12 ? "AM" : "PM";
                break;
            case 21:
                out += c.hour < 12 ? "am" : "pm";
                break;
            default:
                break;
            }
            i += token.size();
            matched = true;
            break;
        }
        if (!matched)
            out += format[i++];
    }
    return out;
}

int dateTimeFormatUniversalTime(lua_State* L)
{
    pushString(L, formatDate(check<DateTime>(L, 1).unixTimestampMillis, checkString(L, 2)));
    return 1;
}

int dateTimeFormatLocalTime(lua_State* L)
{
    const std::int64_t ms = check<DateTime>(L, 1).unixTimestampMillis;
    pushString(L, formatDate(ms + localOffsetMillis(ms), checkString(L, 2)));
    return 1;
}

bool dateTimeExtra(lua_State* L, const DateTime& t, std::string_view key)
{
    if (key != "UnixTimestamp")
        return false;
    pushNumber(L, static_cast<double>(t.unixTimestampMillis >= 0 ? t.unixTimestampMillis / 1000
                                                                 : (t.unixTimestampMillis - 999) / 1000));
    return true;
}

// (Guess: tostring gives the ISO form.)
int dateTimeToString(lua_State* L)
{
    pushString(L, isoDate(check<DateTime>(L, 1).unixTimestampMillis));
    return 1;
}

template <bool Less, bool OrEqual>
int dateTimeCompare(lua_State* L)
{
    const std::int64_t a = check<DateTime>(L, 1).unixTimestampMillis;
    const std::int64_t b = check<DateTime>(L, 2).unixTimestampMillis;
    lua_pushboolean(L, OrEqual ? a <= b : a < b);
    return 1;
}

void openDateTime(lua_State* L)
{
    registerUserdata<DateTime>(L, indexOf<DateTime, dateTimeExtra>,
                               std::to_array<luaL_Reg>({
                                   {"ToUniversalTime", dateTimeToUniversalTime},
                                   {"ToLocalTime", dateTimeToLocalTime},
                                   {"ToIsoDate", dateTimeToIsoDate},
                                   {"FormatUniversalTime", dateTimeFormatUniversalTime},
                                   {"FormatLocalTime", dateTimeFormatLocalTime},
                               }),
                               std::to_array<luaL_Reg>({
                                   {"__eq", eqOf<DateTime>},
                                   {"__lt", dateTimeCompare<true, false>},
                                   {"__le", dateTimeCompare<true, true>},
                                   {"__tostring", dateTimeToString},
                               }));
    setGlobalTable(L, "DateTime",
                   std::to_array<luaL_Reg>({
                       {"now", dateTimeNow},
                       {"fromUnixTimestamp", dateTimeFromUnixTimestamp},
                       {"fromUnixTimestampMillis", dateTimeFromUnixTimestampMillis},
                       {"fromUniversalTime", dateTimeFromUniversalTime},
                       {"fromLocalTime", dateTimeFromLocalTime},
                       {"fromIsoDate", dateTimeFromIsoDate},
                   }));
}

// --- Random ------------------------------------------------------------------------------

// PCG32 exactly as Luau's math.random runs it (lmathlib.cpp: multiplier
// 6364136223846793005, increment 105). (Guess: that Random shares it; the
// generator family is Roblox's documented choice, the constants aren't verified.)
std::uint32_t pcg32(RandomState& rng)
{
    const std::uint64_t old = rng.state;
    rng.state = old * 6364136223846793005ULL + (105 | 1);
    const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
    const auto rot = static_cast<std::uint32_t>(old >> 59u);
    return std::rotr(xorshifted, static_cast<int>(rot));
}

RandomState seeded(std::uint64_t seed)
{
    RandomState rng;
    pcg32(rng);
    rng.state += seed;
    pcg32(rng);
    return rng;
}

double nextUnit(RandomState& rng)
{
    const std::uint32_t lo = pcg32(rng);
    const std::uint32_t hi = pcg32(rng);
    return std::ldexp(static_cast<double>(lo | (static_cast<std::uint64_t>(hi) << 32)), -64);
}

std::int64_t nextInteger(RandomState& rng, std::int64_t min, std::int64_t max)
{
    const auto range = static_cast<std::uint64_t>(max - min) + 1;
    if (range <= 0xFFFFFFFFull) {
        const std::uint64_t x = range * pcg32(rng);
        return min + static_cast<std::int64_t>(x >> 32);
    }
    return min + static_cast<std::int64_t>(nextUnit(rng) * static_cast<double>(range));
}

// The client counts at most the arguments it uses and ignores the rest
// (Random.new: min(top, 1) at 0x10147773b; NextNumber: min(top, 3) at
// 0x1014779f6), so extra arguments never raise.
int randomNew(lua_State* L)
{
    const int n = std::min(lua_gettop(L), 1);
    std::uint64_t seed = 0;
    if (n == 1 && !lua_isnil(L, 1)) {
        const double value = luaL_checknumber(L, 1);
        constexpr double low = -9007199254740992.0, high = 9007199254740992.0;
        if (value < low || value > high || std::isnan(value)) {
            Runtime::of(L).output(
                MessageType::Warning,
                std::vformat(concat("Seed '{}', passed to Random.new(), clamped to 0. For consistent ",
                                    "cross-platform behavior seed should be within the range of [{}, {}]"),
                             std::make_format_args(value, low, high)));
        } else {
            seed = static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
        }
    } else {
        seed = scriptEntropy();
    }
    push<RandomState>(L, seeded(seed));
    return 1;
}

int randomNextNumber(lua_State* L)
{
    auto& rng = check<RandomState>(L, 1);
    const int n = std::min(lua_gettop(L), 3);
    if (n == 2)
        raise(L, "Random:NextNumber requires 0 or 2 arguments");
    const double min = n == 3 ? luaL_checknumber(L, 2) : 0.0;
    const double max = n == 3 ? luaL_checknumber(L, 3) : 1.0;
    pushNumber(L, min + (max - min) * nextUnit(rng));
    return 1;
}

int randomNextInteger(lua_State* L)
{
    auto& rng = check<RandomState>(L, 1);
    const auto min = static_cast<std::int64_t>(luaL_checknumber(L, 2));
    const auto max = static_cast<std::int64_t>(luaL_checknumber(L, 3));
    if (max < min)
        luaL_argerror(L, 3, "interval is empty");
    pushNumber(L, static_cast<double>(nextInteger(rng, min, max)));
    return 1;
}

int randomNextUnitVector(lua_State* L)
{
    auto& rng = check<RandomState>(L, 1);
    // Uniform on the sphere. (Guess: the client's construction isn't known.)
    const double z = nextUnit(rng) * 2.0 - 1.0;
    const double phi = nextUnit(rng) * 2.0 * std::numbers::pi;
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    pushVector3(L, Vector3{static_cast<float>(r * std::cos(phi)), static_cast<float>(r * std::sin(phi)),
                           static_cast<float>(z)});
    return 1;
}

int randomShuffle(lua_State* L)
{
    auto& rng = check<RandomState>(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const int n = lua_objlen(L, 2);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        const bool isNil = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (isNil)
            raise(L, concat("Attempted to shuffle table containing nil values. This is not allowed ",
                            "because the table length may change."));
    }
    for (int i = n; i > 1; --i) {
        const int j = static_cast<int>(nextInteger(rng, 1, i));
        lua_rawgeti(L, 2, i);
        lua_rawgeti(L, 2, j);
        lua_rawseti(L, 2, i);
        lua_rawseti(L, 2, j);
    }
    return 0;
}

int randomClone(lua_State* L)
{
    push<RandomState>(L, check<RandomState>(L, 1));
    return 1;
}

int randomToString(lua_State* L)
{
    lua_pushliteral(L, "Random");
    return 1;
}

void openRandom(lua_State* L)
{
    registerUserdata<RandomState>(L, indexOf<RandomState, nullptr, false>,
                                  std::to_array<luaL_Reg>({
                                      {"NextNumber", randomNextNumber},
                                      {"NextInteger", randomNextInteger},
                                      {"NextUnitVector", randomNextUnitVector},
                                      {"Shuffle", randomShuffle},
                                      {"Clone", randomClone},
                                  }),
                                  std::to_array<luaL_Reg>({{"__tostring", randomToString}}));
    setGlobalTable(L, "Random", std::to_array<luaL_Reg>({{"new", randomNew}}));
}

// --- RaycastParams, OverlapParams, RaycastResult -----------------------------------

void pushFilter(lua_State* L, const std::vector<std::weak_ptr<Instance>>& filter)
{
    lua_createtable(L, static_cast<int>(filter.size()), 0);
    int i = 0;
    for (const auto& weak : filter) {
        if (auto instance = weak.lock()) {
            pushInstance(L, instance.get());
            lua_rawseti(L, -2, ++i);
        }
    }
}

// Tables nest: {part, {model, folder}} filters all three.
void readFilter(lua_State* L, int idx, std::vector<std::weak_ptr<Instance>>& out, int depth = 0)
{
    if (Instance* instance = toInstance(L, idx)) {
        out.push_back(instance->shared_from_this());
        return;
    }
    if (!lua_istable(L, idx) || depth > 16)
        raise(L, "Invalid value type; table must only contain Instances and other tables");
    const int n = lua_objlen(L, idx);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        readFilter(L, lua_gettop(L), out, depth + 1);
        lua_pop(L, 1);
    }
}

template <class Params>
bool paramsGet(lua_State* L, const Params& p, std::string_view key)
{
    if (key == "FilterDescendantsInstances")
        pushFilter(L, p.filterDescendantsInstances);
    else if (key == "FilterType")
        pushEnumItem(L, p.filterType);
    else if (key == "CollisionGroup")
        pushString(L, p.collisionGroup);
    else if (key == "RespectCanCollide")
        lua_pushboolean(L, p.respectCanCollide);
    else if (key == "BruteForceAllSlow")
        lua_pushboolean(L, p.bruteForceAllSlow);
    else if constexpr (std::is_same_v<Params, RaycastParams>) {
        if (key == "IgnoreWater")
            lua_pushboolean(L, p.ignoreWater);
        else
            return false;
    } else {
        if (key == "MaxParts")
            pushNumber(L, p.maxParts);
        else if (key == "Tolerance")
            pushNumber(L, p.tolerance);
        else
            return false;
    }
    return true;
}

template <class Params>
void paramsSet(lua_State* L, Params& p, std::string_view key, int value)
{
    if (key == "FilterDescendantsInstances") {
        luaL_checktype(L, value, LUA_TTABLE);
        std::vector<std::weak_ptr<Instance>> filter;
        readFilter(L, value, filter);
        p.filterDescendantsInstances = std::move(filter);
    } else if (key == "FilterType") {
        auto item = toEnumItem(L, value, "RaycastFilterType");
        if (!item)
            luaL_typeerror(L, value, "Enum.RaycastFilterType");
        p.filterType = *item;
    } else if (key == "CollisionGroup") {
        p.collisionGroup = std::string(checkString(L, value));
    } else if (key == "RespectCanCollide") {
        p.respectCanCollide = lua_toboolean(L, value) != 0;
    } else if (key == "BruteForceAllSlow") {
        p.bruteForceAllSlow = lua_toboolean(L, value) != 0;
    } else if constexpr (std::is_same_v<Params, RaycastParams>) {
        if (key != "IgnoreWater")
            raise(L, std::format("{} is not a valid member of {}", key, TypeTraits<Params>::name));
        p.ignoreWater = lua_toboolean(L, value) != 0;
    } else {
        if (key == "MaxParts") {
            p.maxParts = static_cast<int>(luaL_checknumber(L, value));
        } else if (key == "Tolerance") {
            p.tolerance = static_cast<float>(luaL_checknumber(L, value));
        } else {
            raise(L, std::format("{} is not a valid member of {}", key, TypeTraits<Params>::name));
        }
    }
}

template <class Params>
int paramsNewIndex(lua_State* L)
{
    auto& p = check<Params>(L, 1);
    paramsSet(L, p, checkString(L, 2), 3);
    return 0;
}

template <class Params>
int paramsNew(lua_State* L)
{
    Params p;
    p.filterType = enumItem("RaycastFilterType", "Exclude");
    auto& made = push<Params>(L, std::move(p));
    // Newer clients take a table of initial values.
    if (lua_istable(L, 1)) {
        lua_pushnil(L);
        while (lua_next(L, 1)) {
            if (lua_type(L, -2) == LUA_TSTRING)
                paramsSet(L, made, lua_tostring(L, -2), lua_gettop(L));
            lua_pop(L, 1);
        }
    }
    return 1;
}

template <class Params>
int paramsAddToFilter(lua_State* L)
{
    auto& p = check<Params>(L, 1);
    readFilter(L, 2, p.filterDescendantsInstances);
    return 0;
}

template <class Params>
int paramsToString(lua_State* L)
{
    const Params& p = check<Params>(L, 1);
    bool ignoreWater = false;
    if constexpr (std::is_same_v<Params, RaycastParams>)
        ignoreWater = p.ignoreWater;
    pushString(L, std::format("{}{{IgnoreWater={}, FilterType={}, CollisionGroup={}}}", TypeTraits<Params>::name,
                              ignoreWater, p.filterType.name, p.collisionGroup));
    return 1;
}

bool raycastResultGet(lua_State* L, const RaycastResult& r, std::string_view key)
{
    if (key == "Instance")
        pushInstance(L, r.instance.lock().get());
    else if (key == "Position")
        pushVector3(L, r.position);
    else if (key == "Normal")
        pushVector3(L, r.normal);
    else if (key == "Material")
        pushEnumItem(L, r.material);
    else if (key == "Distance")
        pushNumber(L, r.distance);
    else
        return false;
    return true;
}

// The pieces are the client's: "RaycastResult{", "; normal = ", "; material = ".
// (Guess: the "<name> @ <position>" in front.)
int raycastResultToString(lua_State* L)
{
    const RaycastResult& r = check<RaycastResult>(L, 1);
    auto instance = r.instance.lock();
    pushString(L, std::format("RaycastResult{{{} @ {}; normal = {}; material = {}}}",
                              instance ? instance->name() : std::string("nil"), formatVector(r.position),
                              formatVector(r.normal), r.material.name));
    return 1;
}

void openQueryTypes(lua_State* L)
{
    registerUserdata<RaycastParams>(L, indexOf<RaycastParams, paramsGet<RaycastParams>, false>,
                                    std::to_array<luaL_Reg>({{"AddToFilter", paramsAddToFilter<RaycastParams>}}),
                                    std::to_array<luaL_Reg>({
                                        {"__newindex", paramsNewIndex<RaycastParams>},
                                        {"__tostring", paramsToString<RaycastParams>},
                                    }));
    setGlobalTable(L, "RaycastParams", std::to_array<luaL_Reg>({{"new", paramsNew<RaycastParams>}}));
    registerUserdata<OverlapParams>(L, indexOf<OverlapParams, paramsGet<OverlapParams>, false>,
                                    std::to_array<luaL_Reg>({{"AddToFilter", paramsAddToFilter<OverlapParams>}}),
                                    std::to_array<luaL_Reg>({
                                        {"__newindex", paramsNewIndex<OverlapParams>},
                                        {"__tostring", paramsToString<OverlapParams>},
                                    }));
    setGlobalTable(L, "OverlapParams", std::to_array<luaL_Reg>({{"new", paramsNew<OverlapParams>}}));
    registerUserdata<RaycastResult>(L, indexOf<RaycastResult, raycastResultGet, false>, {},
                                    std::to_array<luaL_Reg>({{"__tostring", raycastResultToString}}));
}

// --- Path2DControlPoint ------------------------------------------------------------

// Path2DControlPoint.new([position [, leftTangent [, rightTangent]]]): UDim2s,
// zero when left out.
int path2DControlPointNew(lua_State* L)
{
    Path2DControlPoint point;
    if (!lua_isnoneornil(L, 1))
        point.position = check<UDim2>(L, 1);
    if (!lua_isnoneornil(L, 2))
        point.leftTangent = check<UDim2>(L, 2);
    if (!lua_isnoneornil(L, 3))
        point.rightTangent = check<UDim2>(L, 3);
    push<Path2DControlPoint>(L, point);
    return 1;
}

// GUESS: the client's format; the three UDim2s as tostring prints them.
int path2DControlPointToString(lua_State* L)
{
    const Path2DControlPoint& p = check<Path2DControlPoint>(L, 1);
    pushString(L, std::format("{}, {}, {}", lunar::toString(Value(p.position)), lunar::toString(Value(p.leftTangent)),
                              lunar::toString(Value(p.rightTangent))));
    return 1;
}

void openPath2D(lua_State* L)
{
    registerUserdata<Path2DControlPoint>(L, indexOf<Path2DControlPoint>, {},
                                         std::to_array<luaL_Reg>({
                                             {"__eq", eqOf<Path2DControlPoint>},
                                             {"__tostring", path2DControlPointToString},
                                         }));
    setGlobalTable(L, "Path2DControlPoint", std::to_array<luaL_Reg>({{"new", path2DControlPointNew}}));
}

// --- Vector2int16 --------------------------------------------------------------------

// Wraps as Vector3int16's components do (ValueTypes.cpp).
std::int16_t toInt16(double value)
{
    return static_cast<std::int16_t>(static_cast<std::int64_t>(value));
}

int vector2int16New(lua_State* L)
{
    push<Vector2int16>(L, Vector2int16{toInt16(luaL_optnumber(L, 1, 0)), toInt16(luaL_optnumber(L, 2, 0))});
    return 1;
}

// As Vector3int16's: + and - take another, * and / another or a number.
template <char Op>
int vector2int16Arith(lua_State* L)
{
    auto component = [&](int idx, int axis) -> std::optional<int> {
        if (const Vector2int16* v = to<Vector2int16>(L, idx))
            return axis == 0 ? v->x : v->y;
        if (lua_type(L, idx) == LUA_TNUMBER && (Op == '*' || Op == '/'))
            return static_cast<int>(lua_tonumber(L, idx));
        return std::nullopt;
    };
    std::array<std::int16_t, 2> r{};
    for (int axis = 0; axis < 2; ++axis) {
        const auto x = component(1, axis);
        const auto y = component(2, axis);
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
    push<Vector2int16>(L, Vector2int16{r[0], r[1]});
    return 1;
}

int vector2int16Unm(lua_State* L)
{
    const Vector2int16& v = check<Vector2int16>(L, 1);
    push<Vector2int16>(L, Vector2int16{static_cast<std::int16_t>(-v.x), static_cast<std::int16_t>(-v.y)});
    return 1;
}

int vector2int16ToString(lua_State* L)
{
    const Vector2int16& v = check<Vector2int16>(L, 1);
    pushString(L, std::format("{}, {}", v.x, v.y));
    return 1;
}

void openVector2int16(lua_State* L)
{
    registerUserdata<Vector2int16>(L, indexOf<Vector2int16>, {},
                                   std::to_array<luaL_Reg>({
                                       {"__add", vector2int16Arith<'+'>},
                                       {"__sub", vector2int16Arith<'-'>},
                                       {"__mul", vector2int16Arith<'*'>},
                                       {"__div", vector2int16Arith<'/'>},
                                       {"__unm", vector2int16Unm},
                                       {"__eq", eqOf<Vector2int16>},
                                       {"__tostring", vector2int16ToString},
                                   }));
    setGlobalTable(L, "Vector2int16", std::to_array<luaL_Reg>({{"new", vector2int16New}}));
}

// --- PathWaypoint --------------------------------------------------------------------

// PathWaypoint.new([position [, action [, label]]]): a Walk with no label
// when left out.
int pathWaypointNew(lua_State* L)
{
    PathWaypoint waypoint{lua_isnoneornil(L, 1) ? Vector3{} : checkVector3(L, 1),
                          enumItem("PathWaypointAction", "Walk"), {}};
    if (!lua_isnoneornil(L, 2)) {
        auto action = toEnumItem(L, 2, "PathWaypointAction", true);
        if (!action)
            luaL_typeerror(L, 2, "Enum.PathWaypointAction");
        waypoint.action = *action;
    }
    if (!lua_isnoneornil(L, 3))
        waypoint.label = checkString(L, 3);
    push<PathWaypoint>(L, waypoint);
    return 1;
}

// GUESS: the client's format; the position, then the action's name.
int pathWaypointToString(lua_State* L)
{
    const PathWaypoint& w = check<PathWaypoint>(L, 1);
    pushString(L, std::format("{}, {}", formatVector(w.position), w.action.name));
    return 1;
}

void openPathWaypoint(lua_State* L)
{
    registerUserdata<PathWaypoint>(L, indexOf<PathWaypoint>, {},
                                   std::to_array<luaL_Reg>({
                                       {"__eq", eqOf<PathWaypoint>},
                                       {"__tostring", pathWaypointToString},
                                   }));
    setGlobalTable(L, "PathWaypoint", std::to_array<luaL_Reg>({{"new", pathWaypointNew}}));
}

// --- RotationCurveKey ----------------------------------------------------------------

// RotationCurveKey.new(time, cframe, interpolation).
int rotationCurveKeyNew(lua_State* L)
{
    RotationCurveKey key{static_cast<float>(luaL_checknumber(L, 1)), checkCFrame(L, 2), {}};
    auto interpolation = toEnumItem(L, 3, "KeyInterpolationMode", true);
    if (!interpolation)
        luaL_typeerror(L, 3, "Enum.KeyInterpolationMode");
    key.interpolation = *interpolation;
    push<RotationCurveKey>(L, key);
    return 1;
}

// The tangents are nil until something sets them, which nothing here does.
bool rotationCurveKeyGet(lua_State* L, const RotationCurveKey& key, std::string_view name)
{
    if (name == "Time")
        pushNumber(L, key.time);
    else if (name == "Value")
        pushCFrame(L, key.value);
    else if (name == "Interpolation")
        pushEnumItem(L, key.interpolation);
    else if (name == "LeftTangent" || name == "RightTangent")
        lua_pushnil(L);
    else
        return false;
    return true;
}

// GUESS: the client's format; the time, the CFrame and the mode's name.
int rotationCurveKeyToString(lua_State* L)
{
    const RotationCurveKey& key = check<RotationCurveKey>(L, 1);
    pushString(L, std::format("{}, {}, {}", key.time, lunar::toString(Value(key.value)), key.interpolation.name));
    return 1;
}

void openRotationCurveKey(lua_State* L)
{
    registerUserdata<RotationCurveKey>(L, indexOf<RotationCurveKey, rotationCurveKeyGet, false>, {},
                                       std::to_array<luaL_Reg>({
                                           {"__eq", eqOf<RotationCurveKey>},
                                           {"__tostring", rotationCurveKeyToString},
                                       }));
    setGlobalTable(L, "RotationCurveKey", std::to_array<luaL_Reg>({{"new", rotationCurveKeyNew}}));
}

// --- Content -------------------------------------------------------------------------

Content contentOf(std::string_view sourceType)
{
    return Content{enumItem("ContentSourceType", sourceType), {}, {}};
}

int contentFromUri(lua_State* L)
{
    Content content = contentOf("Uri");
    content.uri = checkString(L, 1);
    push<Content>(L, content);
    return 1;
}

int contentFromAssetId(lua_State* L)
{
    Content content = contentOf("Uri");
    content.uri = std::format("rbxassetid://{}", static_cast<std::int64_t>(luaL_checknumber(L, 1)));
    push<Content>(L, content);
    return 1;
}

// GUESS: any instance; the client takes the objects an image or mesh can be
// (EditableImage, EditableMesh).
int contentFromObject(lua_State* L)
{
    Content content = contentOf("Object");
    content.object = checkInstance(L, 1).shared_from_this();
    push<Content>(L, content);
    return 1;
}

// Uri and Object are nil unless that is where it comes from.
bool contentGet(lua_State* L, const Content& content, std::string_view key)
{
    if (key == "SourceType")
        pushEnumItem(L, content.sourceType);
    else if (key == "Uri")
        content.sourceType.name == "Uri" ? pushString(L, content.uri) : lua_pushnil(L);
    else if (key == "Object")
        pushInstance(L, content.object.lock().get());
    else
        return false;
    return true;
}

// GUESS: the client's format; the URI, or the source type's name.
int contentToString(lua_State* L)
{
    const Content& content = check<Content>(L, 1);
    pushString(L, content.sourceType.name == "Uri" ? content.uri : std::string(content.sourceType.name));
    return 1;
}

void openContent(lua_State* L)
{
    registerUserdata<Content>(L, indexOf<Content, contentGet, false>, {},
                              std::to_array<luaL_Reg>({
                                  {"__eq", eqOf<Content>},
                                  {"__tostring", contentToString},
                              }));
    setGlobalTable(L, "Content",
                   std::to_array<luaL_Reg>(
                       {{"fromUri", contentFromUri}, {"fromAssetId", contentFromAssetId}, {"fromObject", contentFromObject}}));
    // Content.none, a value rather than a constructor: the table is read-only
    // once made, so it goes in while it can.
    lua_getglobal(L, "Content");
    lua_setreadonly(L, -1, false);
    push<Content>(L, contentOf("None"));
    lua_setfield(L, -2, "none");
    lua_setreadonly(L, -1, true);
    lua_pop(L, 1);
}

// --- PluginDrag ----------------------------------------------------------------------

// PluginDrag.new([sender [, mimeType [, data [, mouseIcon [, dragIcon [,
// hotSpot]]]]]]): empty strings and a zero hot spot when left out.
int pluginDragNew(lua_State* L)
{
    PluginDrag drag;
    for (auto [idx, field] : {std::pair{1, &PluginDrag::sender}, std::pair{2, &PluginDrag::mimeType},
                              std::pair{3, &PluginDrag::data}, std::pair{4, &PluginDrag::mouseIcon},
                              std::pair{5, &PluginDrag::dragIcon}})
        if (!lua_isnoneornil(L, idx))
            drag.*field = checkString(L, idx);
    if (!lua_isnoneornil(L, 6))
        drag.hotSpot = check<Vector2>(L, 6);
    push<PluginDrag>(L, drag);
    return 1;
}

void openPluginDrag(lua_State* L)
{
    registerUserdata<PluginDrag>(L, indexOf<PluginDrag>, {}, std::to_array<luaL_Reg>({{"__eq", eqOf<PluginDrag>}}));
    setGlobalTable(L, "PluginDrag", std::to_array<luaL_Reg>({{"new", pluginDragNew}}));
}

// GUESS: the client has a CellId global (a terrain voxel's address, it
// seems), but what it makes and takes isn't known here, so it is the library
// table alone [sUNC's getrenv checks its type].
void openCellId(lua_State* L)
{
    setGlobalTable(L, "CellId", {});
}

} // namespace

void openScriptTypes(lua_State* L)
{
    openTweenInfo(L);
    openRay(L);
    openRegions(L);
    openDateTime(L);
    openRandom(L);
    openQueryTypes(L);
    openPath2D(L);
    openVector2int16(L);
    openPathWaypoint(L);
    openRotationCurveKey(L);
    openContent(L);
    openPluginDrag(L);
    openCellId(L);
}

} // namespace lunar::lua

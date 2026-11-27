// The console's Drawing library (Console.cpp opens it): Drawing.new and
// Drawing.Fonts, with isrenderobj, getrenderproperty, setrenderproperty and
// cleardrawcache, the shapes the sUNC docs give.
//
// A drawing is a userdata of its own, not an Instance: a type (Line, Text,
// Image, Circle, Square, Quad or Triangle) and whether it still exists, with
// its properties in a table the registry keeps for it. Every type has Visible,
// ZIndex, Transparency (1 is see-through, as sUNC reads the word) and Color;
// the rest are the type's own.
//
// What shows on screen is GUI the engine already draws, in a ScreenGui of
// gethui()'s that ignores the inset, so a drawing's Position is a viewport
// point as WorldToViewportPoint gives one: a Circle is a Frame rounded by a
// UICorner of (1, 0) and Radius * 2 across, centred on its Position; a Square
// a Frame; a Line a Frame turned along it; Text a TextLabel. A UIStroke draws
// the outline of the ones that aren't Filled (and Text's Outline). sUNC looks
// for the Circle's Frame by that UICorner and checks its size and place
// against the Radius and the Position. Image, Quad and Triangle keep their
// properties and draw nothing yet. Each write updates the GUI, and Destroy
// (or cleardrawcache) destroys it.

#include "Console.hpp"
#include "Runtime.hpp"
#include "Userdata.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Instance.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace lunar::lua {

namespace {

// Past the tags Runtime.hpp names, which end below this.
constexpr int DrawingTag = 64;
static_assert(DrawingTag > SharedTableTag);

// The registry's table of every drawing to its properties, weak in its keys.
constexpr const char* drawingsKey = "lunarstone.drawings";
// And the draw cache: the drawings that still exist, held, as an executor's
// stay on screen until they're destroyed whether or not anything refers to
// them. cleardrawcache walks it.
constexpr const char* cacheKey = "lunarstone.drawcache";

struct Drawing {
    std::string type;
    bool exists = true;
    InstancePtr gui;    // what shows it, if its type shows
    InstancePtr stroke; // the gui's UIStroke, for an outline
};

// Pushes the properties of the drawing at idx.
void pushProperties(lua_State* L, int idx)
{
    idx = lua_absindex(L, idx);
    lua_rawgetfield(L, LUA_REGISTRYINDEX, drawingsKey);
    lua_pushvalue(L, idx);
    lua_rawget(L, -2);
    lua_remove(L, -2);
}

void pushDrawings(lua_State* L)
{
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
}

enum class Kind : std::uint8_t { Boolean, Number, String, Vector2, Color3 };

struct Property {
    std::string_view name;
    Kind kind;
};

constexpr std::array shared{Property{"Visible", Kind::Boolean}, Property{"ZIndex", Kind::Number},
                            Property{"Transparency", Kind::Number}, Property{"Color", Kind::Color3}};

struct Type {
    std::string_view name;
    std::span<const Property> properties;
};

constexpr std::array line{Property{"From", Kind::Vector2}, Property{"To", Kind::Vector2},
                          Property{"Thickness", Kind::Number}};
constexpr std::array text{Property{"Text", Kind::String},     Property{"Font", Kind::Number},
                          Property{"Size", Kind::Number},     Property{"Position", Kind::Vector2},
                          Property{"Center", Kind::Boolean},  Property{"Outline", Kind::Boolean},
                          Property{"OutlineColor", Kind::Color3}};
constexpr std::array image{Property{"Data", Kind::String}, Property{"Size", Kind::Vector2},
                           Property{"Position", Kind::Vector2}, Property{"Rounding", Kind::Number}};
constexpr std::array circle{Property{"NumSides", Kind::Number}, Property{"Radius", Kind::Number},
                            Property{"Position", Kind::Vector2}, Property{"Thickness", Kind::Number},
                            Property{"Filled", Kind::Boolean}};
constexpr std::array square{Property{"Size", Kind::Vector2}, Property{"Position", Kind::Vector2},
                            Property{"Thickness", Kind::Number}, Property{"Filled", Kind::Boolean}};
constexpr std::array quad{Property{"PointA", Kind::Vector2}, Property{"PointB", Kind::Vector2},
                          Property{"PointC", Kind::Vector2},  Property{"PointD", Kind::Vector2},
                          Property{"Thickness", Kind::Number}, Property{"Filled", Kind::Boolean}};
constexpr std::array triangle{Property{"PointA", Kind::Vector2}, Property{"PointB", Kind::Vector2},
                              Property{"PointC", Kind::Vector2}, Property{"Thickness", Kind::Number},
                              Property{"Filled", Kind::Boolean}};

constexpr std::array types{Type{"Line", line},     Type{"Text", text},     Type{"Image", image},
                           Type{"Circle", circle}, Type{"Square", square}, Type{"Quad", quad},
                           Type{"Triangle", triangle}};

const Type* typeNamed(std::string_view name)
{
    const auto it = std::ranges::find(types, name, &Type::name);
    return it == types.end() ? nullptr : &*it;
}

const Property* propertyOf(const Drawing& drawing, std::string_view name)
{
    if (const auto it = std::ranges::find(shared, name, &Property::name); it != shared.end())
        return &*it;
    const Type* type = typeNamed(drawing.type);
    const auto it = std::ranges::find(type->properties, name, &Property::name);
    return it == type->properties.end() ? nullptr : &*it;
}

Drawing* toDrawing(lua_State* L, int idx)
{
    return static_cast<Drawing*>(lua_touserdatatagged(L, idx, DrawingTag));
}

Drawing& checkDrawing(lua_State* L, int idx)
{
    if (Drawing* drawing = toDrawing(L, idx))
        return *drawing;
    luaL_typeerror(L, idx, "Drawing");
}

// What a property starts as: off, nothing, the origin and black, a line
// one pixel thick and text 16 high.
void pushDefault(lua_State* L, const Property& property)
{
    switch (property.kind) {
    case Kind::Boolean:
        lua_pushboolean(L, false);
        break;
    case Kind::Number:
        lua_pushnumber(L, property.name == "Thickness" || property.name == "ZIndex" ? 1.0
                          : property.name == "Size"                                 ? 16.0
                                                                                    : 0.0);
        break;
    case Kind::String:
        lua_pushliteral(L, "");
        break;
    case Kind::Vector2:
        push(L, Vector2{});
        break;
    case Kind::Color3:
        pushColor3(L, Color3{});
        break;
    }
}

void checkKind(lua_State* L, int idx, const Property& property)
{
    bool ok = false;
    const char* expected = "";
    switch (property.kind) {
    case Kind::Boolean:
        ok = lua_isboolean(L, idx);
        expected = "boolean";
        break;
    case Kind::Number:
        ok = lua_type(L, idx) == LUA_TNUMBER;
        expected = "number";
        break;
    case Kind::String:
        ok = lua_type(L, idx) == LUA_TSTRING;
        expected = "string";
        break;
    case Kind::Vector2:
        ok = to<Vector2>(L, idx) != nullptr;
        expected = "Vector2";
        break;
    case Kind::Color3:
        ok = toColor3(L, idx) != nullptr;
        expected = "Color3";
        break;
    }
    if (!ok)
        raise(L, std::format("invalid value for {} (expected {}, got {})", property.name, expected,
                             luaL_typename(L, idx)));
}

// --- What shows it -------------------------------------------------------------

// gethui()'s ScreenGui the drawings show in.
Instance& container(lua_State* L)
{
    Instance& hui = consoleGui(L);
    if (Instance* existing = hui.findFirstChild("Drawing"); existing && existing->isA("ScreenGui"))
        return *existing;
    InstancePtr gui = ClassRegistry::create("ScreenGui");
    gui->setName("Drawing");
    pushInstance(L, gui.get());
    lua_pushboolean(L, true);
    lua_setfield(L, -2, "IgnoreGuiInset");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "ResetOnSpawn");
    lua_pushinteger(L, std::numeric_limits<std::int32_t>::max());
    lua_setfield(L, -2, "DisplayOrder");
    lua_pop(L, 1);
    gui->setParent(&hui);
    return *gui;
}

void pushEnum(lua_State* L, const char* type, const char* item)
{
    lua_getglobal(L, "Enum");
    lua_getfield(L, -1, type);
    lua_getfield(L, -1, item);
    lua_replace(L, -3);
    lua_pop(L, 1);
}

UDim2 offset(float x, float y)
{
    return UDim2::fromOffset(static_cast<std::int32_t>(std::lround(x)), static_cast<std::int32_t>(std::lround(y)));
}

// The drawing's properties at `props`, read for the GUI.
struct Reader {
    lua_State* L;
    int props;

    double number(const char* name) const
    {
        lua_rawgetfield(L, props, name);
        const double value = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return value;
    }
    bool boolean(const char* name) const
    {
        lua_rawgetfield(L, props, name);
        const bool value = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return value;
    }
    Vector2 vector(const char* name) const
    {
        lua_rawgetfield(L, props, name);
        const Vector2* value = to<Vector2>(L, -1);
        const Vector2 out = value ? *value : Vector2{};
        lua_pop(L, 1);
        return out;
    }
    Color3 color(const char* name) const
    {
        lua_rawgetfield(L, props, name);
        const Color3* value = toColor3(L, -1);
        const Color3 out = value ? *value : Color3{};
        lua_pop(L, 1);
        return out;
    }
};

// The GUI made to show a drawing of the type, unparented; null for a type
// that doesn't show.
void makeGui(lua_State* L, Drawing& drawing)
{
    const std::string_view type = drawing.type;
    if (type == "Circle" || type == "Square" || type == "Line") {
        drawing.gui = ClassRegistry::create("Frame");
        if (type == "Circle") {
            InstancePtr corner = ClassRegistry::create("UICorner");
            pushInstance(L, corner.get());
            push(L, UDim{1.0f, 0});
            lua_setfield(L, -2, "CornerRadius");
            lua_pop(L, 1);
            corner->setParent(drawing.gui.get());
        }
    } else if (type == "Text") {
        drawing.gui = ClassRegistry::create("TextLabel");
    } else {
        return;
    }
    drawing.gui->setName(drawing.type);
    if (type != "Line") {
        drawing.stroke = ClassRegistry::create("UIStroke");
        drawing.stroke->setParent(drawing.gui.get());
    }
}

// Brings the drawing's GUI (the drawing at idx) in line with its properties.
void sync(lua_State* L, int idx)
{
    idx = lua_absindex(L, idx);
    Drawing& drawing = *static_cast<Drawing*>(lua_touserdatatagged(L, idx, DrawingTag));
    if (!drawing.gui)
        return;
    lua_checkstack(L, 8);
    pushProperties(L, idx);
    const Reader read{L, lua_gettop(L)};
    pushInstance(L, drawing.gui.get());
    const int gui = lua_gettop(L);
    if (drawing.stroke)
        pushInstance(L, drawing.stroke.get());
    const int stroke = drawing.stroke ? lua_gettop(L) : 0;
    const std::string_view type = drawing.type;
    const double transparency = read.number("Transparency");
    const Color3 color = read.color("Color");

    const auto set = [&](int target, const char* field) { lua_setfield(L, target, field); };
    lua_pushboolean(L, read.boolean("Visible") && drawing.exists);
    set(gui, "Visible");
    lua_pushinteger(L, static_cast<int>(std::clamp(read.number("ZIndex"), -1e9, 1e9)));
    set(gui, "ZIndex");

    if (type == "Text") {
        lua_pushliteral(L, "");
        lua_rawgetfield(L, read.props, "Text");
        lua_replace(L, -2);
        set(gui, "Text");
        lua_pushnumber(L, read.number("Size"));
        set(gui, "TextSize");
        pushColor3(L, color);
        set(gui, "TextColor3");
        lua_pushnumber(L, transparency);
        set(gui, "TextTransparency");
        lua_pushnumber(L, 1);
        set(gui, "BackgroundTransparency");
        const bool centre = read.boolean("Center");
        push(L, Vector2{centre ? 0.5f : 0.0f, 0.0f});
        set(gui, "AnchorPoint");
        pushEnum(L, "TextXAlignment", centre ? "Center" : "Left");
        set(gui, "TextXAlignment");
        pushEnum(L, "TextYAlignment", "Top");
        set(gui, "TextYAlignment");
        const Vector2 at = read.vector("Position");
        push(L, offset(at.x, at.y));
        set(gui, "Position");
        push(L, offset(0, 0));
        set(gui, "Size");
        static constexpr std::array fonts{"SourceSans", "Arial", "Code", "Code"};
        const auto font = static_cast<std::size_t>(std::clamp(read.number("Font"), 0.0, 3.0));
        pushEnum(L, "Font", fonts.at(font));
        set(gui, "Font");
        lua_pushboolean(L, read.boolean("Outline"));
        set(stroke, "Enabled");
        pushColor3(L, read.color("OutlineColor"));
        set(stroke, "Color");
        lua_pushnumber(L, transparency);
        set(stroke, "Transparency");
    } else if (type == "Line") {
        const Vector2 from = read.vector("From");
        const Vector2 to = read.vector("To");
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        push(L, Vector2{0.5f, 0.5f});
        set(gui, "AnchorPoint");
        push(L, offset((from.x + to.x) / 2, (from.y + to.y) / 2));
        set(gui, "Position");
        push(L, offset(std::hypot(dx, dy), static_cast<float>(read.number("Thickness"))));
        set(gui, "Size");
        lua_pushnumber(L, static_cast<double>(std::atan2(dy, dx)) * 180.0 / std::numbers::pi);
        set(gui, "Rotation");
        pushColor3(L, color);
        set(gui, "BackgroundColor3");
        lua_pushnumber(L, transparency);
        set(gui, "BackgroundTransparency");
        lua_pushinteger(L, 0);
        set(gui, "BorderSizePixel");
    } else {
        const Vector2 at = read.vector("Position");
        if (type == "Circle") {
            const auto across = static_cast<float>(read.number("Radius") * 2);
            push(L, Vector2{0.5f, 0.5f});
            set(gui, "AnchorPoint");
            push(L, offset(across, across));
        } else {
            const Vector2 size = read.vector("Size");
            push(L, Vector2{});
            set(gui, "AnchorPoint");
            push(L, offset(size.x, size.y));
        }
        set(gui, "Size");
        push(L, offset(at.x, at.y));
        set(gui, "Position");
        const bool filled = read.boolean("Filled");
        pushColor3(L, color);
        set(gui, "BackgroundColor3");
        lua_pushnumber(L, filled ? transparency : 1.0);
        set(gui, "BackgroundTransparency");
        lua_pushinteger(L, 0);
        set(gui, "BorderSizePixel");
        lua_pushboolean(L, !filled);
        set(stroke, "Enabled");
        pushColor3(L, color);
        set(stroke, "Color");
        lua_pushnumber(L, read.number("Thickness"));
        set(stroke, "Thickness");
        lua_pushnumber(L, transparency);
        set(stroke, "Transparency");
    }
    lua_settop(L, read.props - 1);
}

// Takes the drawing out of existence: its GUI goes, its properties stay
// readable.
void unmake(Drawing& drawing)
{
    drawing.exists = false;
    if (drawing.gui)
        drawing.gui->destroy();
    drawing.gui = nullptr;
    drawing.stroke = nullptr;
}

// Destroy() (Remove()).
int destroy(lua_State* L)
{
    unmake(checkDrawing(L, 1));
    lua_rawgetfield(L, LUA_REGISTRYINDEX, cacheKey);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    lua_rawset(L, -3);
    return 0;
}

// TextBounds: how much room the text takes, estimated -- half its size wide
// for each character, its size high for each line.
void pushTextBounds(lua_State* L, const Drawing& drawing)
{
    // The label's, once the GUI engine has laid it out.
    if (drawing.gui) {
        pushInstance(L, drawing.gui.get());
        lua_getfield(L, -1, "TextBounds");
        if (const Vector2* bounds = to<Vector2>(L, -1); bounds && (bounds->x > 0 || bounds->y > 0)) {
            lua_remove(L, -2);
            return;
        }
        lua_pop(L, 2);
    }
    pushProperties(L, 1);
    lua_rawgetfield(L, -1, "Text");
    const std::string_view content = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_rawgetfield(L, -2, "Size");
    const double size = lua_tonumber(L, -1);
    std::size_t widest = 0, lines = content.empty() ? 0 : 1, run = 0;
    for (const char c : content) {
        if (c == '\n') {
            ++lines;
            run = 0;
        } else {
            widest = std::max(widest, ++run);
        }
    }
    lua_pop(L, 3);
    push(L, Vector2{static_cast<float>(static_cast<double>(widest) * size / 2),
                    static_cast<float>(static_cast<double>(lines) * size)});
}

int drawingIndex(lua_State* L)
{
    const Drawing& drawing = checkDrawing(L, 1);
    const std::string_view key = checkString(L, 2);
    if (key == "__OBJECT_EXISTS") {
        lua_pushboolean(L, drawing.exists);
        return 1;
    }
    if (key == "Destroy" || key == "Remove") {
        lua_pushcfunction(L, destroy, "Destroy");
        return 1;
    }
    if (key == "TextBounds" && drawing.type == "Text") {
        pushTextBounds(L, drawing);
        return 1;
    }
    if (!propertyOf(drawing, key))
        raise(L, std::format("{} is not a valid member of {}", key, drawing.type));
    pushProperties(L, 1);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
}

int drawingNewindex(lua_State* L)
{
    const Drawing& drawing = checkDrawing(L, 1);
    const std::string_view key = checkString(L, 2);
    if (key == "TextBounds" || key == "__OBJECT_EXISTS")
        raise(L, std::format("{} is read-only", key));
    const Property* property = propertyOf(drawing, key);
    if (!property)
        raise(L, std::format("{} is not a valid member of {}", key, drawing.type));
    checkKind(L, 3, *property);
    pushProperties(L, 1);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 3);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    sync(L, 1);
    return 0;
}

int drawingToString(lua_State* L)
{
    lua_pushstring(L, std::format("Drawing ({})", checkDrawing(L, 1).type).c_str());
    return 1;
}

// Drawing.new(type): a new drawing of the type, existing and not yet visible.
int drawingNew(lua_State* L)
{
    const std::string_view name = checkString(L, 1);
    const Type* type = typeNamed(name);
    if (!type)
        raise(L, std::format("'{}' is not a type of drawing", name));
    lua_createtable(L, 0, static_cast<int>(shared.size() + type->properties.size()));
    for (std::span<const Property> properties : {std::span<const Property>(shared), type->properties})
        for (const Property& property : properties) {
            pushDefault(L, property);
            lua_setfield(L, -2, std::string(property.name).c_str());
        }
    const int properties = lua_gettop(L);
    void* memory = lua_newuserdatataggedwithmetatable(L, sizeof(Drawing), DrawingTag);
    auto* drawing = new (memory) Drawing{std::string(type->name), true, nullptr, nullptr};
    lua_rawgetfield(L, LUA_REGISTRYINDEX, drawingsKey);
    lua_pushvalue(L, -2);
    lua_pushvalue(L, properties);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lua_rawgetfield(L, LUA_REGISTRYINDEX, cacheKey);
    lua_pushvalue(L, -2);
    lua_pushboolean(L, true);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    makeGui(L, *drawing);
    if (drawing->gui) {
        sync(L, -1);
        drawing->gui->setParent(&container(L));
    }
    return 1;
}

// isrenderobj(value): whether the value is a drawing (one destroyed still is).
int isRenderObj(lua_State* L)
{
    lua_pushboolean(L, toDrawing(L, 1) != nullptr);
    return 1;
}

int getRenderProperty(lua_State* L)
{
    checkDrawing(L, 1);
    checkString(L, 2);
    lua_settop(L, 2);
    return drawingIndex(L);
}

int setRenderProperty(lua_State* L)
{
    checkDrawing(L, 1);
    checkString(L, 2);
    luaL_checkany(L, 3);
    lua_settop(L, 3);
    return drawingNewindex(L);
}

// cleardrawcache(): every drawing stops existing, as each one's Destroy does.
int clearDrawCache(lua_State* L)
{
    lua_rawgetfield(L, LUA_REGISTRYINDEX, cacheKey);
    const int drawings = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, drawings)) {
        if (Drawing* drawing = toDrawing(L, -2))
            unmake(*drawing);
        lua_pop(L, 1);
    }
    lua_newtable(L);
    lua_rawsetfield(L, LUA_REGISTRYINDEX, cacheKey);
    return 0;
}

} // namespace

void openConsoleDrawing(lua_State* thread)
{
    // The metatable is the VM's, so it is made the first time a console's
    // globals are.
    lua_getuserdatametatable(thread, DrawingTag);
    if (lua_isnil(thread, -1)) {
        lua_pop(thread, 1);
        lua_newtable(thread);
        lua_pushcfunction(thread, drawingIndex, "__index");
        lua_setfield(thread, -2, "__index");
        lua_pushcfunction(thread, drawingNewindex, "__newindex");
        lua_setfield(thread, -2, "__newindex");
        lua_pushcfunction(thread, drawingToString, "__tostring");
        lua_setfield(thread, -2, "__tostring");
        // No __type: a drawing is a plain userdata to typeof, as an
        // executor's is [sUNC: "Invalid drawing type" for one that isn't].
        lua_pushliteral(thread, "The metatable is locked");
        lua_setfield(thread, -2, "__metatable");
        lua_setreadonly(thread, -1, true);
        lua_setuserdatametatable(thread, DrawingTag);
        lua_setuserdatadtor(thread, DrawingTag,
                            [](lua_State*, void* memory) { static_cast<Drawing*>(memory)->~Drawing(); });
        pushDrawings(thread);
        lua_rawsetfield(thread, LUA_REGISTRYINDEX, drawingsKey);
        lua_newtable(thread);
        lua_rawsetfield(thread, LUA_REGISTRYINDEX, cacheKey);
    } else {
        lua_pop(thread, 1);
    }

    lua_newtable(thread);
    lua_pushcfunction(thread, drawingNew, "new");
    lua_setfield(thread, -2, "new");
    lua_newtable(thread);
    int font = 0;
    for (const char* name : {"UI", "System", "Plex", "Monospace"}) {
        lua_pushinteger(thread, font++);
        lua_setfield(thread, -2, name);
    }
    lua_setfield(thread, -2, "Fonts");
    lua_setglobal(thread, "Drawing");

    for (const auto& [name, function] :
         {std::pair{"isrenderobj", isRenderObj}, std::pair{"getrenderproperty", getRenderProperty},
          std::pair{"setrenderproperty", setRenderProperty}, std::pair{"cleardrawcache", clearDrawCache}}) {
        lua_pushcfunction(thread, function, name);
        lua_setglobal(thread, name);
    }
}

} // namespace lunar::lua

// StyleSheet, StyleRule, the styled-property queries (Instance:GetStyled) and
// ResetPropertyToDefault: what Roblox's Foundation UI library builds its look from.

#include "InstanceApi.hpp"
#include "Runtime.hpp"
#include "Userdata.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>

#include <array>
#include <format>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lunar::lua {

namespace {

// A list of instances of class T from the array at idx.
template <class T>
std::vector<T*> instanceList(lua_State* L, int idx, const char* what)
{
    luaL_checktype(L, idx, LUA_TTABLE);
    std::vector<T*> out;
    const int count = lua_objlen(L, idx);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, idx, i);
        auto* item = dynamic_cast<T*>(toInstance(L, -1));
        lua_pop(L, 1);
        if (!item)
            raise(L, std::format("{}: item {} is not a {}", what, i, T::staticClassInfo().name));
        out.push_back(item);
    }
    return out;
}

template <class T>
int pushInstances(lua_State* L, const std::vector<T*>& items)
{
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        pushInstance(L, items[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int getStyleRules(lua_State* L)
{
    return pushInstances(L, static_cast<StyleBase&>(self(L)).styleRules());
}

int setStyleRules(lua_State* L)
{
    auto& base = static_cast<StyleBase&>(self(L));
    std::vector<StyleRule*> rules = instanceList<StyleRule>(L, 2, "SetStyleRules");
    guard(L, [&] { base.assignStyleRules(std::move(rules)); });
    return 0;
}

// InsertStyleRule(rule, index?): into the list at index (1-based), else last.
int insertStyleRule(lua_State* L)
{
    auto& base = static_cast<StyleBase&>(self(L));
    auto* rule = dynamic_cast<StyleRule*>(toInstance(L, 2));
    if (!rule)
        raise(L, "InsertStyleRule: argument 1 is not a StyleRule");
    std::vector<StyleRule*> rules = base.styleRules();
    std::erase(rules, rule);
    const auto at = static_cast<std::size_t>(std::clamp(luaL_optinteger(L, 3, static_cast<int>(rules.size()) + 1), 1,
                                                        static_cast<int>(rules.size()) + 1));
    rules.insert(rules.begin() + static_cast<std::ptrdiff_t>(at - 1), rule);
    guard(L, [&] { base.assignStyleRules(std::move(rules)); });
    return 0;
}

StyleRule& checkRule(lua_State* L)
{
    return static_cast<StyleRule&>(self(L));
}

// A property value as the rule keeps it; nil and values with no engine form
// (functions, tables) are errors.
Value ruleValue(lua_State* L, int idx, std::string_view name)
{
    std::optional<Value> value = toValue(L, idx);
    if (!value || lua_isnil(L, idx))
        raise(L, std::format("StyleRule: {} can't be given a {}", name, luaL_typename(L, idx)));
    return std::move(*value);
}

int ruleSetProperties(lua_State* L)
{
    StyleRule& rule = checkRule(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::pair<std::string, Value>> properties;
    for (lua_pushnil(L); lua_next(L, 2) != 0; lua_pop(L, 1)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            raise(L, "SetProperties: property names are strings");
        std::string name = lua_tostring(L, -2);
        Value value = ruleValue(L, -1, name);
        properties.emplace_back(std::move(name), std::move(value));
    }
    guard(L, [&] { rule.setStyleProperties(std::move(properties)); });
    return 0;
}

int ruleSetProperty(lua_State* L)
{
    StyleRule& rule = checkRule(L);
    std::string name(checkString(L, 2));
    Value value = ruleValue(L, 3, name);
    guard(L, [&] { rule.setStyleProperty(std::move(name), std::move(value)); });
    return 0;
}

int ruleGetProperties(lua_State* L)
{
    const StyleRule& rule = checkRule(L);
    lua_createtable(L, 0, static_cast<int>(rule.styleProperties().size()));
    for (const auto& [name, value] : rule.styleProperties()) {
        pushValue(L, value);
        lua_setfield(L, -2, name.c_str());
    }
    return 1;
}

int ruleGetProperty(lua_State* L)
{
    const StyleRule& rule = checkRule(L);
    if (const Value* value = rule.styleProperty(checkString(L, 2)))
        pushValue(L, *value);
    else
        lua_pushnil(L);
    return 1;
}

int sheetGetDerives(lua_State* L)
{
    return pushInstances(L, static_cast<StyleSheet&>(self(L)).derives());
}

int sheetSetDerives(lua_State* L)
{
    auto& sheet = static_cast<StyleSheet&>(self(L));
    std::vector<StyleSheet*> sheets = instanceList<StyleSheet>(L, 2, "SetDerives");
    guard(L, [&] { sheet.assignDerives(std::move(sheets)); });
    return 0;
}

// Instance:GetStyled(name): the property as styling leaves it, which is the
// property itself while the engine applies rules by setting properties.
int getStyled(lua_State* L)
{
    Instance& instance = self(L);
    const std::string_view name = checkString(L, 2);
    pushValue(L, guard(L, [&] { return instance.get(name); }));
    return 1;
}

// Instance:ResetPropertyToDefault(name): the class's default again, and no
// longer the instance's own, so styling may set it. ReactRoblox resets a
// prop that goes nil this way (Foundation's Interactable leaves a button's
// BackgroundTransparency to the "gui-object-defaults" style); the
// StyleEngine hears it through eventFired, under this method's name.
int resetPropertyToDefault(lua_State* L)
{
    Instance& instance = self(L);
    const std::string name(checkString(L, 2));
    guard(L, [&] {
        static std::mutex mutex;
        static std::map<std::pair<std::string, std::string>, Value> defaults;
        const std::pair key{std::string(instance.className()), name};
        Value value;
        {
            const std::scoped_lock lock(mutex);
            auto it = defaults.find(key);
            if (it == defaults.end()) {
                const InstancePtr fresh = ClassRegistry::create(instance.className());
                if (!fresh)
                    throw Error(std::format("{} has no default {}", instance.className(), name));
                it = defaults.emplace(key, fresh->get(name)).first;
            }
            value = it->second;
        }
        instance.set(name, value);
    });
    const std::array<Value, 1> arguments{Value(name)};
    instance.fireEvent("ResetPropertyToDefault", arguments);
    return 0;
}

int getStyledPropertyChangedSignal(lua_State* L)
{
    Instance& instance = self(L);
    pushPropertySignal(L, instance, std::string(checkString(L, 2)));
    return 1;
}

int nothing(lua_State* L)
{
    self(L);
    return 0;
}

int nilResult(lua_State* L)
{
    self(L);
    lua_pushnil(L);
    return 1;
}

int emptyTable(lua_State* L)
{
    self(L);
    lua_newtable(L);
    return 1;
}

constexpr auto methods = std::to_array<Method>({
    {"GetStyleRules", "StyleBase", getStyleRules},
    {"SetStyleRules", "StyleBase", setStyleRules},
    {"InsertStyleRule", "StyleBase", insertStyleRule},
    {"SetProperties", "StyleRule", ruleSetProperties},
    {"SetProperty", "StyleRule", ruleSetProperty},
    {"GetProperties", "StyleRule", ruleGetProperties},
    {"GetProperty", "StyleRule", ruleGetProperty},
    // GUESS: tokens ("$Name") resolve when the engine applies the rule.
    {"GetPropertiesResolved", "StyleRule", ruleGetProperties},
    {"GetPropertyResolved", "StyleRule", ruleGetProperty},
    // Transitions animate a styled property's changes; changes are instant here.
    {"SetPropertyTransition", "StyleRule", nothing},
    {"SetPropertyTransitions", "StyleRule", nothing},
    {"SetDefaultPropertyTransition", "StyleRule", nothing},
    {"GetPropertyTransitions", "StyleRule", emptyTable},
    {"GetDefaultPropertyTransition", "StyleRule", nilResult},
    {"GetDerives", "StyleSheet", sheetGetDerives},
    {"SetDerives", "StyleSheet", sheetSetDerives},
    {"GetStyled", "Instance", getStyled},
    {"ResetPropertyToDefault", "Instance", resetPropertyToDefault},
    {"GetStyledPropertyChangedSignal", "Instance", getStyledPropertyChangedSignal},
});

} // namespace

std::span<const Method> styleMethods()
{
    return methods;
}

} // namespace lunar::lua

// Tools from scripts: Tool:Activate/Deactivate, Humanoid:EquipTool and
// UnequipTools, and StarterGui:SetCoreGuiEnabled, which hides the backpack
// hotbar among others. The equipping itself (the RightGrip weld, Equipped and
// Unequipped) is the engine's ToolSystem, which every game's Shared runs.
//
// Messages are the 0.742 client's.

#include "InstanceApi.hpp"
#include "Services.hpp"
#include "Userdata.hpp"

#include <lunar/Tools.hpp>

#include <array>
#include <format>
#include <memory>
#include <optional>

namespace lunar::lua {

namespace {

// The method table only offers these to Tools.
Tool& checkTool(lua_State* L)
{
    return static_cast<Tool&>(self(L));
}

int toolActivate(lua_State* L)
{
    Tool& tool = checkTool(L);
    Runtime& rt = Runtime::of(L);
    if (!rt.shared().tools->activate(tool, true))
        rt.output(MessageType::Warning,
                  "Tool:Activate() called from script when tool is not equipped. Tool will not be activated.");
    return 0;
}

int toolDeactivate(lua_State* L)
{
    Tool& tool = checkTool(L);
    Runtime& rt = Runtime::of(L);
    if (!rt.shared().tools->deactivate(tool, true))
        rt.output(MessageType::Warning,
                  "Tool:Deactivate() called from script when tool is not equipped. Tool will not be deactivated.");
    return 0;
}

int humanoidUnequipTools(lua_State* L)
{
    auto* humanoid = dynamic_cast<Humanoid*>(&self(L));
    Runtime& rt = Runtime::of(L);
    if (humanoid && !unequipTools(rt.game(), *humanoid))
        rt.output(MessageType::Warning, "UnequipTools could not find Player backpack!");
    return 0;
}

int humanoidEquipTool(lua_State* L)
{
    auto* humanoid = dynamic_cast<Humanoid*>(&self(L));
    auto* tool = dynamic_cast<Tool*>(&checkInstance(L, 2));
    // GUESS: anything but a Tool is ignored.
    if (humanoid && tool)
        guard(L, [&] { Runtime::of(L).shared().tools->equip(*humanoid, *tool); });
    return 0;
}

Enum::CoreGuiType checkCoreGuiType(lua_State* L, int idx)
{
    if (lua_isnoneornil(L, idx))
        raise(L, std::format("Argument {} missing or nil", idx - 1));
    const std::optional<EnumItem> item = toEnumItem(L, idx, "CoreGuiType");
    if (!item)
        raise(L, std::format("Unable to cast {} to token", luaL_typename(L, idx)));
    return static_cast<Enum::CoreGuiType>(item->value);
}

int starterGuiSetCoreGuiEnabled(lua_State* L)
{
    auto* starterGui = dynamic_cast<StarterGui*>(&self(L));
    const Enum::CoreGuiType type = checkCoreGuiType(L, 2);
    if (starterGui)
        starterGui->setCoreGuiEnabled(type, lua_toboolean(L, 3) != 0);
    return 0;
}

int starterGuiGetCoreGuiEnabled(lua_State* L)
{
    auto* starterGui = dynamic_cast<StarterGui*>(&self(L));
    const Enum::CoreGuiType type = checkCoreGuiType(L, 2);
    lua_pushboolean(L, !starterGui || starterGui->coreGuiEnabled(type));
    return 1;
}

constexpr auto methods = std::to_array<Method>({
    {"Activate", "Tool", toolActivate},
    {"Deactivate", "Tool", toolDeactivate},
    {"UnequipTools", "Humanoid", humanoidUnequipTools},
    {"EquipTool", "Humanoid", humanoidEquipTool},
    {"SetCoreGuiEnabled", "StarterGui", starterGuiSetCoreGuiEnabled},
    {"GetCoreGuiEnabled", "StarterGui", starterGuiGetCoreGuiEnabled},
});

} // namespace

std::span<const Method> toolMethods()
{
    return methods;
}

void startTools(Shared& shared, DataModel& game)
{
    if (shared.tools)
        return;
    shared.tools = std::make_unique<ToolSystem>(game);
    shared.tools->mouse = [](Player& player) { return legacyMouse(player).shared_from_this(); };
}

} // namespace lunar::lua

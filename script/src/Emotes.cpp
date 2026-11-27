#include "InstanceApi.hpp"
#include "Userdata.hpp"

#include <cmath>
#include <limits>

namespace lunar::lua {
namespace {

Packed packEmotes(const HumanoidDescription::Emotes& emotes)
{
    auto table = std::make_shared<PackedTable>();
    for (const auto& [name, ids] : emotes) {
        auto array = std::make_shared<PackedTable>();
        double index = 0;
        for (const auto id : ids)
            array->emplace_back(packed(Value(++index)), packed(Value(static_cast<double>(id))));
        table->emplace_back(packed(Value(name)), Packed{Value(), std::move(array)});
    }
    return Packed{Value(), std::move(table)};
}

Packed packEquipped(const HumanoidDescription::EquippedEmotes& emotes)
{
    auto table = std::make_shared<PackedTable>();
    double index = 0;
    for (const auto& [slot, name] : emotes) {
        auto entry = std::make_shared<PackedTable>();
        entry->emplace_back(packed(Value(std::string("Slot"))), packed(Value(static_cast<double>(slot))));
        entry->emplace_back(packed(Value(std::string("Name"))), packed(Value(name)));
        table->emplace_back(packed(Value(++index)), Packed{Value(), std::move(entry)});
    }
    return Packed{Value(), std::move(table)};
}

std::optional<std::int64_t> idAt(lua_State* L, int index)
{
    if (lua_type(L, index) != LUA_TNUMBER)
        return {};
    const double value = lua_tonumber(L, index);
    if (!std::isfinite(value) || value < -0x1p63 || value >= 0x1p63)
        return {};
    return static_cast<std::int64_t>(value);
}

int getEmotes(lua_State* L)
{
    unpack(L, packEmotes(static_cast<HumanoidDescription&>(self(L)).emotes()));
    return 1;
}

int getEquipped(lua_State* L)
{
    unpack(L, packEquipped(static_cast<HumanoidDescription&>(self(L)).equippedEmotes()));
    return 1;
}

int setEmotes(lua_State* L)
{
    auto& description = static_cast<HumanoidDescription&>(self(L));
    luaL_checktype(L, 2, LUA_TTABLE);
    HumanoidDescription::Emotes emotes;
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TTABLE) {
            const std::string name(checkString(L, -2));
            auto& ids = emotes[name];
            const int count = lua_objlen(L, -1);
            for (int i = 1; i <= count; ++i) {
                lua_rawgeti(L, -1, i);
                if (const auto id = idAt(L, -1))
                    ids.insert(*id);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    description.setEmotes(std::move(emotes));
    return 0;
}

int addEmote(lua_State* L)
{
    auto& description = static_cast<HumanoidDescription&>(self(L));
    std::string name(checkString(L, 2));
    luaL_checknumber(L, 3);
    const auto id = idAt(L, 3);
    if (!id)
        raise(L, "Emote assetId must be a finite 64-bit number");
    description.addEmote(std::move(name), *id);
    return 0;
}

int removeEmote(lua_State* L)
{
    static_cast<HumanoidDescription&>(self(L)).removeEmote(checkString(L, 2));
    return 0;
}

int setEquipped(lua_State* L)
{
    auto& description = static_cast<HumanoidDescription&>(self(L));
    luaL_checktype(L, 2, LUA_TTABLE);
    HumanoidDescription::EquippedEmotes emotes;
    const int count = lua_objlen(L, 2);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, 2, i);
        if (lua_type(L, -1) == LUA_TSTRING) {
            emotes.insert_or_assign(i, std::string(checkString(L, -1)));
        } else if (lua_type(L, -1) == LUA_TTABLE) {
            lua_getfield(L, -1, "Name");
            if (lua_type(L, -1) == LUA_TSTRING) {
                const std::string name(checkString(L, -1));
                lua_pop(L, 1);
                lua_getfield(L, -1, "Slot");
                if (const auto slot = idAt(L, -1);
                    slot && *slot >= std::numeric_limits<int>::min() && *slot <= std::numeric_limits<int>::max())
                    emotes.insert_or_assign(static_cast<int>(*slot), name);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    description.setEquippedEmotes(std::move(emotes));
    return 0;
}

} // namespace

std::vector<Connection> bridgeDescriptionSignals(EventHub& hub, HumanoidDescription& owner)
{
    std::vector<Connection> connections;
    connections.push_back(owner.emotesChanged().connect(
        [&hub, &owner](const auto& emotes) { hub.fire(owner, "EmotesChanged", {packEmotes(emotes)}); }));
    connections.push_back(owner.equippedEmotesChanged().connect(
        [&hub, &owner](const auto& emotes) { hub.fire(owner, "EquippedEmotesChanged", {packEquipped(emotes)}); }));
    return connections;
}

std::span<const Method> emoteMethods()
{
    static constexpr Method methods[] = {
        {"GetEmotes", "HumanoidDescription", getEmotes},
        {"SetEmotes", "HumanoidDescription", setEmotes},
        {"AddEmote", "HumanoidDescription", addEmote},
        {"RemoveEmote", "HumanoidDescription", removeEmote},
        {"GetEquippedEmotes", "HumanoidDescription", getEquipped},
        {"SetEquippedEmotes", "HumanoidDescription", setEquipped},
    };
    return methods;
}

} // namespace lunar::lua

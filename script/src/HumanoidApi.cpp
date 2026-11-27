// The Humanoid's state machine as scripts see it: its events (relayed from
// the engine's HumanoidEvents, which the physics character controller fires
// in the client's order) and GetState, ChangeState, SetStateEnabled,
// GetStateEnabled, Move and MoveTo, which go to whatever drives the Humanoid.
//
// Play Solo has one Humanoid object for both sides, so its events reach the
// listeners of both contexts: the client's (the Animate script, for the local
// character) and the server's, which in a real game hears the state the
// owning client replicates (Died, StateChanged, the old server-side Sound
// script's Running/Jumping/FreeFalling).

#include "Characters.hpp"
#include "Userdata.hpp"

#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace lunar::lua {

namespace {

Value stateValue(Enum::HumanoidStateType state)
{
    return {enums::item(state)};
}

} // namespace

std::vector<Connection> bridgeEngineSignals(EventHub& hub, Instance& owner)
{
    if (auto* description = instanceCast<HumanoidDescription>(&owner))
        return bridgeDescriptionSignals(hub, *description);
    auto* humanoid = dynamic_cast<Humanoid*>(&owner);
    if (!humanoid)
        return {};
    // The signals belong to the Humanoid, so it's alive whenever they fire.
    const Instance* key = humanoid;
    auto relay = [&hub, key](std::string_view name, const PackedArgs& args) { hub.fire(*key, name, args); };
    HumanoidEvents& e = humanoid->events;
    std::vector<Connection> connections;
    auto speed = [&](Signal<float>& signal, std::string_view name) {
        connections.push_back(
            signal.connect([relay, name](float value) { relay(name, {packed(Value(static_cast<double>(value)))}); }));
    };
    auto active = [&](Signal<bool>& signal, std::string_view name) {
        connections.push_back(signal.connect([relay, name](bool value) { relay(name, {packed(Value(value))}); }));
    };
    speed(e.running, "Running");
    speed(e.climbing, "Climbing");
    speed(e.swimming, "Swimming");
    active(e.jumping, "Jumping");
    active(e.freeFalling, "FreeFalling");
    active(e.fallingDown, "FallingDown");
    active(e.gettingUp, "GettingUp");
    active(e.ragdoll, "Ragdoll");
    active(e.platformStanding, "PlatformStanding");
    active(e.strafing, "Strafing");
    active(e.moveToFinished, "MoveToFinished");
    connections.push_back(e.seated.connect([relay](bool value, BasePart* seat) {
        relay("Seated", {packed(Value(value)), packed(seat ? Value(seat->shared_from_this()) : Value())});
    }));
    connections.push_back(e.died.connect([relay] { relay("Died", {}); }));
    connections.push_back(e.stateChanged.connect([relay](Enum::HumanoidStateType old, Enum::HumanoidStateType now) {
        relay("StateChanged", {packed(stateValue(old)), packed(stateValue(now))});
    }));
    // StateEnabledChanged(state, isEnabled): the engine signal has them the other way round.
    connections.push_back(e.stateEnabledChanged.connect([relay](bool enabled, Enum::HumanoidStateType state) {
        relay("StateEnabledChanged", {packed(stateValue(state)), packed(Value(enabled))});
    }));
    // HealthChanged follows the property itself, so it fires whether or not
    // anything simulates the Humanoid (a server script's TakeDamage on an NPC).
    // A Humanoid nothing simulates also dies here, once, when its health
    // runs out; a driven one dies through its state machine instead.
    // GUESS: the client's server-side state machine would take NPCs to Dead
    // on its next step; firing at once is close enough without one.
    connections.push_back(humanoid->changed.connect([relay, humanoid, died = false](std::string_view property) mutable {
        if (property != "Health")
            return;
        relay("HealthChanged", {packed(Value(static_cast<double>(humanoid->health())))});
        if (!died && humanoid->health() <= 0.0f && !humanoid->driver()) {
            died = true;
            relay("Died", {});
        }
    }));
    return connections;
}

namespace {

Humanoid& selfHumanoid(lua_State* L)
{
    auto* humanoid = dynamic_cast<Humanoid*>(&self(L));
    if (!humanoid)
        expectedColon(L);
    return *humanoid;
}

Enum::HumanoidStateType checkState(lua_State* L, int idx)
{
    std::optional<EnumItem> item = toEnumItem(L, idx, "HumanoidStateType");
    if (!item)
        luaL_typeerror(L, idx, "EnumItem");
    return static_cast<Enum::HumanoidStateType>(item->value);
}

int humanoidGetState(lua_State* L)
{
    Humanoid& humanoid = selfHumanoid(L);
    Enum::HumanoidStateType state = humanoid.getState();
    // GUESS: a Humanoid nothing has simulated yet (an NPC without a
    // controller, a character before its first step) reads as Running, or
    // Dead without health, which is what scripts checking it expect.
    if (state == Enum::HumanoidStateType::None)
        state = humanoid.health() <= 0.0f ? Enum::HumanoidStateType::Dead : Enum::HumanoidStateType::Running;
    pushEnumItem(L, enums::item(state));
    return 1;
}

// ChangeState only means something to whoever simulates the Humanoid; on
// one nobody drives it does nothing, like a ChangeState on a Humanoid the
// calling machine doesn't own.
int humanoidChangeState(lua_State* L)
{
    Humanoid& humanoid = selfHumanoid(L);
    const Enum::HumanoidStateType state = checkState(L, 2);
    if (HumanoidDriver* driver = humanoid.driver())
        driver->changeState(state);
    return 0;
}

int humanoidSetStateEnabled(lua_State* L)
{
    Humanoid& humanoid = selfHumanoid(L);
    const Enum::HumanoidStateType state = checkState(L, 2);
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    humanoid.setStateEnabled(state, lua_toboolean(L, 3) != 0);
    return 0;
}

int humanoidGetStateEnabled(lua_State* L)
{
    Humanoid& humanoid = selfHumanoid(L);
    lua_pushboolean(L, humanoid.stateEnabled(checkState(L, 2)));
    return 1;
}

int humanoidMove(lua_State* L)
{
    Humanoid& humanoid = selfHumanoid(L);
    const Vector3 direction = checkVector3(L, 2);
    const bool relativeToCamera = lua_toboolean(L, 3) != 0;
    if (HumanoidDriver* driver = humanoid.driver())
        driver->move(direction, relativeToCamera);
    return 0;
}

// Player:Move(direction, relativeToCamera): its character's Humanoid:Move
// (the PlayerModule's controls call it every frame); nothing without one.
int playerMove(lua_State* L)
{
    auto* player = dynamic_cast<Player*>(&self(L));
    const Vector3 direction = checkVector3(L, 2);
    const bool relativeToCamera = lua_toboolean(L, 3) != 0;
    Model* character = player ? player->character() : nullptr;
    auto* humanoid = character ? dynamic_cast<Humanoid*>(character->findFirstChildOfClass("Humanoid")) : nullptr;
    if (HumanoidDriver* driver = humanoid ? humanoid->driver() : nullptr)
        driver->move(direction, relativeToCamera);
    return 0;
}

int humanoidMoveTo(lua_State* L)
{
    Humanoid& humanoid = selfHumanoid(L);
    const Vector3 point = checkVector3(L, 2);
    auto* part = dynamic_cast<BasePart*>(toInstance(L, 3));
    if (HumanoidDriver* driver = humanoid.driver()) {
        driver->moveTo(point, part);
    } else {
        humanoid.setWalkToPoint(point);
        humanoid.setWalkToPart(part);
    }
    return 0;
}

constexpr auto methods = std::to_array<Method>({
    {"GetState", "Humanoid", humanoidGetState},
    {"ChangeState", "Humanoid", humanoidChangeState},
    {"SetStateEnabled", "Humanoid", humanoidSetStateEnabled},
    {"GetStateEnabled", "Humanoid", humanoidGetStateEnabled},
    {"Move", "Humanoid", humanoidMove},
    {"Move", "Player", playerMove},
    {"MoveTo", "Humanoid", humanoidMoveTo},
});

} // namespace

std::span<const Method> humanoidMethods()
{
    return methods;
}

} // namespace lunar::lua

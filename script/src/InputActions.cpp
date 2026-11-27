#include "InstanceApi.hpp"
#include "Services.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lunar::lua {
namespace {

Enum::KeyCode mouseKey(Enum::UserInputType type)
{
    using enum Enum::UserInputType;
    switch (type) {
    case MouseButton1:
        return Enum::KeyCode::MouseLeftButton;
    case MouseButton2:
        return Enum::KeyCode::MouseRightButton;
    case MouseButton3:
        return Enum::KeyCode::MouseMiddleButton;
    case MouseWheel:
        return Enum::KeyCode::MouseWheel;
    case MouseMovement:
        return Enum::KeyCode::MousePosition;
    default:
        return Enum::KeyCode::None;
    }
}

bool held(const InputState& input, Enum::KeyCode key)
{
    if (key == Enum::KeyCode::None)
        return false;
    for (const auto type :
         {Enum::UserInputType::MouseButton1, Enum::UserInputType::MouseButton2, Enum::UserInputType::MouseButton3})
        if (mouseKey(type) == key)
            return input.buttons.contains(type) && !input.processedButtons.contains(type);
    return input.keys.contains(key) && !input.processedKeys.contains(key);
}

bool matches(const InputBinding& binding, Enum::KeyCode key)
{
    if (key == Enum::KeyCode::None)
        return false;
    return binding.keyCode() == key || binding.primaryModifier() == key || binding.secondaryModifier() == key
           || binding.left() == key || binding.right() == key || binding.up() == key || binding.down() == key
           || binding.forward() == key || binding.backward() == key;
}

bool inClientTree(const InputAction& action, const DataModel& game)
{
    const auto* players = instanceCast<const Players>(game.findService("Players"));
    const Player* local = players ? players->localPlayer() : nullptr;
    for (const Instance* ancestor = action.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->isA("ReplicatedStorage") || ancestor->isA("StarterPlayer") || ancestor->isA("StarterGui")
            || ancestor->isA("ServerStorage") || ancestor->isA("ServerScriptService"))
            return false;
        if (const auto* player = instanceCast<const Player>(ancestor); player && player != local)
            return false;
        if (ancestor == &game)
            return true;
    }
    return false;
}

Value bindingState(const InputBinding& binding, const InputAction& action, const InputState& input, Vector2 position)
{
    const auto down = [&](Enum::KeyCode key) { return held(input, key); };
    const bool modified = (binding.primaryModifier() == Enum::KeyCode::None || down(binding.primaryModifier()))
                          && (binding.secondaryModifier() == Enum::KeyCode::None || down(binding.secondaryModifier()));
    const float x = modified ? static_cast<float>(down(binding.right())) - static_cast<float>(down(binding.left())) : 0;
    const float y = modified ? static_cast<float>(down(binding.up())) - static_cast<float>(down(binding.down())) : 0;
    const float z
        = modified ? static_cast<float>(down(binding.backward())) - static_cast<float>(down(binding.forward())) : 0;
    switch (action.type()) {
    case Enum::InputActionType::Bool:
        return modified && down(binding.keyCode());
    case Enum::InputActionType::Direction1D:
        return static_cast<double>(y * binding.scale());
    case Enum::InputActionType::Direction2D: {
        Vector2 value{x * binding.vector2Scale().x, y * binding.vector2Scale().y};
        const float magnitude = value.magnitude();
        if (binding.clampMagnitudeToOne() && magnitude > 1)
            value = value / magnitude;
        return value;
    }
    case Enum::InputActionType::Direction3D: {
        Vector3 value{x * binding.vector3Scale().x, y * binding.vector3Scale().y, z * binding.vector3Scale().z};
        const float magnitude = value.magnitude();
        if (binding.clampMagnitudeToOne() && magnitude > 1)
            value = value / magnitude;
        return value;
    }
    case Enum::InputActionType::ViewportPosition:
        return position;
    }
    return false;
}

int fire(lua_State* L)
{
    Instance& object = self(L);
    InputAction* action = instanceCast<InputAction>(&object);
    if (auto* binding = instanceCast<InputBinding>(&object)) {
        if (binding->type() != Enum::InputBindingType::Scriptable)
            raise(L, "InputBinding:Fire requires Type Scriptable");
        action = binding->action();
    }
    if (!action)
        return 0;
    const auto value = toValue(L, 2);
    if (!value)
        raise(L, "Invalid input action state");
    const bool valid = action->type() == Enum::InputActionType::Bool          ? std::holds_alternative<bool>(*value)
                       : action->type() == Enum::InputActionType::Direction1D ? std::holds_alternative<double>(*value)
                       : action->type() == Enum::InputActionType::Direction3D ? std::holds_alternative<Vector3>(*value)
                                                                              : std::holds_alternative<Vector2>(*value);
    if (!valid)
        raise(L, "InputAction state does not match its Type");
    if (action->active())
        guard(L, [&] { action->assignState(*value); });
    return 0;
}

int getState(lua_State* L)
{
    pushValue(L, static_cast<InputAction&>(self(L)).state());
    return 1;
}

template <bool Preferred>
int bindings(lua_State* L)
{
    auto& action = static_cast<InputAction&>(self(L));
    const auto found = Preferred ? action.preferredBindings() : action.bindings();
    lua_createtable(L, static_cast<int>(found.size()), 0);
    for (std::size_t i = 0; i < found.size(); ++i) {
        pushInstance(L, found[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int actions(lua_State* L)
{
    const auto found = static_cast<InputContext&>(self(L)).actions();
    lua_createtable(L, static_cast<int>(found.size()), 0);
    for (std::size_t i = 0; i < found.size(); ++i) {
        pushInstance(L, found[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

} // namespace

InputActionState::~InputActionState()
{
    added.disconnect();
    for (auto& button : buttons)
        button.events.disconnect();
}

void refreshInputActions(Shared& shared, DataModel& game)
{
    auto& state = shared.state->inputActions;
    if (!state.initialized) {
        state.initialized = true;
        auto add = [&state](Instance* instance) {
            if (instanceCast<InputAction>(instance)
                && !std::ranges::any_of(state.actions, [&](const auto& weak) { return weak.lock().get() == instance; }))
                state.actions.push_back(instance->weak_from_this());
        };
        for (const auto& instance : game.descendants())
            add(instance.get());
        state.added = game.descendantAdded.connect(add);
    }
    std::erase_if(state.actions, [](const auto& weak) {
        auto ref = weak.lock();
        return !ref || ref->destroyed();
    });
    std::erase_if(state.buttons, [](auto& entry) {
        auto binding = entry.binding.lock();
        if (auto* b = instanceCast<InputBinding>(binding.get());
            b && b->uiButton() == entry.button.lock().get() && !b->destroyed())
            return false;
        entry.events.disconnect();
        return true;
    });
    for (const auto& weak : state.actions) {
        auto ref = weak.lock();
        auto* action = instanceCast<InputAction>(ref.get());
        if (!action)
            continue;
        if (!action->active() || !inClientTree(*action, game)) {
            action->resetState();
            continue;
        }
        for (InputBinding* binding : action->bindings()) {
            GuiButton* button = binding->uiButton();
            if (!button || binding->type() != Enum::InputBindingType::Automatic)
                continue;
            if (std::ranges::any_of(state.buttons,
                                    [&](const auto& entry) { return entry.binding.lock().get() == binding; }))
                continue;
            auto connection
                = button->eventFired.connect([owner = action->weak_from_this(), source = binding->weak_from_this()](
                                                 std::string_view event, std::span<const Value>) {
                      auto actionRef = owner.lock();
                      auto bindingRef = source.lock();
                      auto* input = instanceCast<InputAction>(actionRef.get());
                      auto* sourceBinding = instanceCast<InputBinding>(bindingRef.get());
                      if (!input || !sourceBinding || !input->active()
                          || sourceBinding->type() != Enum::InputBindingType::Automatic
                          || input->type() != Enum::InputActionType::Bool)
                          return;
                      if (event == "MouseButton1Down")
                          input->assignState(true);
                      else if (event == "MouseButton1Up")
                          input->assignState(false);
                  });
            state.buttons.push_back({binding->weak_from_this(), button->weak_from_this(), std::move(connection)});
        }
    }
}

bool runInputActions(Shared& shared, Enum::KeyCode key, Enum::UserInputType type, Enum::UserInputState phase,
                     Vector2 position, bool processed)
{
    if (key == Enum::KeyCode::None)
        key = mouseKey(type);
    struct Candidate {
        std::shared_ptr<Instance> ref;
        int priority;
        bool sink;
    };
    std::vector<Candidate> candidates;
    for (const auto& weak : shared.state->inputActions.actions) {
        auto ref = weak.lock();
        auto* action = instanceCast<InputAction>(ref.get());
        if (!action || !action->active() || !inClientTree(*action, shared.runtimes.front()->game()))
            continue;
        auto* context = action->context();
        candidates.push_back({std::move(ref), context ? context->priority() : 0, context && context->sink()});
    }
    std::ranges::stable_sort(candidates, std::greater{}, &Candidate::priority);
    int sinkPriority = std::numeric_limits<int>::min();
    bool sunk = false;
    for (const auto& candidate : candidates) {
        auto& action = static_cast<InputAction&>(*candidate.ref);
        const auto bindings = action.bindings();
        const bool matched = std::ranges::any_of(bindings, [&](InputBinding* binding) {
            return binding->type() == Enum::InputBindingType::Automatic && matches(*binding, key);
        });
        if (!matched)
            continue;
        std::optional<Value> result;
        for (InputBinding* binding : bindings) {
            if (binding->type() != Enum::InputBindingType::Automatic)
                continue;
            const bool suppressed = (sunk && candidate.priority < sinkPriority) || processed;
            if (suppressed && phase != Enum::UserInputState::End)
                continue;
            Value value = bindingState(*binding, action, shared.state->input, position);
            if (suppressed) {
                action.resetState();
                continue;
            }
            if (!result)
                result = value;
            else if (auto* boolean = std::get_if<bool>(&*result))
                *boolean = *boolean || std::get<bool>(value);
            else if (auto* axis = std::get_if<double>(&*result)) {
                if (std::abs(std::get<double>(value)) > std::abs(*axis))
                    *axis = std::get<double>(value);
            }
        }
        if (result)
            action.assignState(*result);
        if (matched && candidate.sink && !processed) {
            sunk = true;
            sinkPriority = candidate.priority;
        }
    }
    return sunk;
}

std::span<const Method> inputActionMethods()
{
    static constexpr Method methods[] = {
        {"Fire", "InputAction", fire},
        {"GetState", "InputAction", getState},
        {"GetInputBindings", "InputAction", bindings<false>},
        {"GetPreferredBindingList", "InputAction", bindings<true>},
        {"Fire", "InputBinding", fire},
        {"GetInputActions", "InputContext", actions},
    };
    return methods;
}

} // namespace lunar::lua

#include <lunar/Classes.hpp>

#include <array>
#include <cmath>

namespace lunar {

std::string inputKeyText(Enum::KeyCode code)
{
    switch (code) {
    case Enum::KeyCode::None:
        return {};
    case Enum::KeyCode::LeftControl:
    case Enum::KeyCode::RightControl:
        return "Ctrl";
    case Enum::KeyCode::LeftShift:
    case Enum::KeyCode::RightShift:
        return "Shift";
    case Enum::KeyCode::LeftAlt:
    case Enum::KeyCode::RightAlt:
        return "Alt";
    case Enum::KeyCode::Return:
        return "Enter";
    case Enum::KeyCode::Escape:
        return "Esc";
    default:
        break;
    }
    const auto number = static_cast<std::uint32_t>(code);
    if (number >= 48 && number <= 57)
        return std::string(1, static_cast<char>(number));
    return std::string(enums::item(code).name);
}

InputAction* InputBinding::action() const
{
    for (Instance* ancestor = parent(); ancestor; ancestor = ancestor->parent())
        if (auto* input = instanceCast<InputAction>(ancestor))
            return input;
    return nullptr;
}

std::string InputBinding::displayText() const
{
    if (!displayName().empty())
        return displayName();
    std::string text;
    auto add = [&](Enum::KeyCode key) {
        const std::string name = inputKeyText(key);
        if (name.empty())
            return;
        if (!text.empty())
            text += " + ";
        text += name;
    };
    add(primaryModifier());
    add(secondaryModifier());
    if (keyCode() != Enum::KeyCode::None)
        add(keyCode());
    else {
        for (const auto key : {up(), left(), down(), right(), forward(), backward()})
            add(key);
    }
    return text;
}

std::vector<InputBinding*> InputAction::bindings() const
{
    std::vector<InputBinding*> found;
    for (const auto& child : children())
        if (auto* binding = instanceCast<InputBinding>(child.get()))
            found.push_back(binding);
    return found;
}

std::vector<InputBinding*> InputAction::preferredBindings() const
{
    std::vector<InputBinding*> found;
    for (InputBinding* binding : bindings()) {
        const auto desktop = [](Enum::KeyCode code) {
            const auto key = static_cast<std::uint32_t>(code);
            return (key > 0 && key < 1000) || (key >= 1026 && key <= 1033) || key == 1035 || key == 1048;
        };
        bool composite = false;
        for (const auto key : {binding->left(), binding->right(), binding->up(), binding->down(), binding->forward(),
                               binding->backward()})
            composite = composite || desktop(key);
        const bool custom = binding->keyCode() == Enum::KeyCode::None
                            && (!binding->displayName().empty() || !binding->displayImage().empty());
        if (desktop(binding->keyCode()) || composite || custom)
            found.push_back(binding);
    }
    return found;
}

InputBinding* InputAction::preferredBinding() const
{
    const auto preferred = preferredBindings();
    return preferred.empty() ? nullptr : preferred.front();
}

InputContext* InputAction::context() const
{
    for (Instance* ancestor = parent(); ancestor; ancestor = ancestor->parent())
        if (auto* context = instanceCast<InputContext>(ancestor))
            return context;
    return nullptr;
}

bool InputAction::active() const
{
    if (!enabled_ || destroyed())
        return false;
    const InputContext* owner = context();
    if (!owner)
        return true;
    if (!owner->enabled())
        return false;
    return owner->findFirstAncestorWhichIsA("InputContext") == nullptr;
}

Value InputAction::state() const
{
    switch (type_) {
    case Enum::InputActionType::Bool:
        return state_.boolean;
    case Enum::InputActionType::Direction1D:
        return state_.axis;
    case Enum::InputActionType::Direction2D:
        return state_.vector2;
    case Enum::InputActionType::Direction3D:
        return state_.vector3;
    case Enum::InputActionType::ViewportPosition:
        return state_.viewport;
    }
    return false;
}

void InputAction::assignState(const Value& value)
{
    const Value previous = state();
    if (previous == value)
        return;
    switch (type_) {
    case Enum::InputActionType::Bool:
        if (const auto* v = std::get_if<bool>(&value))
            setProperty(state_.boolean, *v, "BoolState");
        else
            throw Error("InputAction expects a boolean state");
        break;
    case Enum::InputActionType::Direction1D:
        if (const auto* v = std::get_if<double>(&value); v && std::isfinite(*v))
            setProperty(state_.axis, *v, "Direction1DState");
        else
            throw Error("InputAction expects a finite number state");
        break;
    case Enum::InputActionType::Direction2D:
    case Enum::InputActionType::ViewportPosition:
        if (const auto* v = std::get_if<Vector2>(&value); v && std::isfinite(v->x) && std::isfinite(v->y)) {
            if (type_ == Enum::InputActionType::Direction2D)
                setProperty(state_.vector2, *v, "Direction2DState");
            else
                setProperty(state_.viewport, *v, "ViewportPositionState");
        } else
            throw Error("InputAction expects a finite Vector2 state");
        break;
    case Enum::InputActionType::Direction3D:
        if (const auto* v = std::get_if<Vector3>(&value);
            v && std::isfinite(v->x) && std::isfinite(v->y) && std::isfinite(v->z))
            setProperty(state_.vector3, *v, "Direction3DState");
        else
            throw Error("InputAction expects a finite Vector3 state");
        break;
    }
    const std::array<Value, 1> args{value};
    fireEvent("StateChanged", args);
    if (type_ == Enum::InputActionType::Bool)
        fireEvent(state_.boolean ? "Pressed" : "Released");
}

void InputAction::resetState()
{
    switch (type_) {
    case Enum::InputActionType::Bool:
        assignState(false);
        break;
    case Enum::InputActionType::Direction1D:
        assignState(0.0);
        break;
    case Enum::InputActionType::Direction2D:
    case Enum::InputActionType::ViewportPosition:
        assignState(Vector2{});
        break;
    case Enum::InputActionType::Direction3D:
        assignState(Vector3{});
        break;
    }
}

void InputAction::setEnabled(bool enabled)
{
    setProperty(enabled_, enabled, "Enabled");
    if (!enabled)
        resetState();
}

void InputAction::setType(Enum::InputActionType type)
{
    if (type_ == type)
        return;
    resetState();
    setProperty(type_, type, "Type");
    state_ = State{};
}

std::vector<InputAction*> InputContext::actions() const
{
    std::vector<InputAction*> found;
    for (const auto& descendant : descendants())
        if (auto* action = instanceCast<InputAction>(descendant.get()); action && action->context() == this)
            found.push_back(action);
    return found;
}

void InputContext::setEnabled(bool enabled)
{
    setProperty(enabled_, enabled, "Enabled");
    if (!enabled)
        for (InputAction* action : actions())
            action->resetState();
}

const std::string& InputActionLabel::resolvedText() const
{
    InputBinding* binding = inputAction() ? inputAction()->preferredBinding() : nullptr;
    const std::string next = binding ? binding->displayText() : "?";
    if (text_ != next) {
        text_ = next;
        changed.fire("ResolvedText");
    }
    return text_;
}

const std::string& InputActionLabel::resolvedImageContent() const
{
    InputBinding* binding = inputAction() ? inputAction()->preferredBinding() : nullptr;
    const std::string next = binding ? binding->displayImage() : std::string();
    if (image_ != next) {
        image_ = next;
        changed.fire("ResolvedImageContent");
    }
    return image_;
}

} // namespace lunar

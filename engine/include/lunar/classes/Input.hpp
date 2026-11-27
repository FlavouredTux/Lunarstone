#pragma once

#include <lunar/classes/Gui.hpp>

#include <optional>
#include <vector>

namespace lunar {

class InputAction;
class InputContext;

class InputBinding final : public InstanceImpl<InputBinding, Instance> {
public:
    LUNAR_FIELD(Enum::InputBindingType, type, Type, Enum::InputBindingType::Automatic)
    LUNAR_FIELD(Enum::KeyCode, keyCode, KeyCode, Enum::KeyCode::None)
    LUNAR_FIELD(Enum::KeyCode, primaryModifier, PrimaryModifier, Enum::KeyCode::None)
    LUNAR_FIELD(Enum::KeyCode, secondaryModifier, SecondaryModifier, Enum::KeyCode::None)
    LUNAR_FIELD(Enum::KeyCode, left, Left, Enum::KeyCode::None)
    LUNAR_FIELD(Enum::KeyCode, right, Right, Enum::KeyCode::None)
    LUNAR_FIELD(Enum::KeyCode, up, Up, Enum::KeyCode::None)
    LUNAR_FIELD(Enum::KeyCode, down, Down, Enum::KeyCode::None)
    LUNAR_FIELD(Enum::KeyCode, forward, Forward, Enum::KeyCode::None)
    LUNAR_FIELD(Enum::KeyCode, backward, Backward, Enum::KeyCode::None)
    LUNAR_FIELD(bool, clampMagnitudeToOne, ClampMagnitudeToOne, true)
    LUNAR_FIELD(NumberRange, deadzone, Deadzone, NumberRange{0, 1})
    LUNAR_FIELD(float, scale, Scale, 1.0f)
    LUNAR_FIELD(Vector2, vector2Scale, Vector2Scale, Vector2(1, 1))
    LUNAR_FIELD(Vector3, vector3Scale, Vector3Scale, Vector3(1, 1, 1))
    LUNAR_FIELD(float, responseCurve, ResponseCurve, 1.0f)
    LUNAR_FIELD(float, pressedThreshold, PressedThreshold, 0.5f)
    LUNAR_FIELD(float, releasedThreshold, ReleasedThreshold, 0.5f)
    LUNAR_FIELD(int, pointerIndex, PointerIndex, 0)
    LUNAR_FIELD(std::string, displayName, DisplayName)
    LUNAR_FIELD(std::string, displayImage, DisplayImage)
    LUNAR_REF(GuiButton, uiButton, UIButton)
    LUNAR_REF(GuiButton, uiModifier, UIModifier)

    InputAction* action() const;
    std::string displayText() const;
};

class InputAction final : public InstanceImpl<InputAction, Instance> {
public:
    LUNAR_FIELD(std::string, displayName, DisplayName)
    LUNAR_PROPERTY() bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);
    LUNAR_PROPERTY() Enum::InputActionType type() const { return type_; }
    void setType(Enum::InputActionType type);
    LUNAR_PROPERTY() bool boolState() const { return state_.boolean; }
    LUNAR_PROPERTY() double direction1DState() const { return state_.axis; }
    LUNAR_PROPERTY() Vector2 direction2DState() const { return state_.vector2; }
    LUNAR_PROPERTY() Vector3 direction3DState() const { return state_.vector3; }
    LUNAR_PROPERTY() Vector2 viewportPositionState() const { return state_.viewport; }
    LUNAR_PROPERTY() InputBinding* preferredBinding() const;

    std::vector<InputBinding*> bindings() const;
    std::vector<InputBinding*> preferredBindings() const;
    InputContext* context() const;
    bool active() const;
    Value state() const;
    void assignState(const Value& state);
    void resetState();

private:
    struct State {
        State() = default;
        State(const State&) {}
        State& operator=(const State&) = default;
        bool boolean = false;
        double axis = 0;
        Vector2 vector2;
        Vector3 vector3;
        Vector2 viewport;
    };
    bool enabled_ = true;
    Enum::InputActionType type_ = Enum::InputActionType::Bool;
    State state_;
};

class InputContext final : public InstanceImpl<InputContext, Instance> {
public:
    LUNAR_PROPERTY() bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);
    LUNAR_FIELD(int, priority, Priority, 0)
    LUNAR_FIELD(bool, sink, Sink, false)
    std::vector<InputAction*> actions() const;

private:
    bool enabled_ = true;
};

// The label is a GuiObject, not a TextLabel. Its text/image are derived from
// the action's preferred binding, including custom names and modifiers.
class InputActionLabel final : public InstanceImpl<InputActionLabel, GuiObject> {
public:
    LUNAR_REF(InputAction, inputAction, InputAction)
    LUNAR_FIELD(Font, fontFace, FontFace)
    LUNAR_FIELD(Color3, textColor3, TextColor3, Color3(1, 1, 1))
    LUNAR_FIELD(float, textSize, TextSize, 14.0f)
    LUNAR_FIELD(float, textTransparency, TextTransparency, 0.0f)
    LUNAR_FIELD(bool, textWrapped, TextWrapped, false)
    LUNAR_FIELD(Enum::TextXAlignment, textXAlignment, TextXAlignment, Enum::TextXAlignment::Center)
    LUNAR_FIELD(Enum::TextYAlignment, textYAlignment, TextYAlignment, Enum::TextYAlignment::Center)
    LUNAR_FIELD(Color3, imageColor3, ImageColor3, Color3(1, 1, 1))
    LUNAR_FIELD(float, imageTransparency, ImageTransparency, 0.0f)
    LUNAR_PROPERTY() const std::string& resolvedText() const;
    LUNAR_PROPERTY() const std::string& resolvedImageContent() const;

private:
    mutable std::string text_;
    mutable std::string image_;
};

std::string inputKeyText(Enum::KeyCode code);

} // namespace lunar

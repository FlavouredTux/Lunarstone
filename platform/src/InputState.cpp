#include <lunar/platform/InputState.hpp>

#include <variant>

namespace lunar::platform {

namespace {

template <class... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};

} // namespace

void InputState::beginFrame()
{
    pressed_.reset();
    released_.reset();
    buttonsPressed_.reset();
    buttonsReleased_.reset();
    mouseDelta_ = {};
    scrollDelta_ = {};
    text_.clear();
}

void InputState::releaseKeys()
{
    released_ |= down_;
    down_.reset();
    modifiers_ = {};
}

void InputState::releaseButtons()
{
    buttonsReleased_ |= buttonsDown_;
    buttonsDown_.reset();
}

void InputState::apply(const Event& event)
{
    std::visit(Overloaded{
                   [this](const KeyEvent& e) {
                       modifiers_ = e.modifiers;
                       if (!valid(e.key) || e.key == Key::Unknown)
                           return;
                       std::size_t i = index(e.key);
                       if (e.pressed && !e.repeat && !down_[i])
                           pressed_[i] = true;
                       if (!e.pressed && down_[i])
                           released_[i] = true;
                       down_[i] = e.pressed;
                   },
                   [this](const MouseButtonEvent& e) {
                       mousePosition_ = {e.x, e.y};
                       if (!valid(e.button))
                           return;
                       std::size_t i = index(e.button);
                       if (e.pressed && !buttonsDown_[i])
                           buttonsPressed_[i] = true;
                       if (!e.pressed && buttonsDown_[i])
                           buttonsReleased_[i] = true;
                       buttonsDown_[i] = e.pressed;
                   },
                   [this](const ModifiersEvent& e) { modifiers_ = e.modifiers; },
                   [this](const TextEvent& e) { text_ += e.text; },
                   [this](const MouseMoveEvent& e) { mousePosition_ = {e.x, e.y}; },
                   [this](const ScrollEvent& e) {
                       scrollDelta_.x += e.dx;
                       scrollDelta_.y += e.dy;
                   },
                   [this](const RelativeMotionEvent& e) {
                       mouseDelta_.x += e.rawDx;
                       mouseDelta_.y += e.rawDy;
                   },
                   [this](const MouseEnterEvent& e) {
                       // Wayland keeps an implicit grab while a button is held,
                       // so leaving with buttons down means we lost them.
                       pointerInside_ = e.entered;
                       if (!e.entered)
                           releaseButtons();
                   },
                   [this](const FocusEvent& e) {
                       // Keys released while unfocused never reach us, so
                       // don't leave them stuck down.
                       focused_ = e.focused;
                       if (!e.focused)
                           releaseKeys();
                   },
                   [this](const PointerLockEvent& e) { pointerLocked_ = e.locked; },
                   [](const ResizeEvent&) {},
                   [](const CloseEvent&) {},
               },
               event);
}

} // namespace lunar::platform

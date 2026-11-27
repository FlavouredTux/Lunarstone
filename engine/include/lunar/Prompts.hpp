#pragma once

// What a client does with ProximityPrompts: which ones show, holding their
// key, and the events that follow. A prompt shows when it's enabled, its
// parent places it (a BasePart, an Attachment or a Model), it's on screen,
// within MaxActivationDistance of the local character (of the camera without
// one) and, with RequiresLineOfSight, nothing blocks the camera's view of it;
// Exclusivity then picks among those, nearest first, MaxPromptsVisible at
// most. Pressing a shown prompt's KeyboardKeyCode holds it; held for
// HoldDuration it triggers, and letting go ends the trigger.
//
// Each event fires on the prompt and on ProximityPromptService here, the
// client's side (Triggered for the local player), and the prompt's
// ...ActionReplicated twin goes to the server with our Player, which fires
// the server's [schema: ProximityPrompt.TriggeredActionReplicated(Object)
// and its siblings; the client binary keeps their names beside
// ClickDetector's MouseActionReplicated]. GUESS: the order of the events
// around a release (PromptButtonHoldEnded, then TriggerEnded).

#include <lunar/Classes.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace lunar {

// Where a prompt is: its BasePart's position, its Attachment's world
// position, or its Model's pivot. Nothing under any other parent.
std::optional<Vector3> promptPosition(const ProximityPrompt& prompt);

class PromptSystem {
public:
    // Where the client stands and looks this frame.
    struct View {
        Vector3 reference;         // the local character's root (the camera without one)
        CFrame camera;             // looking along its -Z
        float fieldOfView = 70.0f; // vertical, degrees
        Vector2 viewport{1280.0f, 720.0f};
    };
    // Whether something solid stands between two points; the parts of
    // `ignore` (the prompt's parent) don't count.
    using Blocked = std::function<bool(Vector3 from, Vector3 to, const Instance* ignore)>;
    // An event for the server: one of the prompt's ...ActionReplicated, with our Player.
    using Replicate = std::function<void(ProximityPrompt& prompt, std::string_view event, Player& player)>;

    explicit PromptSystem(DataModel& game);
    ~PromptSystem();
    PromptSystem(const PromptSystem&) = delete;
    PromptSystem& operator=(const PromptSystem&) = delete;

    // Works out which prompts show (PromptShown and PromptHidden fire for the
    // ones that change) and runs the holds on by dt; a hold whose prompt
    // went away ends.
    void step(double dt, const View& view, Player* player, const Blocked& blocked = {});

    // A key went down: the shown prompts with it as their KeyboardKeyCode
    // are held. True when one was (the key was the prompts').
    bool press(Enum::KeyCode key);
    // ...and up: their holds end.
    void release(Enum::KeyCode key);
    // ProximityPrompt:InputHoldBegin and InputHoldEnd, which a Custom
    // prompt's own buttons call. Only a shown prompt can be held.
    void holdBegin(ProximityPrompt& prompt);
    void holdEnd(ProximityPrompt& prompt);

    struct Shown {
        std::shared_ptr<ProximityPrompt> prompt;
        Vector3 position;
        float progress = 0.0f; // of its hold, 0 to 1
    };
    // The prompts on screen after the last step, nearest first.
    const std::vector<Shown>& shown() const { return shown_; }

    Replicate replicate;

private:
    struct Hold {
        std::weak_ptr<ProximityPrompt> prompt;
        double held = 0.0;
        bool triggered = false;
    };
    void fire(ProximityPrompt& prompt, std::string_view promptEvent, std::string_view serviceEvent,
              std::string_view replicated);
    void trigger(Hold& hold, ProximityPrompt& prompt);
    void end(Hold& hold, ProximityPrompt& prompt);
    bool isShown(const ProximityPrompt& prompt) const;

    DataModel& game_;
    std::vector<std::weak_ptr<ProximityPrompt>> prompts_; // every prompt in the game
    std::vector<Connection> connections_;
    std::vector<Shown> shown_;
    std::vector<Hold> holds_;
    std::weak_ptr<Player> player_; // whose prompts these are, as of the last step
};

} // namespace lunar

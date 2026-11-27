#include <lunar/Prompts.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace lunar {

namespace {

// Whether a point shows in the camera's picture.
bool onScreen(const PromptSystem::View& view, Vector3 point)
{
    const Vector3 local = view.camera.pointToObjectSpace(point);
    const float depth = -local.z;
    if (depth <= 0.0f)
        return false;
    const float tanHalf = std::tan(view.fieldOfView * std::numbers::pi_v<float> / 360.0f);
    const float aspect = view.viewport.y > 0.0f ? view.viewport.x / view.viewport.y : 16.0f / 9.0f;
    return std::abs(local.y) <= depth * tanHalf && std::abs(local.x) <= depth * tanHalf * aspect;
}

} // namespace

std::optional<Vector3> promptPosition(const ProximityPrompt& prompt)
{
    const Instance* parent = prompt.parent();
    if (const auto* part = instanceCast<const BasePart>(parent))
        return part->position();
    if (const auto* attachment = instanceCast<const Attachment>(parent))
        return attachment->worldPosition();
    // Workspace is a Model too, but places nothing.
    if (const auto* model = instanceCast<const Model>(parent); model && !instanceCast<const Workspace>(parent))
        return model->getPivot().position;
    return std::nullopt;
}

PromptSystem::PromptSystem(DataModel& game) : game_(game)
{
    for (const InstancePtr& d : game.descendants())
        if (auto prompt = std::dynamic_pointer_cast<ProximityPrompt>(d))
            prompts_.push_back(prompt);
    connections_.push_back(game.descendantAdded.connect([this](Instance* instance) {
        if (auto* prompt = instanceCast<ProximityPrompt>(instance))
            prompts_.push_back(std::static_pointer_cast<ProximityPrompt>(prompt->shared_from_this()));
    }));
}

PromptSystem::~PromptSystem()
{
    for (Connection& c : connections_)
        c.disconnect();
}

void PromptSystem::step(double dt, const View& view, Player* player, const Blocked& blocked)
{
    player_ = player ? std::static_pointer_cast<Player>(player->shared_from_this()) : std::weak_ptr<Player>{};
    std::erase_if(prompts_, [&](const std::weak_ptr<ProximityPrompt>& weak) {
        const auto prompt = weak.lock();
        return !prompt || !prompt->isDescendantOf(&game_);
    });

    const auto* service = instanceCast<const ProximityPromptService>(game_.findService("ProximityPromptService"));
    const Instance* workspace = game_.workspace();
    struct Candidate {
        std::shared_ptr<ProximityPrompt> prompt;
        Vector3 position;
        float distance = 0.0f;
    };
    std::vector<Candidate> candidates;
    if (player && workspace && (!service || service->enabled()))
        for (const auto& weak : prompts_) {
            auto prompt = weak.lock();
            if (!prompt || !prompt->enabled() || !prompt->isDescendantOf(workspace))
                continue;
            const std::optional<Vector3> at = promptPosition(*prompt);
            if (!at)
                continue;
            const float distance = (*at - view.reference).magnitude();
            if (distance > prompt->maxActivationDistance() || !onScreen(view, *at))
                continue;
            if (prompt->requiresLineOfSight() && blocked && blocked(view.camera.position, *at, prompt->parent()))
                continue;
            candidates.push_back({std::move(prompt), *at, distance});
        }
    std::ranges::stable_sort(candidates, std::less{}, &Candidate::distance);

    // Exclusivity: the nearest per key, the nearest of the OneGlobally ones,
    // and every AlwaysShow one; MaxPromptsVisible at most.
    const auto limit = static_cast<std::size_t>(std::max(service ? service->maxPromptsVisible() : 16, 0));
    std::vector<Shown> shown;
    std::vector<Enum::KeyCode> keysTaken;
    bool globalTaken = false;
    for (Candidate& c : candidates) {
        if (shown.size() >= limit)
            break;
        switch (c.prompt->exclusivity()) {
        case Enum::ProximityPromptExclusivity::OnePerButton:
            if (std::ranges::contains(keysTaken, c.prompt->keyboardKeyCode()))
                continue;
            keysTaken.push_back(c.prompt->keyboardKeyCode());
            break;
        case Enum::ProximityPromptExclusivity::OneGlobally:
            if (globalTaken)
                continue;
            globalTaken = true;
            break;
        case Enum::ProximityPromptExclusivity::AlwaysShow:
            break;
        }
        shown.push_back({std::move(c.prompt), c.position, 0.0f});
    }

    auto* promptService = game_.findService("ProximityPromptService");
    const Value keyboard(enums::item(Enum::ProximityPromptInputType::Keyboard));
    for (const Shown& old : shown_)
        if (std::ranges::none_of(shown, [&](const Shown& s) { return s.prompt == old.prompt; })) {
            old.prompt->fireEvent("PromptHidden");
            if (promptService) {
                const std::array args{Value(InstancePtr(old.prompt))};
                promptService->fireEvent("PromptHidden", args);
            }
        }
    const std::vector<Shown> before = std::exchange(shown_, std::move(shown));
    for (const Shown& now : shown_)
        if (std::ranges::none_of(before, [&](const Shown& s) { return s.prompt == now.prompt; })) {
            const std::array args{keyboard};
            now.prompt->fireEvent("PromptShown", args);
            if (promptService) {
                const std::array serviceArgs{Value(InstancePtr(now.prompt)), keyboard};
                promptService->fireEvent("PromptShown", serviceArgs);
            }
        }

    // The holds: a prompt that went away lets go; one held long enough triggers.
    for (std::size_t i = 0; i < holds_.size();) {
        Hold& hold = holds_[i];
        auto prompt = hold.prompt.lock();
        if (!prompt || !isShown(*prompt)) {
            if (prompt)
                end(hold, *prompt);
            holds_.erase(holds_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        hold.held += dt;
        if (!hold.triggered && hold.held >= static_cast<double>(prompt->holdDuration()))
            trigger(hold, *prompt);
        ++i;
    }
    for (Shown& s : shown_)
        for (const Hold& hold : holds_)
            if (hold.prompt.lock() == s.prompt)
                s.progress = s.prompt->holdDuration() > 0.0f
                                 ? static_cast<float>(std::min(hold.held / static_cast<double>(s.prompt->holdDuration()), 1.0))
                                 : 1.0f;
}

bool PromptSystem::isShown(const ProximityPrompt& prompt) const
{
    return std::ranges::any_of(shown_, [&](const Shown& s) { return s.prompt.get() == &prompt; });
}

bool PromptSystem::press(Enum::KeyCode key)
{
    bool taken = false;
    // A copy: holding may fire events whose scripts change what shows.
    const std::vector<Shown> shown = shown_;
    for (const Shown& s : shown)
        if (s.prompt->keyboardKeyCode() == key) {
            holdBegin(*s.prompt);
            taken = true;
        }
    return taken;
}

void PromptSystem::release(Enum::KeyCode key)
{
    std::vector<std::shared_ptr<ProximityPrompt>> released;
    for (const Hold& hold : holds_)
        if (auto prompt = hold.prompt.lock(); prompt && prompt->keyboardKeyCode() == key)
            released.push_back(std::move(prompt));
    for (const auto& prompt : released)
        holdEnd(*prompt);
}

void PromptSystem::holdBegin(ProximityPrompt& prompt)
{
    if (!isShown(prompt) || std::ranges::any_of(holds_, [&](const Hold& h) { return h.prompt.lock().get() == &prompt; }))
        return;
    Hold hold{std::static_pointer_cast<ProximityPrompt>(prompt.shared_from_this())};
    if (prompt.holdDuration() > 0.0f)
        fire(prompt, "PromptButtonHoldBegan", "PromptButtonHoldBegan", "ButtonHoldBeganActionReplicated");
    else
        trigger(hold, prompt);
    holds_.push_back(std::move(hold));
}

void PromptSystem::holdEnd(ProximityPrompt& prompt)
{
    const auto it = std::ranges::find_if(holds_, [&](const Hold& h) { return h.prompt.lock().get() == &prompt; });
    if (it == holds_.end())
        return;
    Hold hold = std::move(*it);
    holds_.erase(it);
    end(hold, prompt);
}

void PromptSystem::trigger(Hold& hold, ProximityPrompt& prompt)
{
    hold.triggered = true;
    fire(prompt, "Triggered", "PromptTriggered", "TriggeredActionReplicated");
}

void PromptSystem::end(Hold& hold, ProximityPrompt& prompt)
{
    if (prompt.holdDuration() > 0.0f)
        fire(prompt, "PromptButtonHoldEnded", "PromptButtonHoldEnded", "ButtonHoldEndedActionReplicated");
    if (hold.triggered)
        fire(prompt, "TriggerEnded", "PromptTriggerEnded", "TriggerEndedActionReplicated");
}

void PromptSystem::fire(ProximityPrompt& prompt, std::string_view promptEvent, std::string_view serviceEvent,
                        std::string_view replicated)
{
    const auto player = player_.lock();
    if (!player)
        return;
    const InstancePtr self = prompt.shared_from_this();
    const std::array args{Value(InstancePtr(player))};
    prompt.fireEvent(promptEvent, args);
    if (auto* service = game_.findService("ProximityPromptService")) {
        const std::array serviceArgs{Value(self), Value(InstancePtr(player))};
        service->fireEvent(serviceEvent, serviceArgs);
    }
    if (replicate)
        replicate(prompt, replicated, *player);
}

} // namespace lunar

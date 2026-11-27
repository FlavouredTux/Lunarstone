// ProximityPrompts on a client: the game's PromptSystem, stepped each
// frame from the camera and the local character, its events sent to a real
// server through the remote sink, and ProximityPrompt:InputHoldBegin and
// InputHoldEnd for the prompts games draw themselves.

#include "InstanceApi.hpp"
#include "Services.hpp"
#include "Userdata.hpp"

#include <lunar/Prompts.hpp>

#include <array>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <vector>

namespace lunar::lua {

namespace {

ProximityPrompt& checkPrompt(lua_State* L)
{
    return static_cast<ProximityPrompt&>(self(L));
}

int promptInputHoldBegin(lua_State* L)
{
    ProximityPrompt& prompt = checkPrompt(L);
    if (auto& prompts = Runtime::of(L).shared().prompts)
        guard(L, [&] { prompts->holdBegin(prompt); });
    return 0;
}

int promptInputHoldEnd(lua_State* L)
{
    ProximityPrompt& prompt = checkPrompt(L);
    if (auto& prompts = Runtime::of(L).shared().prompts)
        guard(L, [&] { prompts->holdEnd(prompt); });
    return 0;
}

constexpr auto methods = std::to_array<Method>({
    {"InputHoldBegin", "ProximityPrompt", promptInputHoldBegin},
    {"InputHoldEnd", "ProximityPrompt", promptInputHoldEnd},
});

// The local character's root part, where prompts are measured from.
const BasePart* characterRoot(const Player& player)
{
    const Model* character = player.character();
    if (!character)
        return nullptr;
    if (const auto* root = dynamic_cast<const BasePart*>(character->findFirstChild("HumanoidRootPart")))
        return root;
    return character->primaryPart();
}

} // namespace

std::span<const Method> promptMethods()
{
    return methods;
}

void startPrompts(Shared& shared, DataModel& game)
{
    if (shared.prompts)
        return;
    shared.prompts = std::make_unique<PromptSystem>(game);
    // A real server hears of it through the prompt's ...ActionReplicated
    // events; in Play Solo the server's scripts share the prompt itself.
    shared.prompts->replicate = [&shared, &game](ProximityPrompt& prompt, std::string_view event, Player& player) {
        if (shared.remoteSink)
            shared.remoteSink(prompt, event, {remoteValue(Value(player.shared_from_this()))});
        // research: where we were for each trigger, as the server can check
        // it: the root's and the head's distance to the prompt, and whether a
        // ray from the head reaches it (the game ignores some of ours).
        if (event != "TriggeredActionReplicated")
            return;
        const BasePart* root = characterRoot(player);
        const std::optional<Vector3> at = promptPosition(prompt);
        if (!root || !at)
            return;
        const auto* head = player.character() ? dynamic_cast<const BasePart*>(player.character()->findFirstChild("Head")) : nullptr;
        const Vector3 eye = head ? head->position() : root->position();
        std::string sight = "no raycast";
        if (shared.raycast) {
            RaycastQuery query{.origin = eye, .direction = *at - eye};
            if (prompt.parent())
                query.filter.push_back(prompt.parent()->shared_from_this());
            query.filter.push_back(player.character()->shared_from_this());
            const std::optional<RaycastHit> hit = shared.raycast(game, query);
            sight = hit && hit->distance < (*at - eye).magnitude() - 0.1f
                        ? std::format("blocked by {} at {:.1f}", hit->instance ? hit->instance->fullName() : "?", hit->distance)
                        : std::string("clear");
        }
        std::println(stderr, "prompt: {} triggered at {:.1f} studs from the root ({:.1f}, {:.1f}, {:.1f}), {:.1f} from the head, max {:.0f}; head sight {}",
                     prompt.fullName(), (*at - root->position()).magnitude(), root->position().x, root->position().y,
                     root->position().z, (*at - eye).magnitude(), prompt.maxActivationDistance(), sight);
    };
}

void stepPrompts(Shared& shared, DataModel& game, double dt)
{
    if (!shared.prompts)
        return;
    auto* players = dynamic_cast<Players*>(game.findFirstChildOfClass("Players"));
    Player* player = players ? players->localPlayer() : nullptr;
    Workspace* workspace = game.workspace();
    const Camera* camera = workspace ? workspace->currentCamera() : nullptr;
    if (!camera) {
        shared.prompts->step(dt, {}, nullptr);
        return;
    }
    PromptSystem::View view{
        .reference = camera->cframe().position,
        .camera = camera->cframe(),
        .fieldOfView = camera->fieldOfView(),
        .viewport = camera->viewportSize().x > 0.0f ? camera->viewportSize() : Vector2{1280.0f, 720.0f},
    };
    const BasePart* root = player ? characterRoot(*player) : nullptr;
    if (root)
        view.reference = root->position();
    // Line of sight: a ray from the camera that nothing but the prompt's
    // parent and our character stops before it gets there.
    auto blocked = [&](Vector3 from, Vector3 to, const Instance* ignore) {
        if (!shared.raycast)
            return false;
        RaycastQuery query{.origin = from, .direction = to - from};
        if (ignore)
            query.filter.push_back(std::const_pointer_cast<Instance>(ignore->shared_from_this()));
        if (player && player->character())
            query.filter.push_back(player->character()->shared_from_this());
        const std::optional<RaycastHit> hit = shared.raycast(game, query);
        return hit && hit->distance < (to - from).magnitude() - 0.1f;
    };
    shared.prompts->step(dt, view, player, blocked);
}

} // namespace lunar::lua

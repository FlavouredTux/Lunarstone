#include <lunar/ui/HealthBars.hpp>

#include <lunar/ui/CoreGui.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>

namespace lunar::ui {

std::optional<RectF> projectBox(const CFrame& view, float fieldOfView, Vector2 screen, const CFrame& box, Vector3 min,
                                Vector3 max)
{
    if (screen.x <= 0.0f || screen.y <= 0.0f)
        return std::nullopt;
    const float tanHalf = std::tan(fieldOfView * std::numbers::pi_v<float> / 360.0f);
    if (!(tanHalf > 0.0f))
        return std::nullopt;
    const float aspect = screen.x / screen.y;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 8; ++i) {
        const Vector3 corner{i & 1 ? max.x : min.x, i & 2 ? max.y : min.y, i & 4 ? max.z : min.z};
        const Vector3 c = view.pointToObjectSpace(box * corner);
        const float depth = -c.z;
        if (!(depth >= 0.1f)) // behind the camera, at its near plane, or NaN
            return std::nullopt;
        const float sx = (c.x / (depth * tanHalf * aspect) + 1.0f) * 0.5f * screen.x;
        const float sy = (1.0f - c.y / (depth * tanHalf)) * 0.5f * screen.y;
        x0 = std::min(x0, sx);
        x1 = std::max(x1, sx);
        y0 = std::min(y0, sy);
        y1 = std::max(y1, sy);
    }
    if (!std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) || !std::isfinite(y1))
        return std::nullopt;
    return RectF{x0, y0, x1 - x0, y1 - y0};
}

HealthBarRects healthBarRects(const RectF& character, float fraction, float pixelScale)
{
    const float k = pixelScale;
    const float outline = std::max(1.0f, std::round(k));
    const float inner = std::max(2.0f, std::round(3 * k));
    const float gap = std::round(3 * k);
    HealthBarRects r;
    r.track = {std::round(character.x - gap - inner - 2 * outline), std::round(character.y), inner + 2 * outline,
               std::max(std::round(character.h), 2 * outline + 1)};
    const float innerH = r.track.h - 2 * outline;
    const float filled = std::round(innerH * std::clamp(fraction, 0.0f, 1.0f));
    r.fill = {r.track.x + outline, r.track.y + outline + innerH - filled, inner, filled};
    return r;
}

float healthFraction(float health, float maxHealth)
{
    if (!(maxHealth > 0.0f) || !std::isfinite(maxHealth) || !(health > 0.0f))
        return 0.0f;
    return std::clamp(health / maxHealth, 0.0f, 1.0f);
}

namespace {

// R6's and R15's body parts, what a character's rectangle is made of.
constexpr std::array<std::string_view, 20> bodyParts{
    "Head",          "Torso",        "Left Arm",     "Right Arm",     "Left Leg",      "Right Leg",    "UpperTorso",
    "LowerTorso",    "LeftUpperArm", "LeftLowerArm", "LeftHand",      "RightUpperArm", "RightLowerArm", "RightHand",
    "LeftUpperLeg",  "LeftLowerLeg", "LeftFoot",     "RightUpperLeg", "RightLowerLeg", "RightFoot"};

} // namespace

void HealthBars::refresh(Tracked& tracked, Model* character, bool accessories)
{
    if (tracked.character.get() != character) {
        tracked.character.reset(character);
        tracked.humanoid.reset(nullptr);
        tracked.root.reset(nullptr);
        tracked.parts.clear();
        tracked.children = static_cast<std::size_t>(-1);
    }
    if (!character)
        return;
    // Looked up again only when missing or moved out of the character: a
    // new character (a respawn) starts over above.
    if (const Humanoid* humanoid = tracked.humanoid.get(); !humanoid || humanoid->parent() != character)
        tracked.humanoid.reset(instanceCast<Humanoid>(character->findFirstChildOfClass("Humanoid")));
    if (const BasePart* root = tracked.root.get(); !root || root->parent() != character)
        tracked.root.reset(instanceCast<BasePart>(character->findFirstChild("HumanoidRootPart")));
    // The body's parts, again whenever something comes or goes.
    const auto& children = character->children();
    if (children.size() == tracked.children)
        return;
    tracked.children = children.size();
    tracked.parts.clear();
    for (const InstancePtr& child : children) {
        if (auto* part = instanceCast<BasePart>(child.get())) {
            // Not the root, which stays the torso's size and place wherever
            // the animation has thrown the body, nor effects parented here.
            if (std::ranges::find(bodyParts, std::string_view(part->name())) != bodyParts.end())
                tracked.parts.emplace_back().reset(part);
        } else if (accessories && child->isA("Accoutrement")) {
            if (auto* handle = instanceCast<BasePart>(child->findFirstChild("Handle")))
                tracked.parts.emplace_back().reset(handle);
        }
    }
}

std::optional<RectF> HealthBars::characterRect(const Tracked& tracked, const Humanoid& humanoid, const BasePart& root,
                                               const CFrame& view, float fieldOfView, Vector2 screen,
                                               Vector3& centre) const
{
    std::optional<RectF> rect;
    Vector3 sum;
    int count = 0;
    for (const Ref<BasePart>& ref : tracked.parts) {
        const BasePart* part = ref.get();
        if (!part)
            continue;
        const Vector3 half = part->size() * 0.5f;
        const auto r = projectBox(view, fieldOfView, screen, part->cframe(), -half, half);
        if (!r)
            return std::nullopt; // part of it behind the camera
        if (rect) {
            const float x0 = std::min(rect->x, r->x), y0 = std::min(rect->y, r->y);
            rect = RectF{x0, y0, std::max(rect->right(), r->right()) - x0, std::max(rect->bottom(), r->bottom()) - y0};
        } else {
            rect = r;
        }
        sum = sum + part->position();
        ++count;
    }
    if (count > 0) {
        centre = sum * (1.0f / static_cast<float>(count));
        return rect;
    }
    // No body to go by: a box around the root part, R6's arms, head and legs
    // around the torso, R15's the same from its hip height.
    const Vector3 size = root.size();
    const float unit = size.y / 2;
    const bool r6 = humanoid.rigType() == Enum::HumanoidRigType::R6;
    const float halfX = r6 ? 2 * unit : 1.6f * unit;
    const float legs = r6 ? 2 * unit : humanoid.hipHeight();
    const float head = r6 ? 1 * unit : 1.2f * unit;
    centre = root.position();
    return projectBox(view, fieldOfView, screen, root.cframe(), {-halfX, -unit - legs, -size.z / 2},
                      {halfX, unit + head, size.z / 2});
}

void HealthBars::update(DataModel* game, Vector2 screen, float pixelScale)
{
    ++frame_;
    bars_.clear();
    Workspace* workspace = game ? game->workspace() : nullptr;
    const Camera* camera = workspace ? workspace->currentCamera() : nullptr;
    auto* players = game ? instanceCast<Players>(game->findService("Players")) : nullptr;
    if (!camera || !players) {
        tracked_.clear();
        return;
    }
    const float k = pixelScale;
    const CFrame view = camera->cframe();
    const float fieldOfView = camera->fieldOfView();
    Player* local = players->localPlayer();
    auto track = [&](Player* player) -> Tracked& {
        Tracked& tracked = tracked_[player];
        if (tracked.player.get() != player) { // new, or another player where one used to be
            tracked = Tracked{};
            tracked.player.reset(player);
        }
        tracked.seen = frame_;
        refresh(tracked, player->character(), player == local);
        return tracked;
    };

    // Our own character gets no bar, and isn't in the way of the others': a
    // player just behind it (a teammate, the one we're fighting) still shows
    // theirs. Where on the screen it is, and how far its front is.
    std::optional<RectF> ours;
    float oursFront = 0.0f;
    if (local) {
        const Tracked& tracked = track(local);
        const Humanoid* humanoid = tracked.humanoid.get();
        const BasePart* root = tracked.root.get();
        Vector3 centre;
        if (humanoid && root)
            if ((ours = characterRect(tracked, *humanoid, *root, view, fieldOfView, screen, centre)))
                oursFront = -view.pointToObjectSpace(centre).z - 2.5f;
    }

    for (const InstancePtr& child : players->children()) {
        auto* player = instanceCast<Player>(child.get());
        if (!player || player == local)
            continue;
        Tracked& tracked = track(player);
        const Humanoid* humanoid = tracked.humanoid.get();
        const BasePart* root = tracked.root.get();
        if (!humanoid || !root || !(humanoid->health() > 0.0f))
            continue; // no character, or a dead one: no bar until the respawn
        Vector3 centre;
        const auto rect = characterRect(tracked, *humanoid, *root, view, fieldOfView, screen, centre);
        if (!rect || rect->h < minHeight * k || rect->right() < 0 || rect->x > screen.x || rect->bottom() < 0
            || rect->y > screen.y)
            continue;
        Bar bar;
        bar.player = player;
        bar.character = *rect;
        bar.fraction = healthFraction(humanoid->health(), humanoid->maxHealth());
        bar.rects = healthBarRects(*rect, bar.fraction, k);
        bar.color = healthColor(bar.fraction);
        bar.number = rect->h >= numberHeight * k;
        const float labelW = std::round(40 * k);
        bar.label = {bar.rects.track.x - std::round(3 * k) - labelW, bar.rects.track.y - std::round(2 * k), labelW,
                     std::round(16 * k)};
        // As deep as the character's front: a wall in front hides it, the
        // ground at its feet and its own swinging arms don't.
        const Vector3 toCamera = view.position - centre;
        const float distance = toCamera.magnitude();
        bar.origin = distance > 0.0f ? centre + toCamera * (std::min(2.5f, distance * 0.5f) / distance) : centre;
        // Behind our own character on the screen: drawn just in front of it
        // instead, which nothing between it and them can hide (rarely
        // anything: the camera keeps clear of walls in front of us).
        if (ours && oursFront > 0.5f) {
            const float depth = -view.pointToObjectSpace(bar.origin).z;
            RectF drawn = bar.rects.track;
            if (bar.number) {
                const float x0 = std::min(drawn.x, bar.label.x), y0 = std::min(drawn.y, bar.label.y);
                drawn = {x0, y0, std::max(drawn.right(), bar.label.right()) - x0,
                         std::max(drawn.bottom(), bar.label.bottom()) - y0};
            }
            const RectF overlap = drawn.intersect(*ours);
            if (depth > oursFront && overlap.w > 0 && overlap.h > 0)
                bar.origin = view.position + (bar.origin - view.position) * (oursFront / depth);
        }
        if (bar.number) {
            const int hp = static_cast<int>(std::min(std::ceil(humanoid->health()), 1e9f));
            if (tracked.hp != hp || tracked.scale != k) {
                ++layouts_;
                tracked.hp = hp;
                tracked.scale = k;
                const std::string text = std::format("{}", hp);
                TextParams params;
                params.text = text;
                params.style.font = fontFromEnum(Enum::Font::BuilderSansBold);
                params.style.size = 12 * k;
                params.style.color = {1, 1, 1};
                params.xAlignment = Enum::TextXAlignment::Right;
                params.yAlignment = Enum::TextYAlignment::Top;
                params.box = {labelW, std::round(14 * k)};
                tracked.text = layoutText(*fonts_, params);
            }
        }
        bars_.push_back(bar);
    }
    // Players that left take their bars with them.
    std::erase_if(tracked_, [this](const auto& item) { return item.second.seen != frame_; });
}

void HealthBars::draw(DrawList& list)
{
    for (const Bar& bar : bars_) {
        list.layers.push_back({.kind = WorldLayer::Kind::Billboard, .origin = bar.origin, .depthTested = true});
        const auto layer = static_cast<std::uint32_t>(list.layers.size());
        const std::size_t first = list.quads.size();
        Quad track;
        track.rect = bar.rects.track;
        track.color = {0.0f, 0.0f, 0.0f, 0.7f};
        list.push(track);
        if (bar.rects.fill.h > 0) {
            Quad fill;
            fill.rect = bar.rects.fill;
            fill.color = Color4::from(bar.color);
            list.push(fill);
        }
        if (bar.number)
            if (const auto found = tracked_.find(bar.player); found != tracked_.end())
                list.text(*atlas_, found->second.text, bar.label.x, bar.label.y, bar.label, {0, 0, 0, 0.85f});
        for (std::size_t i = first; i < list.quads.size(); ++i)
            list.quads[i].layer = layer;
    }
}

} // namespace lunar::ui

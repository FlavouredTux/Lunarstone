// What moves on its own between frames: tweens, input routed to
// ContextActionService, and characters spawning.

#include "Services.hpp"

#include <lunar/ClassRegistry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace lunar::lua {

namespace {

constexpr double pi = std::numbers::pi;

double bounceOut(double t)
{
    constexpr double n1 = 7.5625, d1 = 2.75;
    if (t < 1 / d1)
        return n1 * t * t;
    if (t < 2 / d1) {
        t -= 1.5 / d1;
        return n1 * t * t + 0.75;
    }
    if (t < 2.5 / d1) {
        t -= 2.25 / d1;
        return n1 * t * t + 0.9375;
    }
    t -= 2.625 / d1;
    return n1 * t * t + 0.984375;
}

// The "In" curve of each style; Out and InOut derive from it.
double easeIn(double t, std::string_view style)
{
    constexpr double c1 = 1.70158;
    if (style == "Linear")
        return t;
    if (style == "Sine")
        return 1 - std::cos(t * pi / 2);
    if (style == "Back")
        return (c1 + 1) * t * t * t - c1 * t * t;
    if (style == "Quad")
        return t * t;
    if (style == "Cubic")
        return t * t * t;
    if (style == "Quart")
        return t * t * t * t;
    if (style == "Quint")
        return t * t * t * t * t;
    if (style == "Bounce")
        return 1 - bounceOut(1 - t);
    if (style == "Elastic") {
        if (t <= 0 || t >= 1)
            return t <= 0 ? 0 : 1;
        return -std::pow(2.0, 10 * t - 10) * std::sin((t * 10 - 10.75) * (2 * pi / 3));
    }
    if (style == "Exponential")
        return t <= 0 ? 0 : std::pow(2.0, 10 * t - 10);
    if (style == "Circular")
        return 1 - std::sqrt(std::max(0.0, 1 - t * t));
    return t;
}

} // namespace

// The easings.net curves; Back's InOut uses the 1.525-scaled overshoot whose
// constant the client carries. (Guess: the other curves aren't checked
// against the client's own implementation.)
double ease(double alpha, std::string_view style, std::string_view direction)
{
    const double t = std::clamp(alpha, 0.0, 1.0);
    if (direction == "In")
        return easeIn(t, style);
    if (direction == "Out")
        return 1 - easeIn(1 - t, style);
    if (style == "Back") {
        constexpr double c2 = 1.70158 * 1.525;
        return t < 0.5 ? (std::pow(2 * t, 2) * ((c2 + 1) * 2 * t - c2)) / 2
                       : (std::pow(2 * t - 2, 2) * ((c2 + 1) * (t * 2 - 2) + c2) + 2) / 2;
    }
    return t < 0.5 ? easeIn(2 * t, style) / 2 : 1 - easeIn(2 - 2 * t, style) / 2;
}

bool tweenable(const Value& value)
{
    return std::holds_alternative<double>(value) || std::holds_alternative<bool>(value)
           || std::holds_alternative<Vector3>(value) || std::holds_alternative<Vector2>(value)
           || std::holds_alternative<Color3>(value) || std::holds_alternative<CFrame>(value)
           || std::holds_alternative<UDim>(value) || std::holds_alternative<UDim2>(value)
           || std::holds_alternative<Rect>(value) || std::holds_alternative<NumberRange>(value)
           || std::holds_alternative<EnumItem>(value) || std::holds_alternative<Vector3int16>(value);
}

namespace {

float mix(float a, float b, double t)
{
    return static_cast<float>(static_cast<double>(a) + (static_cast<double>(b) - static_cast<double>(a)) * t);
}

Vector3 mix(Vector3 a, Vector3 b, double t)
{
    return {mix(a.x, b.x, t), mix(a.y, b.y, t), mix(a.z, b.z, t)};
}

UDim mix(UDim a, UDim b, double t)
{
    return {mix(a.scale, b.scale, t),
            static_cast<std::int32_t>(std::lround(a.offset + (static_cast<double>(b.offset) - a.offset) * t))};
}

struct Quat {
    double w, x, y, z;
};

Quat toQuat(const CFrame& cf)
{
    double m[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i][j] = static_cast<double>(cf.r[i][j]);
    const double trace = m[0][0] + m[1][1] + m[2][2];
    Quat q{};
    if (trace > 0) {
        const double s = std::sqrt(trace + 1.0) * 2;
        q = {0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q = {(m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
    } else if (m[1][1] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q = {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s};
    } else {
        const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q = {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s};
    }
    return q;
}

CFrame mix(const CFrame& a, const CFrame& b, double t)
{
    Quat qa = toQuat(a), qb = toQuat(b);
    double dot = qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z;
    if (dot < 0) {
        qb = {-qb.w, -qb.x, -qb.y, -qb.z};
        dot = -dot;
    }
    double wa = 1 - t, wb = t;
    if (dot < 0.9995) {
        const double theta = std::acos(dot);
        wa = std::sin((1 - t) * theta) / std::sin(theta);
        wb = std::sin(t * theta) / std::sin(theta);
    }
    Quat q{qa.w * wa + qb.w * wb, qa.x * wa + qb.x * wb, qa.y * wa + qb.y * wb, qa.z * wa + qb.z * wb};
    const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    q = {q.w / n, q.x / n, q.y / n, q.z / n};
    CFrame out(mix(a.position, b.position, t));
    auto f = [](double v) { return static_cast<float>(v); };
    out.r[0][0] = f(1 - 2 * (q.y * q.y + q.z * q.z));
    out.r[0][1] = f(2 * (q.x * q.y - q.z * q.w));
    out.r[0][2] = f(2 * (q.x * q.z + q.y * q.w));
    out.r[1][0] = f(2 * (q.x * q.y + q.z * q.w));
    out.r[1][1] = f(1 - 2 * (q.x * q.x + q.z * q.z));
    out.r[1][2] = f(2 * (q.y * q.z - q.x * q.w));
    out.r[2][0] = f(2 * (q.x * q.z - q.y * q.w));
    out.r[2][1] = f(2 * (q.y * q.z + q.x * q.w));
    out.r[2][2] = f(1 - 2 * (q.x * q.x + q.y * q.y));
    return out;
}

} // namespace

// Booleans and EnumItems switch to the goal at the end. (Guess: when exactly
// Roblox flips them isn't pinned.)
Value lerpValue(const Value& from, const Value& to, double t)
{
    if (from.index() != to.index())
        return t >= 1 ? to : from;
    if (const auto* a = std::get_if<double>(&from))
        return *a + (std::get<double>(to) - *a) * t;
    if (const auto* a = std::get_if<Vector3>(&from))
        return mix(*a, std::get<Vector3>(to), t);
    if (const auto* a = std::get_if<Vector2>(&from)) {
        const Vector2 b = std::get<Vector2>(to);
        return Vector2{mix(a->x, b.x, t), mix(a->y, b.y, t)};
    }
    if (const auto* a = std::get_if<Color3>(&from)) {
        const Color3 b = std::get<Color3>(to);
        return Color3{mix(a->r, b.r, t), mix(a->g, b.g, t), mix(a->b, b.b, t)};
    }
    if (const auto* a = std::get_if<CFrame>(&from))
        return mix(*a, std::get<CFrame>(to), t);
    if (const auto* a = std::get_if<UDim>(&from))
        return mix(*a, std::get<UDim>(to), t);
    if (const auto* a = std::get_if<UDim2>(&from)) {
        const UDim2 b = std::get<UDim2>(to);
        return UDim2{mix(a->x, b.x, t), mix(a->y, b.y, t)};
    }
    if (const auto* a = std::get_if<Rect>(&from)) {
        const Rect b = std::get<Rect>(to);
        return Rect{{mix(a->min.x, b.min.x, t), mix(a->min.y, b.min.y, t)},
                    {mix(a->max.x, b.max.x, t), mix(a->max.y, b.max.y, t)}};
    }
    if (const auto* a = std::get_if<NumberRange>(&from)) {
        const NumberRange b = std::get<NumberRange>(to);
        return NumberRange{mix(a->min, b.min, t), mix(a->max, b.max, t)};
    }
    if (const auto* a = std::get_if<Vector3int16>(&from)) {
        const Vector3int16 b = std::get<Vector3int16>(to);
        auto m = [t](std::int16_t x, std::int16_t y) {
            return static_cast<std::int16_t>(std::lround(x + (static_cast<double>(y) - x) * t));
        };
        return Vector3int16{m(a->x, b.x), m(a->y, b.y), m(a->z, b.z)};
    }
    return t >= 1 ? to : from;
}

// --- Tweens ------------------------------------------------------------------

namespace {

std::shared_ptr<Tween> findTween(Shared& shared, const Instance& instance)
{
    for (const auto& tween : shared.state->tweens)
        if (tween->tween.lock().get() == &instance)
            return tween;
    return nullptr;
}

void setPlaybackState(Shared& shared, Tween& tween, std::string_view state)
{
    tween.state = state;
    auto instance = tween.tween.lock();
    tween.playing = state == "Playing" || state == "Delayed" ? instance : nullptr;
    if (!instance)
        return;
    auto& values = shared.state->extraProperties[instance.get()];
    values.first = instance;
    values.second.insert_or_assign("PlaybackState", packed(Value(enumItem("PlaybackState", state))));
    instance->changed.fire("PlaybackState");
}

void finish(Shared& shared, Tween& tween, std::string_view state)
{
    // Held through Completed: a Tween only the engine kept (Create(...):Play())
    // goes with the state that kept it.
    const InstancePtr instance = tween.tween.lock();
    setPlaybackState(shared, tween, state);
    if (instance)
        shared.events.fire(*instance, "Completed", PackedArgs{packed(Value(enumItem("PlaybackState", state)))});
}

// A property the API knows but the engine doesn't model, as scripts set it
// (InstanceApi's extraValues): where a tween reads and writes it. None
// unless the instance has one stored and `create` is false.
Packed* storedProperty(Shared& shared, Instance& target, const std::string& name, bool create)
{
    auto& all = shared.state->extraProperties;
    auto it = all.find(&target);
    if (it != all.end() && it->second.first.lock().get() != &target) {
        all.erase(it);
        it = all.end();
    }
    if (it == all.end()) {
        if (!create)
            return nullptr;
        it = all.emplace(&target, std::pair{target.shared_from_this(), std::map<std::string, Packed, std::less<>>{}})
                 .first;
    }
    auto& values = it->second.second;
    auto value = values.find(name);
    if (value == values.end()) {
        if (!create)
            return nullptr;
        value = values.emplace(name, Packed{}).first;
    }
    return &value->second;
}

void applyTween(Shared& shared, Tween& tween, double alpha)
{
    auto target = tween.target.lock();
    if (!target)
        return;
    const double eased = ease(alpha, tween.info.easingStyle.name, tween.info.easingDirection.name);
    for (const TweenTrack& track : tween.tracks) {
        // At the ends the exact values land, not eased approximations.
        auto valueAt = [&] {
            return alpha <= 0 ? track.start : alpha >= 1 ? track.goal : lerpValue(track.start, track.goal, eased);
        };
        const Property* property = target->classInfo().findProperty(track.property);
        if (!property) {
            // One only the API knows (AudioEqualizer.MidGain, which JJS tweens).
            *storedProperty(shared, *target, track.property, true) = packed(valueAt());
            target->changed.fire(track.property);
            continue;
        }
        if (property->readOnly())
            continue;
        try {
            Value value = valueAt();
            // A tweened CFrame is a script's move: it carries the part's
            // assembly (the game's drawers tween an anchored Main their parts are welded to).
            std::optional<BasePart::CarryAssembly> carry;
            if (track.property == "CFrame" && dynamic_cast<BasePart*>(target.get()))
                carry.emplace();
            property->set(*target, value);
        } catch (const Error&) {
            continue; // the property refused the value (a script locked it, say); the others go on
        }
    }
}

void startTween(Shared& shared, const std::shared_ptr<Tween>& tween)
{
    auto target = tween->target.lock();
    if (!target)
        return;
    // A newer tween on the same property takes it over.
    for (const auto& other : shared.state->tweens) {
        if (other == tween || other->target.lock() != target
            || (other->state != "Playing" && other->state != "Delayed"))
            continue;
        const bool overlaps = std::ranges::any_of(tween->tracks, [&](const TweenTrack& track) {
            return std::ranges::any_of(other->tracks,
                                       [&](const TweenTrack& o) { return o.property == track.property; });
        });
        if (overlaps)
            finish(shared, *other, "Cancelled");
    }
    for (TweenTrack& track : tween->tracks) {
        if (const Property* property = target->classInfo().findProperty(track.property)) {
            track.start = property->get(*target);
        } else if (const Packed* stored = storedProperty(shared, *target, track.property, false);
                   stored && stored->value.index() == track.goal.index()) {
            track.start = stored->value;
        } else {
            // Never set: a number starts from 0, the API's default for one;
            // anything else jumps to its goal. GUESS: the defaults.
            track.start = std::holds_alternative<double>(track.goal) ? Value(0.0) : track.goal;
        }
    }
    tween->elapsed = 0.0;
    tween->repetition = 0;
    tween->reversing = false;
    setPlaybackState(shared, *tween, tween->info.delayTime > 0 ? "Delayed" : "Playing");
}

} // namespace

InstancePtr createTween(Shared& shared, Instance& target, const TweenInfo& info, std::vector<TweenTrack> tracks)
{
    auto instance = std::make_shared<UnknownInstance>(ClassRegistry::placeholder("Tween"));
    auto tween = std::make_shared<Tween>();
    tween->tween = instance;
    tween->target = target.shared_from_this();
    tween->info = info;
    tween->tracks = std::move(tracks);
    std::erase_if(shared.state->tweens, [](const auto& t) { return t->tween.expired(); });
    shared.state->tweens.push_back(tween);
    auto& values = shared.state->extraProperties[instance.get()];
    values.first = instance;
    values.second.insert_or_assign("Instance", packed(Value(target.shared_from_this())));
    values.second.insert_or_assign("PlaybackState", packed(Value(enumItem("PlaybackState", "Begin"))));
    return instance;
}

void commandTween(Shared& shared, Instance& instance, TweenCommand command)
{
    auto tween = findTween(shared, instance);
    if (!tween)
        return;
    switch (command) {
    case TweenCommand::Play:
        if (tween->state == "Paused")
            setPlaybackState(shared, *tween,
                             tween->elapsed < tween->info.delayTime && tween->repetition == 0 ? "Delayed" : "Playing");
        else if (tween->state != "Playing" && tween->state != "Delayed")
            startTween(shared, tween);
        break;
    case TweenCommand::Pause:
        if (tween->state == "Playing" || tween->state == "Delayed")
            setPlaybackState(shared, *tween, "Paused");
        break;
    case TweenCommand::Cancel:
        if (tween->state != "Cancelled" && tween->state != "Completed" && tween->state != "Begin") {
            tween->elapsed = 0.0;
            finish(shared, *tween, "Cancelled");
        }
        break;
    }
}

void turnMotor(Shared& shared, Instance& motor)
{
    auto& turning = shared.state->turningMotors;
    for (const std::weak_ptr<Instance>& m : turning)
        if (m.lock().get() == &motor)
            return;
    turning.push_back(std::static_pointer_cast<Instance>(motor.shared_from_this()));
}

void stepMotors(Shared& shared, double dt)
{
    // MaxVelocity is radians a frame of the client's 60. GUESS: that the
    // frame is a 60th of a second rather than a physics step; the R6 rig's
    // 0.1 is what the R6 Animate script's swings were written for.
    auto& turning = shared.state->turningMotors;
    std::erase_if(turning, [dt](const std::weak_ptr<Instance>& weak) {
        auto* motor = dynamic_cast<Motor*>(weak.lock().get());
        if (!motor)
            return true;
        const float to = motor->desiredAngle();
        const float at = motor->currentAngle();
        const float most = motor->maxVelocity() * static_cast<float>(dt * 60.0);
        if (most <= 0.0f)
            return at == to;
        motor->setCurrentAngle(std::abs(to - at) <= most ? to : at + std::copysign(most, to - at));
        return motor->currentAngle() == to;
    });
}

void jumpToPage(Shared& shared, UIPageLayout& layout, GuiObject& page)
{
    GuiObject* from = layout.currentPage();
    if (&page == from)
        return;
    layout.assignCurrentPage(&page);
    layout.assignSlide(from, layout.animated() && layout.tweenTime() > 0.0f ? 0.0f : 1.0f);
    // A newer move takes over one under way.
    std::erase_if(shared.state->pageMoves, [&](const PageMove& move) { return move.layout.lock().get() == &layout; });
    shared.state->pageMoves.push_back({layout.weak_from_this()});
    if (from)
        shared.events.fire(layout, "PageLeave", PackedArgs{packed(Value(from->shared_from_this()))});
    shared.events.fire(layout, "PageEnter", PackedArgs{packed(Value(page.shared_from_this()))});
}

void stepPageMoves(Shared& shared, double dt)
{
    // Stopped's handlers may move again: walk the moves under way now.
    auto moves = std::exchange(shared.state->pageMoves, {});
    for (PageMove& move : moves) {
        const InstancePtr instance = move.layout.lock();
        auto* layout = dynamic_cast<UIPageLayout*>(instance.get());
        if (!layout)
            continue;
        move.elapsed += dt;
        const double duration = layout->animated() ? std::max(static_cast<double>(layout->tweenTime()), 0.0) : 0.0;
        if (move.elapsed < duration) {
            const double eased = ease(move.elapsed / duration, enums::item(layout->easingStyle()).name,
                                      enums::item(layout->easingDirection()).name);
            layout->assignSlide(layout->slide().from.lock().get(), static_cast<float>(eased));
            shared.state->pageMoves.push_back(move);
            continue;
        }
        layout->assignSlide(nullptr, 1.0f);
        if (GuiObject* page = layout->currentPage())
            shared.events.fire(*layout, "Stopped", PackedArgs{packed(Value(page->shared_from_this()))});
    }
}

void stepTweens(Shared& shared, double dt)
{
    // Tweens finishing can start others; walk a snapshot.
    auto tweens = shared.state->tweens;
    for (const auto& tween : tweens) {
        if (tween->state != "Playing" && tween->state != "Delayed")
            continue;
        if (tween->tween.expired() || tween->target.expired()) {
            tween->state = "Cancelled";
            continue;
        }
        tween->elapsed += dt;
        const double delay = tween->repetition == 0 ? tween->info.delayTime : 0.0;
        if (tween->elapsed < delay)
            continue;
        if (tween->state == "Delayed")
            setPlaybackState(shared, *tween, "Playing");
        const double duration = std::max(tween->info.time, 0.0);
        double t = duration > 0 ? (tween->elapsed - delay) / duration : 1.0;
        while (t >= 1.0) {
            applyTween(shared, *tween, tween->reversing ? 0.0 : 1.0);
            if (tween->info.reverses && !tween->reversing) {
                tween->reversing = true;
            } else {
                tween->reversing = false;
                ++tween->repetition;
                const double repeats = tween->info.repeatCount;
                if (repeats >= 0 && tween->repetition > repeats) {
                    finish(shared, *tween, "Completed");
                    break;
                }
            }
            tween->elapsed -= duration + delay;
            t = duration > 0 ? tween->elapsed / duration : 1.0;
            if (duration <= 0 && tween->state == "Playing" && tween->info.repeatCount < 0)
                break; // a zero-length endless tween: once per frame
        }
        if (tween->state == "Playing" && t < 1.0)
            applyTween(shared, *tween, tween->reversing ? 1.0 - t : t);
    }
}

// --- Input -------------------------------------------------------------------

std::vector<InstancePtr> InputState::pressedKeyObjects() const
{
    std::vector<InstancePtr> objects;
    for (const auto& [key, object] : keyObjects)
        if (keys.contains(key))
            objects.push_back(object);
    return objects;
}

InputState& inputState(Shared& shared)
{
    return shared.state->input;
}

void bindAction(Runtime& runtime, std::string name, int ref, int priority, std::vector<ActionInput> inputs,
                ThreadAccess access, int globals)
{
    unbindAction(runtime, name);
    auto& state = *runtime.shared().state;
    state.actions.push_back(BoundAction{&runtime, std::move(name), ref, priority, state.nextActionSeq++,
                                        std::move(inputs), access, globals});
}

namespace {

void unrefAction(Runtime& runtime, const BoundAction& action)
{
    lua_unref(runtime.main(), action.ref);
    if (action.globals != LUA_NOREF)
        lua_unref(runtime.main(), action.globals);
}

} // namespace

void unbindAction(Runtime& runtime, std::string_view name)
{
    std::erase_if(runtime.shared().state->actions, [&](const BoundAction& action) {
        if (action.runtime != &runtime || action.name != name)
            return false;
        unrefAction(runtime, action);
        return true;
    });
}

void unbindAllActions(Runtime& runtime)
{
    std::erase_if(runtime.shared().state->actions, [&](const BoundAction& action) {
        if (action.runtime != &runtime)
            return false;
        unrefAction(runtime, action);
        return true;
    });
}

bool runActions(Shared& shared, const InstancePtr& input, Enum::KeyCode key, Enum::UserInputType type,
                Enum::UserInputState state)
{
    // Highest priority first, the most recent binding first among equals.
    std::vector<BoundAction> candidates;
    for (const BoundAction& action : shared.state->actions) {
        const bool matches = std::ranges::any_of(action.inputs, [&](const ActionInput& in) {
            return in.isKey ? in.value == static_cast<std::uint32_t>(key) && key != Enum::KeyCode::None
                            : in.value == static_cast<std::uint32_t>(type);
        });
        if (matches)
            candidates.push_back(action);
    }
    std::ranges::sort(candidates, [](const BoundAction& a, const BoundAction& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.seq > b.seq;
    });
    for (const BoundAction& action : candidates) {
        Runtime& rt = *action.runtime;
        lua_State* thread = lua_newthread(rt.main());
        rt.setAccess(thread, action.access);
        Runtime::useGlobals(thread, action.globals);
        const int ref = lua_ref(rt.main(), -1);
        lua_pop(rt.main(), 1);
        lua_getref(thread, action.ref);
        pushString(thread, action.name);
        pushEnumItem(thread, enums::item(state));
        pushInstance(thread, input.get());
        // The result decides whether the input passes on; nil means Sink.
        bool sink = true;
        rt.onFinish(thread, [&sink](lua_State* t, int status, int results) {
            if (status != LUA_OK || results == 0)
                return;
            if (const EnumItem* item = to<EnumItem>(t, lua_gettop(t) - results + 1))
                sink = item->name != "Pass";
        });
        if (auto error = rt.resume(thread, 3, "BindAction"))
            rt.report(*error);
        lua_unref(rt.main(), ref);
        if (sink)
            return true;
    }
    return false;
}

// --- Characters --------------------------------------------------------------

namespace {

std::shared_ptr<Part> makePart(std::string_view name, Vector3 size, const CFrame& cframe, Instance& parent)
{
    auto part = std::make_shared<Part>();
    part->setName(std::string(name));
    part->setSize(size);
    part->setCFrame(cframe);
    part->setParent(&parent);
    return part;
}

// A minimal R6-shaped stand-in until the character side supplies real ones:
// enough for WaitForChild("Humanoid") and friends.
InstancePtr defaultCharacter(const CFrame& spawn)
{
    auto model = std::make_shared<Model>();
    auto root = makePart("HumanoidRootPart", {2, 2, 1}, spawn, *model);
    root->setTransparency(1.0f);
    makePart("Torso", {2, 2, 1}, spawn, *model);
    makePart("Head", {2, 1, 1}, spawn * CFrame(0, 1.5f, 0), *model);
    makePart("Left Arm", {1, 2, 1}, spawn * CFrame(-1.5f, 0, 0), *model);
    makePart("Right Arm", {1, 2, 1}, spawn * CFrame(1.5f, 0, 0), *model);
    makePart("Left Leg", {1, 2, 1}, spawn * CFrame(-0.5f, -2, 0), *model);
    makePart("Right Leg", {1, 2, 1}, spawn * CFrame(0.5f, -2, 0), *model);
    // The attachments character scripts wait for.
    auto attach = [](Instance& part, std::string_view name, Vector3 position) {
        auto attachment = std::make_shared<Attachment>();
        attachment->setName(std::string(name));
        attachment->setPosition(position);
        attachment->setParent(&part);
    };
    attach(*root, "RootRigAttachment", {});
    attach(*root, "RootAttachment", {});
    if (Instance* head = model->findFirstChild("Head")) {
        attach(*head, "HatAttachment", {0, 0.5f, 0});
        attach(*head, "FaceFrontAttachment", {0, 0, -0.5f});
    }
    auto humanoid = std::make_shared<Humanoid>();
    humanoid->setParent(model.get());
    model->setPrimaryPart(root.get());
    return model;
}

CFrame spawnPoint(DataModel& game)
{
    for (const InstancePtr& d : game.workspace()->descendants())
        if (auto* spawn = dynamic_cast<SpawnLocation*>(d.get()))
            return CFrame{spawn->position() + Vector3{0, spawn->size().y / 2 + 3.0f, 0}};
    return {0, 5, 0};
}

void copyChildren(const Instance* from, Instance& to)
{
    if (!from)
        return;
    for (const InstancePtr& child : from->children())
        if (InstancePtr copy = child->clone())
            copy->setParent(&to);
}

Value instanceValue(Instance& instance)
{
    return {instance.shared_from_this()};
}

} // namespace

void spawnCharacter(Shared& shared, DataModel& game, Player& player)
{
    if (Model* old = player.character()) {
        shared.events.fire(player, "CharacterRemoving", PackedArgs{packed(instanceValue(*old))});
        player.setCharacter(nullptr);
        old->destroy();
    }
    auto* starterPlayer = game.findService("StarterPlayer");
    InstancePtr character;
    if (Instance* custom = starterPlayer ? starterPlayer->findFirstChild("StarterCharacter") : nullptr)
        character = custom->clone();
    if (!character && shared.characterFactory)
        character = shared.characterFactory(player);
    if (!character)
        character = defaultCharacter(spawnPoint(game));
    auto* model = dynamic_cast<Model*>(character.get());
    if (!model)
        return;
    // A spawn buried under terrain or a part still puts the character on
    // top (the battle royale template hides its spawns under the grass).
    if (shared.raycast)
        liftOutOfObstructions(*model, [&](const RaycastQuery& query) { return shared.raycast(game, query); });
    model->setName(player.name());
    // StarterCharacterScripts' scripts replace the character's own of the
    // same name (a place's Animate instead of the default one).
    if (Instance* scripts = starterPlayer ? starterPlayer->findFirstChildOfClass("StarterCharacterScripts") : nullptr)
        for (const InstancePtr& child : scripts->children())
            if (child->isA("LuaSourceContainer"))
                if (Instance* existing = model->findFirstChild(child->name());
                    existing && existing->isA("LuaSourceContainer"))
                    existing->destroy();
    copyChildren(starterPlayer ? starterPlayer->findFirstChildOfClass("StarterCharacterScripts") : nullptr, *model);

    // Every character comes with a new Backpack, filled from StarterPack and
    // then the player's StarterGear. (GUESS: the order of the two; the
    // BackpackScript's special case for a StarterGear tool suggests StarterPack's
    // come first.) The old one goes with its tools.
    if (Instance* old = player.findFirstChildOfClass("Backpack"))
        old->destroy();
    auto backpack = std::make_shared<Backpack>();
    copyChildren(game.findService("StarterPack"), *backpack);
    copyChildren(player.findFirstChildOfClass("StarterGear"), *backpack);
    backpack->setParent(&player);

    // Avatar loading order: Character is set and parented before CharacterAdded.
    player.setCharacter(model);
    model->setParent(game.workspace());
    shared.events.fire(player, "CharacterAdded", PackedArgs{packed(instanceValue(*model))});
    shared.events.fire(player, "CharacterAppearanceLoaded", PackedArgs{packed(instanceValue(*model))});

    // Died when health reaches zero, then a respawn after Players.RespawnTime.
    if (auto* humanoid = model->findFirstChildOfClass("Humanoid")) {
        auto weakHumanoid = std::weak_ptr<Instance>(humanoid->shared_from_this());
        auto fired = std::make_shared<bool>(false);
        humanoid->changed.connect(
            [&shared, &game, weakHumanoid, fired,
             weakPlayer = std::weak_ptr<Instance>(player.shared_from_this())](std::string_view property) {
                auto h = std::dynamic_pointer_cast<Humanoid>(weakHumanoid.lock());
                if (property != "Health" || !h || *fired || h->health() > 0.0f)
                    return;
                *fired = true; // Died itself is the Humanoid's (HumanoidApi.cpp)
                auto* players = dynamic_cast<Players*>(game.findFirstChildOfClass("Players"));
                Runtime* server = shared.runtimeFor(ScriptRole::Server);
                if (!players || !players->characterAutoLoads() || !server)
                    return;
                server->after(players->respawnTime(), [&shared, &game, weakPlayer] {
                    if (auto p = std::dynamic_pointer_cast<Player>(weakPlayer.lock()); p && p->parent())
                        spawnCharacter(shared, game, *p);
                });
            });
    }
}

} // namespace lunar::lua

namespace lunar {

float liftOutOfObstructions(Model& model, const std::function<std::optional<RaycastHit>(const RaycastQuery&)>& cast)
{
    // The box the model's parts span in the world.
    constexpr float huge = std::numeric_limits<float>::max();
    std::array<float, 3> low{huge, huge, huge};
    std::array<float, 3> high{-huge, -huge, -huge};
    for (const InstancePtr& d : model.descendants()) {
        const auto* part = dynamic_cast<const BasePart*>(d.get());
        if (!part)
            continue;
        const CFrame& cf = part->cframe();
        const Vector3 half = part->size() * 0.5f;
        const std::array<float, 3> centre{cf.position.x, cf.position.y, cf.position.z};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto& row = cf.r[axis];
            const float reach = std::abs(row[0]) * half.x + std::abs(row[1]) * half.y + std::abs(row[2]) * half.z;
            low[axis] = std::min(low[axis], centre[axis] - reach);
            high[axis] = std::max(high[axis], centre[axis] + reach);
        }
    }
    if (low[0] > high[0])
        return 0.0f;
    // GUESS: Roblox tests the box for overlaps; rays down through it on a
    // grid of at most two studs (from its top, so a ceiling with room under
    // it stays a ceiling) find what's inside, and the model rises onto the
    // highest of it until nothing is.
    constexpr float spacing = 2.0f;
    constexpr int maxRays = 16;
    const int nx = std::clamp(static_cast<int>(std::ceil((high[0] - low[0]) / spacing)), 1, maxRays);
    const int nz = std::clamp(static_cast<int>(std::ceil((high[2] - low[2]) / spacing)), 1, maxRays);
    constexpr float inset = 0.05f;
    constexpr float tolerance = 0.01f;
    RaycastQuery query;
    query.filter = {model.shared_from_this()};
    query.ignoreWater = true;
    query.respectCanCollide = true;
    float risen = 0.0f;
    for (int pass = 0; pass < 8; ++pass) {
        const float bottom = low[1] + risen;
        const float height = high[1] - low[1];
        float top = bottom;
        for (int i = 0; i <= nx; ++i)
            for (int k = 0; k <= nz; ++k) {
                const float x
                    = std::lerp(low[0] + inset, high[0] - inset, static_cast<float>(i) / static_cast<float>(nx));
                const float z
                    = std::lerp(low[2] + inset, high[2] - inset, static_cast<float>(k) / static_cast<float>(nz));
                query.origin = {x, bottom + height, z};
                query.direction = {0.0f, -height, 0.0f};
                if (const auto hit = cast(query))
                    top = std::max(top, hit->position.y);
            }
        if (top <= bottom + tolerance)
            break;
        risen += top - bottom;
    }
    if (risen > 0.0f)
        model.pivotTo(model.getPivot() + Vector3{0.0f, risen, 0.0f});
    return risen;
}

} // namespace lunar

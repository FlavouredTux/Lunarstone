#include <lunar/net/Motion.hpp>

#include <lunar/Classes.hpp>
#include <lunar/avatar/Quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lunar::net {

namespace {

std::uint64_t keyOf(InstanceId id)
{
    return static_cast<std::uint64_t>(id.scope) << 32 | id.index;
}

// How many samples a root keeps (MaxFrameBufferSize), the most one sample
// spacing counts for in the average (InterpolationMaxDelayMSec), and the
// average's weights [code: sub_10121ee1c and the FInt defaults].
constexpr std::size_t kMaxSamples = 10;
constexpr double kMaxSpacing = 0.5;
constexpr double kSpacingKeep = 0.9;
// How long a root remembers a sample's lateness, and how much faster or
// slower than real time its playback may run while it catches up with a
// changed lateness; a change bigger than kSnapOffset is jumped (GUESS all).
constexpr double kLatenessWindow = 3.0;
constexpr double kWarp = 0.1;
constexpr double kSnapOffset = 0.5;
// A root with no instance (streamed out, destroyed) is dropped after this long without samples.
constexpr double kForgetAfter = 10.0;
// Two samples farther apart than this are a teleport: no curve between them
// and no easing into the new place (GUESS).
constexpr float kTeleportStuds = 50.0f;
// Samples farther apart in time than this have a pause between them (the
// server sent nothing while the part rested): the first one holds until
// kGapBlend before the second, then glides to it (GUESS both).
constexpr double kMaxGap = 0.5;
constexpr double kGapBlend = 0.1;
// A correction smaller than this (studs, or about this many radians) is over.
constexpr float kSettled = 1e-3f;

// The rotation made a rotation again, as a part's setter does
// (CFrame::orthonormalized). Moving an assembly takes the root's inverse as
// its transpose, which only holds while the rotation is one: off by a little,
// the parts moved by it scale by that little each time.
CFrame rigid(CFrame cf)
{
    return cf.orthonormalized();
}

// An attachment's part (attachments can sit under other attachments).
BasePart* attachmentPart(const Attachment* attachment)
{
    const Instance* node = attachment;
    while (dynamic_cast<const Attachment*>(node))
        node = node->parent();
    return const_cast<BasePart*>(dynamic_cast<const BasePart*>(node));
}

// The joints that hold two parts rigidly, as far as an assembly goes.
bool rigidJoint(const Instance& instance)
{
    return dynamic_cast<const JointInstance*>(&instance) || dynamic_cast<const WeldConstraint*>(&instance)
           || instance.isA("AnimationConstraint") || instance.isA("RigidConstraint");
}

// The two parts a rigid joint holds while it's enabled; nulls otherwise.
std::pair<BasePart*, BasePart*> heldParts(const Instance& instance)
{
    if (const auto* joint = dynamic_cast<const JointInstance*>(&instance))
        return joint->enabled() ? std::pair{joint->part0(), joint->part1()} : std::pair<BasePart*, BasePart*>{};
    if (const auto* weld = dynamic_cast<const WeldConstraint*>(&instance))
        return weld->enabled() ? std::pair{weld->part0(), weld->part1()} : std::pair<BasePart*, BasePart*>{};
    if (const auto* constraint = dynamic_cast<const Constraint*>(&instance);
        constraint && constraint->enabled()
        && (instance.isA("AnimationConstraint") || instance.isA("RigidConstraint")))
        return {attachmentPart(constraint->attachment0()), attachmentPart(constraint->attachment1())};
    return {};
}

// Where the root's joints live: the Model it is in (a character, a
// vehicle), or the root itself when it isn't in one. Not the outermost
// model: a game's whole map can be one, and it changes as it streams.
Instance* jointScope(BasePart& root)
{
    for (Instance* node = root.parent(); node; node = node->parent())
        if (dynamic_cast<Model*>(node))
            return dynamic_cast<Workspace*>(node) ? &root : node;
    return &root;
}

// The parts the joints in `scope` hold to the root, breadth first from it;
// `onJoint` sees every rigid joint on the way, enabled or not.
std::vector<BasePart*> walk(BasePart& root, const Instance& scope, const std::function<void(const Instance&)>& onJoint)
{
    std::unordered_map<BasePart*, std::vector<BasePart*>> links;
    auto visit = [&](const Instance& instance) {
        if (onJoint && rigidJoint(instance))
            onJoint(instance);
        const auto [part0, part1] = heldParts(instance);
        if (!part0 || !part1 || part0 == part1)
            return;
        links[part0].push_back(part1);
        links[part1].push_back(part0);
    };
    visit(scope);
    for (const InstancePtr& descendant : scope.descendants())
        visit(*descendant);
    std::vector<BasePart*> out;
    std::unordered_set<BasePart*> seen{&root};
    std::deque<BasePart*> queue{&root};
    while (!queue.empty()) {
        BasePart* part = queue.front();
        queue.pop_front();
        if (auto found = links.find(part); found != links.end())
            for (BasePart* other : found->second)
                if (seen.insert(other).second) {
                    out.push_back(other);
                    queue.push_back(other);
                }
    }
    return out;
}

} // namespace

PhysicsPV interpolatePv(const PhysicsPV& a, const PhysicsPV& b, double dt, double u)
{
    const auto t = static_cast<float>(std::clamp(u, 0.0, 1.0));
    const auto h = static_cast<float>(dt);
    const Vector3 p0 = a.cframe.position, p1 = b.cframe.position;
    Vector3 position;
    if ((p1 - p0).magnitude() > kTeleportStuds) {
        position = t < 1.0f ? p0 : p1;
    } else {
        // Cubic Hermite: the curve leaves a with a's velocity and meets b with b's.
        const float t2 = t * t, t3 = t2 * t;
        const float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
        position = p0 * h00 + a.linearVelocity * (h10 * h) + p1 * h01 + b.linearVelocity * (h11 * h);
    }
    PhysicsPV out;
    out.cframe = avatar::Quaternion::slerp(avatar::Quaternion::fromCFrame(a.cframe),
                                           avatar::Quaternion::fromCFrame(b.cframe), t)
                     .toCFrame(position);
    out.linearVelocity = a.linearVelocity + (b.linearVelocity - a.linearVelocity) * t;
    out.angularVelocity = a.angularVelocity + (b.angularVelocity - a.angularVelocity) * t;
    return out;
}

PhysicsPV extrapolatePv(const PhysicsPV& pv, double dt)
{
    const auto e = static_cast<float>(dt);
    PhysicsPV out = pv;
    const CFrame turn = avatar::fromRotationVector(pv.angularVelocity * e).toCFrame();
    out.cframe = turn * pv.cframe.rotation();
    out.cframe.position = pv.cframe.position + pv.linearVelocity * e;
    return out;
}

ReplicatedMotion::~ReplicatedMotion()
{
    for (auto& [key, track] : tracks_)
        forget(track);
}

void ReplicatedMotion::forget(Track& track)
{
    for (Connection& watch : track.watches)
        watch.disconnect();
    track.watches.clear();
}

void ReplicatedMotion::receive(InstanceId part, double serverTime, double arrival, const PhysicsPV& pv, bool timed)
{
    ++received_;
    Track& track = tracks_[keyOf(part)];
    track.lastArrival = arrival;
    ++track.received;
    if (timed || track.lateness.empty()) {
        track.lateness.emplace_back(arrival, arrival - serverTime);
        while (track.lateness.size() > 1 && track.lateness.front().first < arrival - kLatenessWindow)
            track.lateness.pop_front();
        track.target = std::ranges::max(track.lateness, {}, &std::pair<double, double>::second).second;
        if (!track.offset)
            track.offset = track.target;
    }
    // As the client does: a sample older than the newest is dropped, one
    // with the same time replaces it, and the spacing of the rest is averaged.
    auto& samples = track.samples;
    if (!samples.empty() && serverTime < samples.back().serverTime)
        return;
    if (!samples.empty() && serverTime - samples.back().serverTime < 1e-6) {
        samples.back().pv = pv;
        return;
    }
    if (!samples.empty())
        track.spacing = kSpacingKeep * track.spacing
                        + (1 - kSpacingKeep) * std::min(serverTime - samples.back().serverTime, kMaxSpacing);
    samples.push_back(Sample{serverTime, pv});
    if (samples.size() > kMaxSamples)
        samples.erase(samples.begin());
}

double ReplicatedMotion::shownTime(const Track& track, double now) const
{
    return now - delayFactor * track.spacing - track.offset.value_or(track.target);
}

std::optional<PhysicsPV> ReplicatedMotion::pathAt(const Track& track, double shown, bool* held) const
{
    bool scratch = false;
    bool& still = held ? *held : scratch;
    still = true;
    const auto& samples = track.samples;
    if (samples.empty())
        return std::nullopt;
    if (shown <= samples.front().serverTime)
        return samples.front().pv;
    const auto next = std::ranges::upper_bound(samples, shown, {}, &Sample::serverTime);
    const Sample& a = *std::prev(next);
    if (next == samples.end()) {
        still = shown - a.serverTime >= maxExtrapolation;
        return extrapolatePv(a.pv, std::min(shown - a.serverTime, maxExtrapolation));
    }
    const Sample& b = *next;
    if (b.serverTime - a.serverTime > kMaxGap) {
        const double start = b.serverTime - kGapBlend;
        const PhysicsPV rest = extrapolatePv(a.pv, std::min(std::min(shown, start) - a.serverTime, maxExtrapolation));
        still = shown < start && shown - a.serverTime >= maxExtrapolation;
        return shown < start ? rest : interpolatePv(rest, b.pv, kGapBlend, (shown - start) / kGapBlend);
    }
    still = false;
    const double dt = b.serverTime - a.serverTime;
    return interpolatePv(a.pv, b.pv, dt, dt > 0 ? (shown - a.serverTime) / dt : 1.0);
}

std::optional<PhysicsPV> ReplicatedMotion::at(InstanceId part, double now) const
{
    const auto it = tracks_.find(keyOf(part));
    if (it == tracks_.end())
        return std::nullopt;
    return pathAt(it->second, shownTime(it->second, now));
}

std::optional<double> ReplicatedMotion::shownAt(InstanceId part, double now) const
{
    const auto it = tracks_.find(keyOf(part));
    if (it == tracks_.end() || it->second.samples.empty())
        return std::nullopt;
    return shownTime(it->second, now);
}

std::size_t ReplicatedMotion::receivedFor(InstanceId root) const
{
    const auto it = tracks_.find(keyOf(root));
    return it == tracks_.end() ? 0 : it->second.received;
}

std::vector<InstanceId> ReplicatedMotion::rootIds() const
{
    std::vector<InstanceId> out;
    for (const auto& [key, track] : tracks_)
        out.push_back({static_cast<std::uint32_t>(key >> 32), static_cast<std::uint32_t>(key)});
    return out;
}

std::vector<BasePart*> ReplicatedMotion::assemblyOf(BasePart& root)
{
    return walk(root, *jointScope(root), {});
}

void ReplicatedMotion::rebuild(Track& track, BasePart& root)
{
    forget(track);
    *track.dirty = false;
    auto markDirty = [dirty = std::weak_ptr<bool>(track.dirty)](auto&&...) {
        if (auto flag = dirty.lock())
            *flag = true;
    };
    Instance* scope = jointScope(root);
    track.watches.push_back(scope->descendantAdded.connect(markDirty));
    track.watches.push_back(scope->descendantRemoving.connect(markDirty));
    track.watches.push_back(root.ancestryChanged.connect(markDirty));
    const auto parts = walk(root, *scope, [&](const Instance& joint) {
        track.watches.push_back(const_cast<Instance&>(joint).changed.connect([markDirty](std::string_view property) {
            if (property == "Part0" || property == "Part1" || property == "Enabled" || property == "Attachment0"
                || property == "Attachment1")
                markDirty();
        }));
    });
    track.assembly.clear();
    for (BasePart* part : parts)
        track.assembly.push_back(part->weak_from_this());
}

void ReplicatedMotion::apply(Track& track, BasePart& root, const PhysicsPV& pv)
{
    if (*track.dirty)
        rebuild(track, root);
    // Everything jointed to the root moves with it, keeping its offset (the
    // pose the animation last left it in); the joints are then solved from
    // the root by whoever animates the rig.
    // Every place is worked out before anything moves: setting one part of a
    // welded assembly moves the rest with it.
    const CFrame moved = rigid(pv.cframe) * rigid(root.cframe()).inverse();
    std::vector<std::pair<InstancePtr, CFrame>> moves;
    for (const std::weak_ptr<Instance>& weak : track.assembly)
        if (InstancePtr instance = weak.lock())
            if (auto* part = dynamic_cast<BasePart*>(instance.get()))
                moves.emplace_back(std::move(instance), rigid(moved * part->cframe()));
    root.setCFrame(rigid(pv.cframe));
    root.setAssemblyLinearVelocity(pv.linearVelocity);
    root.setAssemblyAngularVelocity(pv.angularVelocity);
    for (const auto& [instance, cframe] : moves) {
        auto* part = static_cast<BasePart*>(instance.get());
        part->setCFrame(cframe);
        part->setAssemblyLinearVelocity(pv.linearVelocity);
        part->setAssemblyAngularVelocity(pv.angularVelocity);
    }
    track.applied = pv;
}

void ReplicatedMotion::dropJoined(const Resolve& resolve)
{
    // The livelier of two tracks: samples more lately, or more of them.
    const auto livelier = [](const Track& a, const Track& b) {
        return std::pair(a.lastArrival, a.received) > std::pair(b.lastArrival, b.received);
    };
    std::unordered_map<const BasePart*, const Track*> heldBy;
    std::vector<std::pair<std::uint64_t, const BasePart*>> roots;
    for (auto& [key, track] : tracks_) {
        const InstancePtr instance
            = resolve({static_cast<std::uint32_t>(key >> 32), static_cast<std::uint32_t>(key)});
        auto* root = dynamic_cast<BasePart*>(instance.get());
        if (!root)
            continue;
        if (*track.dirty)
            rebuild(track, *root);
        roots.emplace_back(key, root);
        for (const std::weak_ptr<Instance>& weak : track.assembly)
            if (const InstancePtr part = weak.lock()) {
                const Track*& holder = heldBy[static_cast<const BasePart*>(part.get())];
                if (!holder || livelier(track, *holder))
                    holder = &track;
            }
    }
    std::vector<std::uint64_t> joined;
    for (const auto& [key, root] : roots)
        if (const auto holder = heldBy.find(root);
            holder != heldBy.end() && livelier(*holder->second, tracks_.at(key)))
            joined.push_back(key);
    for (const std::uint64_t key : joined) {
        forget(tracks_.at(key));
        tracks_.erase(key);
    }
}

void ReplicatedMotion::applyNewest(InstanceId part, const Resolve& resolve)
{
    const auto it = tracks_.find(keyOf(part));
    if (it == tracks_.end() || it->second.samples.empty())
        return;
    const InstancePtr instance = resolve(part);
    if (auto* root = dynamic_cast<BasePart*>(instance.get()))
        apply(it->second, *root, it->second.samples.back().pv);
}

void ReplicatedMotion::step(double now, const Resolve& resolve, const std::function<bool(InstanceId)>& ours)
{
    stepping_ = true;
    dropJoined(resolve);
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        Track& track = it->second;
        const InstanceId id{static_cast<std::uint32_t>(it->first >> 32), static_cast<std::uint32_t>(it->first)};
        const InstancePtr instance = resolve(id);
        auto* root = dynamic_cast<BasePart*>(instance.get());
        if (!root) {
            if (now - track.lastArrival > kForgetAfter) {
                forget(track);
                it = tracks_.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        if (ours && ours(id)) {
            // Ours to simulate. GUESS: the owner drops what the server sends
            // for it (the server drops an owned part's frames from anyone but
            // its owner, ServerReplicator vfn_143 [code]); the game's server kept
            // sending its own swing of a side door for a few frames after
            // handing it to us, then nothing for up to 2.5 s mid-swing
            // [capture].
            track.samples.clear();
            track.applied.reset();
            track.lastShown.reset();
            track.held = true;
            ++it;
            continue;
        }
        // The playback's offset eases toward the latest lateness (at once
        // while the part stood still, where nobody sees the time jump).
        const double dt = track.lastShown ? std::max(now - track.lastStep, 0.0) : 0.0;
        track.lastStep = now;
        const double gap = track.target - track.offset.value_or(track.target);
        bool jumped = false;
        if (!track.offset || std::abs(gap) > kSnapOffset || track.held) {
            jumped = track.offset && gap != 0;
            track.offset = track.target;
        } else {
            *track.offset += std::clamp(gap, -kWarp * dt, kWarp * dt);
        }
        const double shown = shownTime(track, now);
        std::optional<PhysicsPV> pv = pathAt(track, shown, &track.held);
        // What the last step showed against where the samples there are
        // now put that moment (or, after a jump in time, put this one):
        // the difference fades instead of jumping.
        if (pv && track.applied && (track.lastShown || jumped)) {
            const PhysicsPV was = *pathAt(track, jumped ? shown : *track.lastShown);
            const Vector3 off = track.applied->cframe.position - was.cframe.position;
            const avatar::Quaternion turned
                = avatar::Quaternion::fromCFrame(track.applied->cframe.rotation() * was.cframe.rotation().inverse());
            // Once it has faded to nothing a resting part stays put.
            const bool left = off.magnitude() > kSettled || std::abs(turned.w) < 1.0f - kSettled * kSettled;
            if (left && off.magnitude() < kTeleportStuds) {
                const auto fade = static_cast<float>(std::exp(-dt / correctionTime));
                const CFrame turn = avatar::Quaternion::slerp({}, turned, fade).toCFrame();
                const Vector3 position = pv->cframe.position + off * fade;
                pv->cframe = turn * pv->cframe.rotation();
                pv->cframe.position = position;
            }
        }
        track.lastShown = shown;
        // Samples the playback has passed are no longer needed (one before it stays).
        const auto next = std::ranges::upper_bound(track.samples, shown, {}, &Sample::serverTime);
        if (next - track.samples.begin() > 1)
            track.samples.erase(track.samples.begin(), std::prev(next));
        if (pv && pv != track.applied)
            apply(track, *root, *pv);
        ++it;
    }
}

} // namespace lunar::net

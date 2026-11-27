#include <lunar/net/StreamingClient.hpp>

#include <lunar/Classes.hpp>
#include <lunar/Instance.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/Session.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <utility>

namespace lunar::net {

namespace {

using RegionKey = std::tuple<std::int32_t, std::int32_t, std::int32_t>;

RegionKey keyOf(RegionId r)
{
    return {r.x, r.y, r.z};
}

RegionId regionAt(const RegionKey& k)
{
    return {std::get<0>(k), std::get<1>(k), std::get<2>(k)};
}

float distance(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::int32_t floorRegion(float v)
{
    return static_cast<std::int32_t>(std::floor(v / kRegionStuds));
}

// Instances the client keeps whatever the distance: characters, persistent
// models (ModelStreamingMode Persistent / PersistentPerPlayer) and what is
// under them.
bool kept(const Instance& instance, const std::set<const Instance*>& characters)
{
    for (const Instance* i = instance.parent(); i; i = i->parent()) {
        if (characters.contains(i))
            return true;
        if (const auto* model = dynamic_cast<const Model*>(i)) {
            const auto mode = model->modelStreamingMode();
            if (mode == Enum::ModelStreamingMode::Persistent || mode == Enum::ModelStreamingMode::PersistentPerPlayer)
                return true;
        }
        if (i->isA("Player") || i->isA("Players"))
            return true;
    }
    return false;
}

} // namespace

RegionId regionOf(Vector3 position)
{
    return {floorRegion(position.x), floorRegion(position.y), floorRegion(position.z)};
}

Vector3 regionCenter(RegionId region)
{
    const float half = kRegionStuds / 2;
    return {static_cast<float>(region.x) * kRegionStuds + half, static_cast<float>(region.y) * kRegionStuds + half,
            static_cast<float>(region.z) * kRegionStuds + half};
}

RegionId resolveRegion(const StreamRegionRef& ref, RegionId previous)
{
    switch (ref.code) {
    case 0:
        return ref.region;
    case 1:
        return {previous.x, previous.y + 1, previous.z};
    case 2:
        return {previous.x, previous.y, previous.z + 1};
    case 3:
        return {previous.x + 1, previous.y, previous.z};
    default:
        return kNoRegion; // 4: all three INT_MAX
    }
}

StreamingClient::StreamingClient(StreamingClientOptions options) : options_(std::move(options)) {}

std::int32_t StreamingClient::requestStreamAround(Vector3 position, double timeoutSeconds)
{
    Prefetch p;
    p.id = nextPrefetchId_++;
    if (nextPrefetchId_ < 0)
        nextPrefetchId_ = 1; // [code: 0x1056c585a resets a negative counter to 1]
    p.position = position;
    p.timeout = timeoutSeconds;
    prefetches_.push_back(p);
    return p.id;
}

bool StreamingClient::streamAroundDone(std::int32_t id) const
{
    for (const Prefetch& p : prefetches_)
        if (p.id == id)
            return p.done;
    return id > 0 && id < nextPrefetchId_; // answered (or expired) and forgotten
}

std::size_t StreamingClient::partsHeld() const
{
    std::size_t n = 0;
    for (const auto& [k, held] : regions_)
        for (const auto& weak : held.parts)
            if (!weak.expired())
                ++n;
    return n;
}

std::vector<std::string> StreamingClient::describe(std::optional<Vector3> from) const
{
    std::vector<std::pair<float, std::string>> rows;
    for (const auto& [k, held] : regions_) {
        std::size_t alive = 0;
        for (const auto& weak : held.parts)
            if (auto p = weak.lock(); p && p->parent())
                ++alive;
        const RegionId r = regionAt(k);
        const float d = from ? distance(regionCenter(r), *from) : 0.0f;
        rows.emplace_back(d, std::format("{} {} parts{}", regionText(r), alive,
                                         from ? std::format(", {:.0f} studs away", d) : ""));
    }
    std::ranges::sort(rows, {}, &std::pair<float, std::string>::first);
    std::vector<std::string> out;
    for (auto& [d, line] : rows)
        out.push_back(std::move(line));
    return out;
}

std::optional<Vector3> StreamingClient::characterPosition(Replica& replica) const
{
    auto game = replica.game();
    auto* players = game ? dynamic_cast<Players*>(game->findService("Players")) : nullptr;
    if (!players)
        return std::nullopt;
    // The local Player, else ours by user id; a server can list a stale Player
    // of ours with no character next to the live one [live].
    std::vector<const Player*> candidates;
    if (const Player* local = players->localPlayer())
        candidates.push_back(local);
    for (const auto& child : players->children())
        if (const auto* p = dynamic_cast<const Player*>(child.get());
            p && options_.userId != 0 && p->userId() == options_.userId)
            candidates.push_back(p);
    for (const Player* me : candidates) {
        const Model* character = me->character();
        if (!character)
            continue;
        if (const auto* root = dynamic_cast<const BasePart*>(character->findFirstChild("HumanoidRootPart")))
            return root->position();
        if (const BasePart* primary = character->primaryPart())
            return primary->position();
    }
    return std::nullopt;
}

std::vector<Vector3> StreamingClient::keepPoints(Replica& replica, std::uint64_t nowMs) const
{
    std::vector<Vector3> points;
    if (focus_)
        points.push_back(*focus_);
    if (auto c = characterPosition(replica))
        points.push_back(*c);
    for (const Prefetch& p : prefetches_)
        if (!p.done && (!p.sent || static_cast<double>(nowMs - p.sentMs) < p.timeout * 1000.0))
            points.push_back(p.position);
    for (const auto& [id, f] : serverFoci_)
        points.push_back(regionCenter(f.region));
    // Where the server streams around (our character as it sees it): what it
    // streams there would only come back.
    if (stats_.lastInfo && stats_.lastInfo->region != kNoRegion)
        points.push_back(regionCenter(stats_.lastInfo->region));
    return points;
}

void StreamingClient::takeBatches(Replica& replica, std::uint64_t nowMs)
{
    for (StreamedBatch& batch : std::exchange(replica.runtime.streamedBatches, {})) {
        active_ = true;
        ++stats_.batches;
        stats_.instancesStreamed += batch.instances.size();
        bool named = false;
        for (const StreamRegionRef& ref : batch.regions) {
            previousRegion_ = resolveRegion(ref, previousRegion_);
            named = named || previousRegion_ != kNoRegion;
        }
        // Pinned instances and the ones streamed outside any region (the
        // sentinel) stay; parts sit in the region of their position, the
        // rest of what came goes with the parts it is under.
        if (batch.pinned || !named)
            continue;
        for (InstanceId id : batch.instances) {
            InstancePtr instance = replica.find(id);
            const auto* part = dynamic_cast<const BasePart*>(instance.get());
            if (!part)
                continue;
            const RegionKey k = keyOf(regionOf(part->position()));
            regions_[k].parts.push_back(instance);
            if (auto dropped = droppedAt_.find(k); dropped != droppedAt_.end()) {
                if (nowMs - dropped->second < options_.bounceMs)
                    returned_.insert(k); // a bounce if it is still far at the next pass
                droppedAt_.erase(dropped);
            }
        }
    }
}

void StreamingClient::streamOut(Replica& replica, std::uint64_t nowMs, std::vector<RuntimeItem>& out)
{
    float radius = options_.streamOutStuds;
    if (radius < 0) {
        auto game = replica.game();
        const auto* ws = game ? dynamic_cast<const Workspace*>(game->findService("Workspace")) : nullptr;
        radius = ws ? static_cast<float>(ws->streamingTargetRadius()) : 0.0f;
    }
    if (radius <= 0 || regions_.empty())
        return;
    if (lastStreamOutMs_ != 0 && nowMs - lastStreamOutMs_ < options_.streamOutIntervalMs)
        return;
    lastStreamOutMs_ = nowMs;
    const std::vector<Vector3> keep = keepPoints(replica, nowMs);
    if (keep.empty())
        return; // nothing to measure from yet
    // A region goes when its nearest corner is past the radius from every focus.
    const float halfDiagonal = kRegionStuds * std::sqrt(3.0f) / 2;
    std::vector<std::pair<float, RegionKey>> far;
    for (const auto& [k, held] : regions_) {
        if (auto h = heldUntil_.find(k); h != heldUntil_.end() && nowMs < h->second)
            continue;
        const Vector3 c = regionCenter(regionAt(k));
        float nearest = distance(c, keep.front());
        for (const Vector3& p : keep)
            nearest = std::min(nearest, distance(c, p));
        const bool isFar = nearest - halfDiagonal > radius;
        if (returned_.erase(k) && isFar) {
            heldUntil_[k] = nowMs + options_.holdMs; // sent back though nothing of ours is near: the server wants it
            ++stats_.bounced;
            continue;
        }
        if (isFar)
            far.emplace_back(nearest, k);
    }
    if (far.empty())
        return;
    std::ranges::sort(far, std::greater<>{}, &std::pair<float, RegionKey>::first);
    if (far.size() > options_.maxRegionsPerPass)
        far.resize(options_.maxRegionsPerPass);
    std::set<const Instance*> characters;
    if (auto game = replica.game())
        if (Instance* players = game->findService("Players"))
            for (const auto& child : players->children())
                if (const auto* p = dynamic_cast<const Player*>(child.get()); p && p->character())
                    characters.insert(p->character());
    for (const auto& [d, k] : far) {
        auto it = regions_.find(k);
        if (it == regions_.end())
            continue;
        RegionRemovalItem removal;
        removal.region = regionAt(k);
        std::vector<std::weak_ptr<Instance>> stay;
        std::set<const Instance*> seen; // a part streamed again under its old id is listed twice
        for (const auto& weak : std::exchange(it->second.parts, {})) {
            InstancePtr instance = weak.lock();
            const auto* part = dynamic_cast<const BasePart*>(instance.get());
            if (!part || !part->parent() || !seen.insert(part).second)
                continue; // gone already (destroyed by the server or a script)
            if (const RegionId now = regionOf(part->position()); keyOf(now) != k) {
                regions_[keyOf(now)].parts.push_back(weak); // it moved: it belongs to where it is
                continue;
            }
            // A part bigger than its region (a baseplate) stays while any of it is near.
            float reach = distance(part->position(), keep.front());
            for (const Vector3& p : keep)
                reach = std::min(reach, distance(part->position(), p));
            if (characters.contains(part) || kept(*part, characters) || reach - part->size().magnitude() / 2 <= radius) {
                stay.push_back(weak);
                continue;
            }
            auto id = replica.idOf(part);
            if (!id)
                continue; // not the server's (made by a script)
            removal.removed.push_back(*id);
            for (const InstancePtr& d2 : part->descendants())
                if (auto did = replica.idOf(d2.get()))
                    removal.removed.push_back(*did);
        }
        it = regions_.find(k); // the moves above may have added regions
        if (stay.empty())
            regions_.erase(it);
        else
            it->second.parts = std::move(stay);
        if (removal.removed.empty())
            continue;
        removal.sequence = ++removalSequence_;
        droppedAt_[k] = nowMs;
        for (InstanceId id : removal.removed)
            replica.remove(id);
        ++stats_.regionsRemoved;
        stats_.instancesRemoved += removal.removed.size();
        out.emplace_back(RuntimeItem{std::move(removal)});
    }
}

std::vector<std::vector<std::uint8_t>> StreamingClient::tick(Replica& replica, std::uint64_t nowMs)
{
    RuntimeState& rt = replica.runtime;
    takeBatches(replica, nowMs);
    for (const StreamingInfoItem& info : std::exchange(rt.streamingInfos, {})) {
        active_ = true;
        stats_.lastInfo = info;
    }
    for (const StreamPrefetchInfoItem& info : std::exchange(rt.prefetchInfos, {}))
        for (Prefetch& p : prefetches_)
            if (p.sent && !p.done && (p.id & 0xff) == (info.b & 0xff)) {
                p.done = true;
                ++stats_.streamAroundDone;
                break;
            }
    for (const StreamFocusItem& f : std::exchange(rt.focuses, {}))
        serverFoci_[f.focusId] = f;
    for (std::uint32_t id : std::exchange(rt.focusRemovals, {}))
        serverFoci_.erase(id);
    if (replica.globals && replica.globals->streamingEnabled)
        active_ = true;
    if (!active_)
        return {};

    std::vector<RuntimeItem> items;
    streamOut(replica, nowMs, items);
    // The focus: the area around it, asked for again each time it has moved far enough.
    if (focus_ && (!lastFocusRequest_ || distance(*focus_, *lastFocusRequest_) >= options_.refocusStuds)) {
        requestStreamAround(*focus_, options_.streamAroundTimeout);
        lastFocusRequest_ = focus_;
    }
    for (Prefetch& p : prefetches_)
        if (!p.sent) {
            items.emplace_back(RuntimeItem{StreamPrefetchRequestItem{p.position, p.timeout, p.id}});
            p.sent = true;
            p.sentMs = nowMs;
            ++stats_.streamAroundSent;
        }
    std::erase_if(prefetches_, [&](const Prefetch& p) {
        return p.sent && (p.done || static_cast<double>(nowMs - p.sentMs) > p.timeout * 1000.0 + 5000.0);
    });
    // The quota and the radius, when either changed (at most every 0.125 s).
    if (!quotaStarted_ || nowMs - lastQuotaMs_ >= options_.quotaIntervalMs) {
        // The initial quota first, the steady one once streamed data has come
        // and been taken in (GUESS: the genuine client measures its pace).
        const std::int32_t want = std::min(
            quotaStarted_ && stats_.batches > 0 ? options_.steadyQuota : options_.initialQuota, options_.quotaCap);
        if (!quotaStarted_ || want != quotaSent_ || options_.maxRegionRadius != radiusSent_) {
            ClientCapacityUpdateItem update;
            update.quotaDelta = want - quotaSent_;
            update.maxRegionRadius = options_.maxRegionRadius;
            items.emplace_back(RuntimeItem{update});
            quotaSent_ = want;
            radiusSent_ = options_.maxRegionRadius;
            quotaStarted_ = true;
            ++stats_.quotaUpdates;
            stats_.quota = want;
        }
        lastQuotaMs_ = nowMs;
    }
    if (items.empty())
        return {};
    static const Schema kNoSchema;
    std::vector<Item> wrapped(std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
    std::vector<std::vector<std::uint8_t>> messages;
    messages.push_back(encodeData(wrapped, replica.schema() ? *replica.schema() : kNoSchema, replica.context(),
                                  replica.framing, ItemContext{false, options_.capabilities}));
    return messages;
}

} // namespace lunar::net

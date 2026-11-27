#pragma once

#include <lunar/Math.hpp>
#include <lunar/net/Streaming.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace lunar {
class Instance;
}

namespace lunar::net {

class Replica;

// The client's side of content streaming (Workspace.StreamingEnabled), the
// job RobloxPlayer 0.742's ClientReplicator::GCJob does (notes-stream.md,
// 2026-10-12 sections):
//
// - the instance quota and the farthest region radius it wants streamed
//   (Client Quota Stream Update, every 0.125 s at most when either changed);
// - Player:RequestStreamAroundAsync as Stream Prefetch Requests, answered by
//   Stream Prefetch Info items once the server has streamed the area;
// - the streamed instances by region, and dropping the regions far from
//   every focus: the instances leave the replica and a Region Removal tells
//   the server, which streams them again when they are wanted again.
//
// The server streams around our character (or Player.ReplicationFocus)
// itself; a focus the client sets (a camera, a scripted path) is asked for
// with stream-around requests as it moves. Regions are 64-stud cubes.
inline constexpr float kRegionStuds = 64.0f; // [code: >> 6 in the GC job and RequestStreamAroundAsync]
RegionId regionOf(Vector3 position);
Vector3 regionCenter(RegionId region);
// The resolved region a Stream Data list entry names, given the one before it
// (codes 1/2/3 step y/z/x, 4 is the "no region" sentinel) [code: writer 0x1056e6bbe].
inline constexpr RegionId kNoRegion{0x7fffffff, 0x7fffffff, 0x7fffffff};
RegionId resolveRegion(const StreamRegionRef& ref, RegionId previous);

struct StreamingClientOptions {
    std::uint64_t capabilities = kBaseCapabilities; // negotiated: the optional item fields
    std::int64_t userId = 0; // our Player (its character is a focus and is never streamed out)
    std::int32_t initialQuota = 2000; // DFInt ClientInstanceQuotaInitial [code: 0x1054e6571]
    std::int32_t quotaCap = 10000;    // DFInt ClientInstanceQuotaCap [code: 0x1054e6535]
    // The quota once streamed data flows. The client computes it from how long
    // it takes to process an instance, up to the cap (0x1054d64c0); Lunarstone
    // processes a Stream Data item in well under a frame. GUESS: the cap.
    std::int32_t steadyQuota = 10000;
    std::int16_t maxRegionRadius = kUnlimitedRegionRadius; // [code: GC job+0x2cc starts at 0x7fff]
    std::uint32_t quotaIntervalMs = 125;                   // [code: 0x1054d64c0, 0.125 s]
    // Stream out: regions farther than this from every focus (ours, the
    // character, live stream-around requests, the server's replication foci
    // and the center its Streaming Info names) are dropped. 0 never drops (the genuine default, StreamOutBehavior
    // LowMemory, only drops under memory pressure); negative: the place's
    // Workspace.StreamingTargetRadius (StreamOutBehavior Opportunistic).
    float streamOutStuds = -1;
    std::uint32_t streamOutIntervalMs = 1000;
    std::size_t maxRegionsPerPass = 32;
    // A region the server streams again this soon after we dropped it is one
    // it wants us to have (it streams around something we don't count as a
    // focus): it is left alone for holdMs. GUESS: the genuine client lowers
    // the radius it asks for instead ("INFO REDUCE" in the GC job's log).
    std::uint32_t bounceMs = 30000, holdMs = 120000;
    // Stream-around requests for the focus: a new one when it has moved this
    // far from the last one's center, each with this timeout.
    float refocusStuds = 64;
    double streamAroundTimeout = 30;
};

class StreamingClient {
public:
    explicit StreamingClient(StreamingClientOptions options = {});

    // Where the client is looking from, each frame (a windowed client's camera
    // or character, --net-join --focus); nullopt for none. Moving it asks the
    // server for the area around it (Stream Prefetch Request) and keeps that
    // area from being streamed out.
    void setFocus(std::optional<Vector3> focus) { focus_ = focus; }
    std::optional<Vector3> focus() const { return focus_; }
    // Player:RequestStreamAroundAsync: sent with the next tick; the id to wait on.
    std::int32_t requestStreamAround(Vector3 position, double timeoutSeconds);
    bool streamAroundDone(std::int32_t id) const;

    // Takes what the replica received since the last call (streamed
    // instances, Streaming Info, prefetch answers, foci) and returns the
    // ID_DATA messages to send now (reliable ordered), streaming out what is
    // too far away first.
    std::vector<std::vector<std::uint8_t>> tick(Replica& replica, std::uint64_t nowMs);

    bool active() const { return active_; } // the server streams to us
    const StreamingClientOptions& options() const { return options_; }

    // What happened, for logs and tests.
    struct Stats {
        std::size_t batches = 0, instancesStreamed = 0;
        std::size_t regionsRemoved = 0, instancesRemoved = 0;
        std::size_t quotaUpdates = 0, streamAroundSent = 0, streamAroundDone = 0;
        std::size_t bounced = 0; // regions the server sent again right after we dropped them
        std::int32_t quota = 0; // what the server has added up
        std::optional<StreamingInfoItem> lastInfo;
    };
    const Stats& stats() const { return stats_; }
    std::size_t regionsHeld() const { return regions_.size(); }
    std::size_t partsHeld() const;
    // One line per region held (center, parts), nearest first from `from`.
    std::vector<std::string> describe(std::optional<Vector3> from = std::nullopt) const;

private:
    struct Prefetch {
        std::int32_t id = 0;
        Vector3 position;
        std::uint64_t sentMs = 0;
        double timeout = 0;
        bool sent = false, done = false;
    };
    struct Held {
        std::vector<std::weak_ptr<Instance>> parts; // streamed BaseParts that were in the region when they came
    };
    void takeBatches(Replica& replica, std::uint64_t nowMs);
    void streamOut(Replica& replica, std::uint64_t nowMs, std::vector<RuntimeItem>& out);
    std::vector<Vector3> keepPoints(Replica& replica, std::uint64_t nowMs) const;
    std::optional<Vector3> characterPosition(Replica& replica) const;

    StreamingClientOptions options_;
    bool active_ = false;
    std::optional<Vector3> focus_;
    std::optional<Vector3> lastFocusRequest_;
    std::vector<Prefetch> prefetches_;
    std::int32_t nextPrefetchId_ = 1; // GC job+0x2d8 starts at 1 [code: 0x1056ba894]
    std::int32_t quotaSent_ = 0;      // the quota the server has added up from our updates
    std::int16_t radiusSent_ = 0;
    bool quotaStarted_ = false;
    std::uint64_t lastQuotaMs_ = 0, lastStreamOutMs_ = 0;
    RegionId previousRegion_ = kNoRegion; // the delta codes' state [code: rep+0x2ba0 on the writer]
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, Held> regions_;
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, std::uint64_t> droppedAt_, heldUntil_;
    std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> returned_; // streamed again soon after a drop
    std::map<std::uint32_t, StreamFocusItem> serverFoci_;
    std::uint32_t removalSequence_ = 0; // GC job+0xed0 [code: 0x1056cebd4]
    Stats stats_;
};

} // namespace lunar::net

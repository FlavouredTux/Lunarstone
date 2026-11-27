#pragma once

#include <lunar/Signal.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/Streaming.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lunar {
class Humanoid;
class Instance;
class Tool;
class Weld;
} // namespace lunar

namespace lunar::net {

// What a client of a live server sends of its own tools, which
// FilteringEnabled lets it do for itself (--net-join --scripts):
// - a tool going between the local player's Backpack and character, or
//   dropped from the character into the Workspace: Change Property of Parent
//   (the id past the schema's last property), flag 0 (ClientReplicator
//   vfn_137 writes 1 and a version only for a change it holds one for) [code];
// - the RightGrip weld the client's ToolSystem makes for a tool from the
//   Backpack, which the server waits for instead of welding the tool itself
//   (Tool vfn_60 / 0x1043c2e82) [code]: a New Instance in the client's own id
//   scope, after the tool's Parent; Part0 and Part1 in the first property
//   pass, C0, C1 and Name in the second, as the genuine writer splits by type
//   (0x1055d2fdc) and as servers send welds [capture]. A Delete when the
//   client takes it away; other players' RightGrips reach us in their
//   client's scope [capture];
// - Tool.Activated / Deactivated and Humanoid.ServerEquipTool: Event
//   Invocations; both Tool events are on the list of events a client may
//   send (0x10579f2aa) [code].
// Changes the server made (while the Replica applies its messages) aren't
// sent back; the weld goes out whoever moved the tool, as the client's
// ToolSystem made it.
class ToolReplicator {
public:
    explicit ToolReplicator(std::shared_ptr<Replica> replica);
    ~ToolReplicator();
    ToolReplicator(const ToolReplicator&) = delete;
    ToolReplicator& operator=(const ToolReplicator&) = delete;

    // What happened since the last call, as ID_DATA messages, to be sent
    // reliable and ordered.
    std::vector<std::vector<std::uint8_t>> take(const ItemContext& context = {false, kBaseCapabilities});

    // A line per item queued ("tool: ... Parent = ...").
    std::function<void(const std::string&)> log;
    std::size_t sent = 0, dropped = 0; // items (dropped: no id or no schema entry)

private:
    struct Watched {
        std::weak_ptr<Tool> tool;
        std::weak_ptr<Instance> parent; // the parent the server knows
        std::vector<Connection> connections;
    };
    struct WatchedHumanoid {
        std::weak_ptr<Instance> humanoid;
        std::vector<Connection> connections;
    };
    struct Grip {
        std::weak_ptr<Instance> weld;
        std::weak_ptr<Instance> character; // where it was made
        InstanceId id;
        Connection connection;
    };

    void added(Instance& instance);
    void watchTool(Tool& tool);
    void watchHumanoid(Humanoid& humanoid);
    void moved(Tool& tool);
    void gripped(Weld& weld);
    void ungripped(const Instance* weld);
    void fired(Instance& instance, std::string_view event, std::span<const Value> arguments);
    bool ours(const Instance* container) const;
    void queue(Item item, std::string line);

    std::shared_ptr<Replica> replica_;
    std::vector<Connection> connections_;
    std::map<const Tool*, Watched> tools_;
    std::map<const Instance*, WatchedHumanoid> humanoids_;
    std::map<const Instance*, Grip> grips_;
    std::vector<Item> items_;
};

} // namespace lunar::net

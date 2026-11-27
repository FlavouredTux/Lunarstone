#pragma once

#include <lunar/Signal.hpp>
#include <lunar/Value.hpp>
#include <lunar/net/Describe.hpp>
#include <lunar/net/JoinData.hpp>
#include <lunar/net/NetworkStream.hpp>
#include <lunar/net/Session.hpp>
#include <lunar/net/Values.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace lunar {
class BasePart;
class Instance;
class DataModel;
} // namespace lunar

namespace lunar::net {

class ReplicatedMotion;

// The Roblox layer's replication messages (ROBLOX-LAYER.md sections 1, 3, 5):
// the network schema, ID_DATA and its items, and the id remap / hash
// wrapper some capability sets add. Layouts from RobloxPlayer 0.742.

// -- framing (ROBLOX-LAYER.md 1)
struct Framing {
    bool remapIds = false; // capability bit 12: packet ids and item types go through fixed tables
    bool hashed = false;   // capability bit 55: 0xAA, FNV-1a 32 of the rest, then the packet
    static Framing fromCapabilities(std::uint64_t negotiated);
};
std::uint8_t encodePacketId(std::uint8_t id);   // table at 0x10680f920
std::uint8_t decodePacketId(std::uint8_t wire); // table at 0x10680fa20
std::uint8_t encodeItemType(std::uint8_t type); // table at 0x10680fb20
std::uint8_t decodeItemType(std::uint8_t wire);
std::uint32_t fnv1a32(std::span<const std::uint8_t> bytes);
// Applies the framing to a whole message (id byte first).
std::vector<std::uint8_t> frame(std::vector<std::uint8_t> message, const Framing& framing);
// Undoes it: the plain message, or an error (hash mismatch: "PacketCorruption").
std::expected<std::vector<std::uint8_t>, std::string> unframe(std::span<const std::uint8_t> wire,
                                                              const Framing& framing);

// -- schema (ROBLOX-LAYER.md 5)
struct SchemaProperty {
    std::string name;
    std::uint8_t type = 0;
    std::uint16_t enumId = 0xffff;
    std::uint64_t extra = 0; // type 0x3c only
    std::uint16_t owner = 0; // class index
};
struct SchemaEvent {
    std::string name;
    std::vector<std::pair<std::uint8_t, std::uint16_t>> params; // network type, enum id
    std::uint16_t owner = 0;
};
struct SchemaClass {
    std::string name;
    std::uint16_t firstProperty = 0, propertyCount = 0;
    std::uint16_t firstEvent = 0, eventCount = 0;
    std::uint16_t tag = 0xffff;
};
struct SchemaEnum {
    std::string name;
    std::uint8_t size = 0;
};

struct Schema {
    std::uint64_t flags = 0;
    std::vector<SchemaEnum> enums;
    std::vector<SchemaClass> classes;
    std::vector<SchemaProperty> properties; // global table: property network id = index
    std::vector<SchemaEvent> events;        // global table: event network id = index
    // The trailing sections (ROBLOX-LAYER.md 5, notes-joindata.md 5), each
    // present only when bytes remain: the ContentId prefixes (type 0x21's
    // prefix index; [0] is always ""), the string dictionary (type 0x31's
    // ids), the classes with ISR properties (class id + global property
    // ids) and the "lr type range" list (GUESS: NetAsset types).
    std::vector<std::string> contentPrefixes{""};
    std::vector<std::string> stringDictionary;
    struct IsrClass {
        std::uint32_t classId = 0;
        std::vector<std::uint32_t> properties;
    };
    std::vector<IsrClass> isrClasses;
    struct LrTypeRange {
        std::uint64_t type = 0, a = 0, b = 0, c = 0;
    };
    std::vector<LrTypeRange> lrTypeRanges;

    // Lunarstone's own server: a schema for the engine's classes, each
    // property's network type from its Luau type. GUESS: the real server's
    // schema has Roblox's full reflection; only the layout is the same.
    static Schema fromEngine();

    std::optional<std::uint16_t> classId(std::string_view name) const;
    // Property network id of `name` on class `classId` (searching the class's own range).
    std::optional<std::uint16_t> propertyId(std::uint16_t classId, std::string_view name) const;
    std::optional<std::uint16_t> eventId(std::uint16_t classId, std::string_view name) const;

    std::vector<std::uint8_t> encodeBody() const; // uncompressed
    static std::expected<Schema, std::string> decodeBody(std::span<const std::uint8_t> body);
};

// The schema message: id, then u32 compressed length, u32 raw length, zstd bytes.
inline constexpr std::uint8_t kSchemaPacketId = 0x97; // [code] ClientReplicator::vfn_152: 0x97 = schema, 0x81 = globals
std::vector<std::uint8_t> encodeSchemaPacket(const Schema& schema);
std::expected<Schema, std::string> decodeSchemaPacket(std::span<const std::uint8_t> message);

// -- join (ROBLOX-LAYER.md 6)
inline constexpr std::uint8_t kJoinRequestId = 0x90;     // client -> server after the connection is accepted
inline constexpr std::uint8_t kPlaceIdVerifiedId = 0x92; // server -> client: u8 ok
inline constexpr std::uint8_t kKickId = 0x98;            // server -> client: string message [, string metadata]
inline constexpr std::uint8_t kMarkerId = 0x84;          // u32 marker

// The client's join request as the server reads it (ServerReplicator::vfn_89 case 0x90).
struct JoinRequest {
    std::uint8_t flags = 0;        // GUESS: protocol variant
    std::uint32_t clientValue = 0; // GUESS: protocol/client version
    std::uint8_t luauMin = 0, luauMax = 0;
    std::vector<std::string> extra; // u16-counted strings, only if bytes remain (Lunarstone sends none)
    std::string joinInfo;           // JSON from the join API (ticket, user ...); opaque here
};
std::vector<std::uint8_t> encodeJoinRequest(const JoinRequest& request, std::uint64_t negotiatedCapabilities);
std::expected<JoinRequest, std::string> decodeJoinRequest(std::span<const std::uint8_t> message,
                                                          std::uint64_t negotiatedCapabilities);

// -- ID_DATA items (ROBLOX-LAYER.md 3)
namespace item {
inline constexpr std::uint8_t End = 0x00;
inline constexpr std::uint8_t Delete = 0x01;
inline constexpr std::uint8_t New = 0x02;
inline constexpr std::uint8_t ChangeProperty = 0x03;
inline constexpr std::uint8_t Marker = 0x04;
inline constexpr std::uint8_t EventInvocation = 0x07;
} // namespace item
const char* itemTypeName(std::uint8_t type);

struct NewInstanceItem {
    InstanceId id;
    std::uint16_t classId = 0;
    std::vector<std::pair<std::uint16_t, Value>> properties;    // pass 1 (GUESS: non-references)
    std::vector<std::pair<std::uint16_t, Value>> refProperties; // pass 2 (GUESS: references)
    InstanceId parent;
    // Decoding: references to instances the reader doesn't know yet (they
    // come later in the join); the Replica fills them in when they arrive.
    std::vector<std::pair<std::uint16_t, InstanceId>> forwardRefs;
    // Decoding: shared-string properties whose content key wasn't in the
    // cache (the value holds the key's hex until the content arrives).
    std::vector<std::pair<std::uint16_t, SharedStringKey>> pendingSharedStrings;
};
struct ChangePropertyItem {
    InstanceId id;
    std::uint16_t propertyId = 0;
    Value value;
    // Between the property id and the value, the receiver's vfn_138 reads a
    // u8 flag (ClientReplicator 0x1054d7d5c) and, on the server and when it is
    // set, a zigzag32 (ServerReplicator 0x105687a5e) [code]. Meaning GUESS.
    bool flag = false;
    std::int32_t flagValue = 0; // client -> server only, when flag is set
    // Parent has no schema entry: a property id one past the last one says
    // the value is the instance's new parent, an instance id [live].
    std::optional<InstanceId> parent;
    // Decoding: a reference to an instance the reader doesn't know yet (the
    // value is nil). The server sends Player.Character before the character
    // model, in the same message [capture]; the Replica sets it on arrival.
    InstanceId forwardRef;
};
struct DeleteItem {
    InstanceId id;
};
struct MarkerItem {
    std::uint32_t marker = 0;
};
struct EventInvocationItem {
    InstanceId id;
    std::uint16_t eventId = 0;
    std::vector<Value> arguments; // flattened: a Tuple parameter contributes all its members
    // The arguments that are tables (Array and Dictionary variants inside a
    // Tuple: what remotes carry for Luau tables), by their index in
    // `arguments`, where the Value is nil.
    std::map<std::size_t, std::shared_ptr<const RemoteTable>> tables;
    // The ones that are Randoms, as their generators' states, likewise.
    std::map<std::size_t, std::uint64_t> randoms;
};
// An event invocation's arguments with the tables in place, and an item made from such arguments.
std::vector<RemoteValue> remoteArguments(const EventInvocationItem& item);
EventInvocationItem eventInvocation(InstanceId id, std::uint16_t eventId, std::span<const RemoteValue> arguments);
using Item = std::variant<NewInstanceItem, ChangePropertyItem, DeleteItem, MarkerItem, EventInvocationItem,
                          // join-time items (JoinData.hpp)
                          JoinDataV2Item, JoinDataItem, TagItem, ServerTimeItem, ResolveAtomicTreeItem,
                          ChangeAttributeItem, ChangeInstItem,
                          // the post-join items (pings, streaming ...; Streaming.hpp)
                          RuntimeItem>;

// The property list of New Instance and join instances: repeat { u8 delta;
// value } until 0xff, ids relative to the class's first property and
// increasing. Reading fills `properties` (or `refProperties` when refPass),
// `forwardRefs` and `pendingSharedStrings`.
void writePropertyList(NetworkStream& out, const std::vector<std::pair<std::uint16_t, Value>>& props,
                       const SchemaClass& cls, const Schema& schema, const ValueContext& context);
bool readPropertyList(NetworkStream& in, NewInstanceItem& into, bool refPass, const SchemaClass& cls,
                      const Schema& schema, const ValueContext& context, Node* tree, std::string& error);
// The join-time items, one call each way from encodeData/decodeData (JoinData.cpp).
bool encodeJoinItem(NetworkStream& out, const Item& item, const Schema& schema, const ValueContext& context,
                    const Framing& framing);
bool decodeJoinItem(std::uint8_t type, NetworkStream& in, const Schema& schema, const ValueContext& context,
                    Node* tree, std::string& error, std::vector<Item>& items);

// Builds an ID_DATA message (0x83 ... 0x00). `items` says who writes (pings
// differ by role) and which capabilities frame the optional fields.
std::vector<std::uint8_t> encodeData(std::span<const Item> items, const Schema& schema, const ValueContext& context,
                                     const Framing& framing = {}, const ItemContext& itemContext = {});
// Reads one. `tree` (if given) gets the readable form, item by item; unknown
// item types stop the read with their bytes dumped. With `onItem`, each item
// goes there as soon as it's read (and isn't returned), so a caller that
// applies it lets the next items' refs resolve to what it made.
std::expected<std::vector<Item>, std::string> decodeData(std::span<const std::uint8_t> message, const Schema& schema,
                                                         const ValueContext& context, Node* tree = nullptr,
                                                         const Framing& framing = {},
                                                         const ItemContext& itemContext = {},
                                                         const std::function<void(Item&&)>& onItem = {});

// The event invocations a Replica received, numbered from 0 in the order
// they came, of which it keeps the latest `limit` (0: all): readers keep
// their place by number and read on from it each frame. A game sends them
// for as long as it runs (the game: 30,000 in 70 s), so all of them would grow
// without end.
class EventLog {
public:
    static constexpr std::size_t defaultLimit = 4096;

    // How many have come, kept or not: one past the newest's number.
    std::size_t size() const { return first_ + items_.size(); }
    bool empty() const { return size() == 0; }
    // The oldest one still kept's number.
    std::size_t first() const { return first_; }
    // The one numbered i, which must be kept (first() <= i < size()).
    const EventInvocationItem& operator[](std::size_t i) const { return items_[i - first_]; }
    const EventInvocationItem& back() const { return items_.back(); }
    // The kept ones, oldest first.
    std::span<const EventInvocationItem> kept() const { return items_; }

    void push_back(EventInvocationItem item)
    {
        items_.push_back(std::move(item));
        // Down to the limit once twice it are kept: each item moves once.
        if (limit_ != 0 && items_.size() >= limit_ * 2) {
            const std::size_t drop = items_.size() - limit_;
            items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(drop));
            first_ += drop;
        }
    }
    std::size_t limit() const { return limit_; }
    void setLimit(std::size_t limit) { limit_ = limit; }

private:
    std::vector<EventInvocationItem> items_;
    std::size_t first_ = 0;
    std::size_t limit_ = defaultLimit;
};

// The client side: a DataModel built from what the server replicates.
// The decoder uses one per flow too, so instance ids print as paths.
class Replica {
public:
    Replica();
    Replica(const Replica&) = delete;
    Replica& operator=(const Replica&) = delete;

    // Applies one Roblox-layer message (unframed). `tree` gets the readable
    // form. False when it isn't one this understands.
    bool handle(std::span<const std::uint8_t> message, Node* tree = nullptr);

    std::shared_ptr<DataModel> game() const { return game_; }
    const Schema* schema() const { return haveSchema_ ? &schema_ : nullptr; }
    InstancePtr find(InstanceId id) const;
    std::optional<InstanceId> idOf(const Instance* instance) const;
    ValueContext context() const;
    std::size_t instanceCount() const { return instances_.size(); }
    // Every instance by key (scope << 32 | index), in key order.
    std::vector<std::pair<std::uint64_t, InstancePtr>> instances() const;
    // Instances still waiting for their parent to arrive, by the parent's key.
    const std::map<std::uint64_t, std::vector<std::weak_ptr<Instance>>>& pendingParents() const
    {
        return pendingParents_;
    }
    std::vector<std::uint32_t> markers;      // Marker items received, in order
    // The server's changes to an instance's CFrame, with the change's flag
    // (the client starts its count of them again at 0 when it's set,
    // 0x1054d787a). CharacterSync acknowledges those of our character's parts
    // and takes them; until it does the server drops our motion [code].
    struct CFrameChange {
        InstanceId id;
        std::uint16_t propertyId = 0;
        bool restart = false;
    };
    std::vector<CFrameChange> cframeChanges;
    EventLog events; // event invocations received (the latest of them)
    // Event invocations the owner takes instead of the engine event firing
    // on the instance (remotes, for the client's scripts): true when taken.
    // They still go to `events`.
    std::function<bool(const InstancePtr& target, std::string_view event, const EventInvocationItem& item)> takeEvent;
    // An event invocation of ours (a script's FireServer ...) as an item for
    // encodeData: nothing when the instance isn't replicated or its class has
    // no such event in the schema.
    std::optional<EventInvocationItem> eventItem(const Instance& instance, std::string_view event,
                                                 std::span<const RemoteValue> arguments) const;
    // The network types of the parameters of the instance's event, in order,
    // as the schema has them: nothing when its class has no such event.
    std::optional<std::vector<std::uint8_t>> eventParameters(const Instance& instance, std::string_view event) const;
    // ID_DATA messages that stopped on an item this can't read, and the first few reasons.
    std::size_t decodeFailures = 0;
    std::vector<std::string> decodeErrors;
    Framing framing;
    std::uint64_t capabilities = 0x4000b10810ULL; // negotiated, for the join request's layout
    // Join-time state (JoinData.hpp): ID_SET_GLOBALS as received (the peer id
    // is the scope of ids this client creates; the top containers become the
    // services), the tags and server time seen, and the shared strings this
    // connection exchanged.
    std::optional<SetGlobals> globals;
    std::vector<std::uint32_t> tags;
    std::optional<std::int64_t> serverTime;
    bool joinDataDone = false; // Tag 13
    SharedStringCache& sharedStrings() const { return sharedStrings_; }
    // The post-join traffic (Session.hpp): what arrived for the session layer
    // to act on, and whether the messages come from the server (the decoder
    // feeds one Replica both directions).
    RuntimeState runtime;
    bool decodingServerToClient = true;
    // Forgets an instance the server destroyed or the client streamed out
    // (Destroy items, StreamingClient): destroys it and drops its id, so the
    // server can send it again as a new instance.
    void remove(InstanceId id);

    // True while handle() applies what the server sent: changes made then
    // are the server's, not the client's to send back.
    bool applying() const { return applying_ > 0; }
    // An instance the client made and replicates itself (a tool's RightGrip):
    // an id in the client's own scope (ID_SET_GLOBALS' peer id), known by it
    // from now on. Nothing before ID_SET_GLOBALS. Indexes count from 1
    // (GUESS: the genuine client numbers everything it creates, so its ids
    // run higher; the server only needs them unique).
    std::optional<InstanceId> addLocal(const InstancePtr& instance);
    // Forgets one of those without touching the instance.
    void forgetLocal(InstanceId id);
    // Physics replication (Motion.hpp): the motion the server sends for the
    // assemblies others own, from ISR (0xa1) and ID_PHYSICS (0x85, behind
    // ID_TIMESTAMP) packets.
    ReplicatedMotion& motion() { return *motion_; }
    const ReplicatedMotion& motion() const { return *motion_; }
    // Moves those assemblies to where they are at `now` (seconds on
    // receiveTime's clock). Call once a frame, before animating and drawing;
    // until the first call each sample is applied as it arrives. Roots
    // `simulatedHere` names (the client simulates them: the parts it owns,
    // see ownedHere) are left alone.
    void stepMotion(double now);
    std::function<bool(const BasePart&)> simulatedHere;
    // When the message being handled arrived, in local seconds (a decoder
    // sets the capture time); unset, clockSeconds() at the time.
    std::optional<double> receiveTime;
    static double clockSeconds(); // the steady clock
    // Motion samples for instances this replica doesn't have (streamed out ...).
    std::size_t motionForUnknown = 0;
    // Whether the motion a client sent (its ISR, when a capture's both
    // directions are decoded) is played back too. A replay that runs a
    // client of its own takes only the server's: the recorded client's
    // motion for its root read as the server taking the root over.
    bool motionFromClients = true;
    // Network ownership (BasePart.NetworkOwnerV3, the owner's peer id; the
    // server's parts carry 1, our character's root our own peer id
    // [capture]): the parts whose ownership the server gave this client, as
    // The game gives a side door to the player who opens it. Their owner
    // simulates them (LiveCharacter); the motion the server still sends for
    // them isn't played (see stepMotion). `ownershipChanges` counts the
    // changes.
    bool ownedHere(InstanceId id) const;
    std::vector<InstanceId> partsOwnedHere() const;
    std::uint64_t ownershipChanges() const { return ownershipChanges_; }

private:
    bool handlePhysics(std::span<const std::uint8_t> message, Node* tree);
    bool handleIsr(std::span<const std::uint8_t> message, Node* tree);
    void apply(Item item);
    void applyGlobals(const SetGlobals& g);
    void bind(std::uint64_t key, const InstancePtr& instance);
    void unbind(std::uint64_t key);

    Schema schema_;
    bool haveSchema_ = false;
    std::shared_ptr<DataModel> game_;
    // By key: looked up for every item a server sends, and never walked in
    // order but by instances().
    std::unordered_map<std::uint64_t, InstancePtr> instances_;
    std::unordered_map<const Instance*, std::uint64_t> keys_; // idOf: instances_ the other way
    std::map<std::uint64_t, std::vector<std::pair<std::weak_ptr<Instance>, std::string>>> pendingRefs_;
    // Instances whose parent hadn't arrived yet, by the parent's id: the
    // server doesn't always send a parent before its children.
    std::map<std::uint64_t, std::vector<std::weak_ptr<Instance>>> pendingParents_;
    // Keyed-string properties whose content hasn't arrived (a later mode-1 blob brings it).
    std::map<SharedStringKey, std::vector<std::pair<std::weak_ptr<Instance>, std::string>>> pendingStrings_;
    mutable SharedStringCache sharedStrings_;
    int applying_ = 0;
    std::uint32_t nextLocalIndex_ = 1;
    void noteOwner(InstanceId id, std::string_view property, const Value& value);
    std::unordered_map<std::uint64_t, std::uint32_t> networkOwners_; // by key: NetworkOwnerV3 as last sent
    std::uint64_t ownershipChanges_ = 0;
    std::shared_ptr<ReplicatedMotion> motion_; // shared_ptr: Replica's destructor stays implicit
};

// The server side over a DataModel: ids for instances, the join messages and
// property changes as they happen (the engine's Changed signal).
class ReplicationServer {
public:
    // Replicates the DataModel's children of these classes ("Workspace", "Lighting" ...).
    ReplicationServer(std::shared_ptr<DataModel> game, const std::vector<std::string>& services);
    ~ReplicationServer();
    ReplicationServer(const ReplicationServer&) = delete;
    ReplicationServer& operator=(const ReplicationServer&) = delete;

    const Schema& schema() const { return schema_; }
    InstanceId idOf(const Instance* instance);
    InstancePtr find(InstanceId id) const;
    ValueContext context();

    // The join, shaped like a real server's (ROBLOX-LAYER.md 6, notes-joindata.md):
    // schema, ID_SET_GLOBALS naming the services as top containers, ID_DATA
    // messages with Join Data V2 items (shared strings first, then the
    // tree in batches of about `itemBytes`, parents first), a Tag 13 and a
    // Marker. JoinStyle::NewItems is the older shape: every instance
    // (DataModel and services included) as New Instance items.
    enum class JoinStyle { JoinDataV2, NewItems };
    std::vector<std::vector<std::uint8_t>> joinMessages(std::size_t itemBytes = std::size_t{16} * 1024,
                                                        JoinStyle style = JoinStyle::JoinDataV2);
    // The globals the join sends: peer id 2 for the client (GUESS: the server is 1), the services.
    SetGlobals globals() const;
    // ID_DATA messages for the properties and attributes changed since the last call.
    std::vector<std::vector<std::uint8_t>> takeChanges();
    // An event invocation (e.g. RemoteEvent.OnClientEvent) as an ID_DATA message.
    std::vector<std::uint8_t> invoke(const Instance& instance, std::string_view event, std::vector<Value> arguments);
    std::size_t replicatedCount() const { return order_.size(); }
    SharedStringCache& sharedStrings() { return sharedStrings_; }
    // Starts replicating an instance added after the join (its parent must be
    // replicated already): the ID_DATA message with its New Instance item.
    std::vector<std::uint8_t> addInstance(const InstancePtr& instance);
    // Streams instances (StreamingEnabled): the ID_DATA message with one Stream
    // Data item bringing them (parents before children; ones sent before keep
    // their ids, as a server streams a region again) and naming `regions`.
    std::vector<std::uint8_t> streamData(std::span<const InstancePtr> instances,
                                         std::vector<StreamRegionRef> regions);

private:
    void track(const InstancePtr& instance);
    NewInstanceItem newItem(const Instance& instance);

    std::shared_ptr<DataModel> game_;
    Schema schema_;
    std::map<const Instance*, std::uint32_t> ids_;
    std::map<std::uint32_t, std::weak_ptr<Instance>> byIndex_;
    std::vector<InstancePtr> order_;
    std::vector<InstancePtr> services_;
    std::vector<std::pair<const Instance*, std::string>> dirty_;
    std::vector<std::pair<const Instance*, std::string>> dirtyAttributes_;
    std::vector<lunar::Connection> connections_;
    SharedStringCache sharedStrings_;
};

// A describer for describeMessage (setRobloxDescriber) feeding a Replica.
MessageDescriber robloxDescriber(const std::shared_ptr<Replica>& replica);

} // namespace lunar::net

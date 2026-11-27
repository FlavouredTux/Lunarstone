// Our own character on a live server (net::CharacterSync): what the owner's
// client sends each frame, read back the way the server reads it.

#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/RemoteValue.hpp>
#include <lunar/net/CharacterSync.hpp>
#include <lunar/net/Physics.hpp>
#include <lunar/net/Replication.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace lunar;
using namespace lunar::net;

namespace {

// The engine's schema plus what a live server's has and the engine's
// classes don't: the Humanoid's network properties and the Animator's
// OnCombinedUpdate [capture]. A class's properties and events stay one run
// of the global tables, as on the wire.
void addProperty(Schema& s, std::string_view className, std::string name, std::uint8_t type)
{
    const std::uint16_t c = *s.classId(className);
    const std::uint16_t at = static_cast<std::uint16_t>(s.classes[c].firstProperty + s.classes[c].propertyCount);
    for (std::size_t i = 0; i < s.classes.size(); ++i)
        if (i != c && s.classes[i].firstProperty >= at)
            ++s.classes[i].firstProperty;
    s.properties.insert(s.properties.begin() + at, SchemaProperty{std::move(name), type, 0xffff, 0, c});
    ++s.classes[c].propertyCount;
}

void addEvent(Schema& s, std::string_view className, std::string name,
              std::vector<std::pair<std::uint8_t, std::uint16_t>> params)
{
    const std::uint16_t c = *s.classId(className);
    const std::uint16_t at = static_cast<std::uint16_t>(s.classes[c].firstEvent + s.classes[c].eventCount);
    for (std::size_t i = 0; i < s.classes.size(); ++i)
        if (i != c && s.classes[i].firstEvent >= at)
            ++s.classes[i].firstEvent;
    s.events.insert(s.events.begin() + at, SchemaEvent{std::move(name), std::move(params), c});
    ++s.classes[c].eventCount;
}

Schema liveSchema()
{
    Schema s = Schema::fromEngine();
    addProperty(s, "Humanoid", "NetworkHumanoidState", nettype::Enum);
    addProperty(s, "Humanoid", "JumpReplicate", nettype::Bool);
    addProperty(s, "Humanoid", "MoveDirectionInternal", nettype::Vector3);
    addEvent(s, "Animator", "OnCombinedUpdate",
             {{nettype::ContentId, 0xffff}, {nettype::Bool, 0xffff}, {nettype::Float, 0xffff},
              {nettype::Float, 0xffff}, {nettype::Float, 0xffff}, {nettype::Float, 0xffff},
              {nettype::Enum, 0xffff}, {nettype::Bool, 0xffff}, {nettype::Int, 0xffff}});
    return s;
}

// A live server's schema with its ISR section for parts: PhysicsRepRootPart,
// PhysicsRepRootRef and ReplicationPV, the three every BasePart class has
// [capture]; Name and Size stand in for the first two.
Schema isrSchema()
{
    Schema s = liveSchema();
    addProperty(s, "Part", "ReplicationPV", nettype::ReplicationPV);
    const std::uint16_t part = *s.classId("Part");
    s.isrClasses.push_back(
        {part, {*s.propertyId(part, "Name"), *s.propertyId(part, "Size"), *s.propertyId(part, "ReplicationPV")}});
    return s;
}

// A client's replica of a server that spawned our character: Workspace, the
// Model, its HumanoidRootPart, Humanoid and Animator, ids as a server gives them.
struct Session {
    std::shared_ptr<Replica> replica = std::make_shared<Replica>();
    Schema schema = liveSchema();
    Model* character = nullptr;
    BasePart* root = nullptr;
    Humanoid* humanoid = nullptr;
    static constexpr InstanceId rootId{1, 11};

    explicit Session(Schema withSchema = liveSchema()) : schema(std::move(withSchema))
    {
        CHECK(replica->handle(encodeSchemaPacket(schema)));
        SetGlobals globals;
        globals.peerId = 914;
        globals.containers.push_back({InstanceId{1, 1}, *schema.classId("Workspace")});
        CHECK(replica->handle(encodeSetGlobals(globals, replica->capabilities)));
        auto made = [&](InstanceId id, std::string_view className, std::string name, InstanceId parent) {
            NewInstanceItem n;
            n.id = id;
            n.classId = *schema.classId(className);
            n.properties.emplace_back(*schema.propertyId(n.classId, "Name"), Value(std::move(name)));
            n.parent = parent;
            return Item(std::move(n));
        };
        const std::vector<Item> items{made({1, 10}, "Model", "SamplePlayer", {1, 1}),
                                      made(rootId, "Part", "HumanoidRootPart", {1, 10}),
                                      made({1, 12}, "Humanoid", "Humanoid", {1, 10}),
                                      made({1, 13}, "Animator", "Animator", {1, 12})};
        CHECK(replica->handle(encodeData(items, schema, replica->context())));
        character = dynamic_cast<Model*>(replica->find({1, 10}).get());
        root = dynamic_cast<BasePart*>(replica->find(rootId).get());
        humanoid = dynamic_cast<Humanoid*>(replica->find({1, 12}).get());
        CHECK(character && root && humanoid && humanoid->rootPart() == root);
        root->setPosition({588.0f, 0.5f, -232.5f});
    }

    // What the server reads in one ID_DATA from us: the property changes and events.
    std::vector<std::string> read(const CharacterSync::Outgoing& out) const
    {
        std::vector<std::string> lines;
        auto items = decodeData(out.message, schema, replica->context(), nullptr, {},
                                ItemContext{false, replica->capabilities});
        CHECK(items.has_value());
        for (const Item& item : *items) {
            if (const auto* c = std::get_if<ChangePropertyItem>(&item))
                lines.push_back(std::format("{} = {}", schema.properties.at(c->propertyId).name,
                                            toString(c->value)));
            else if (const auto* e = std::get_if<EventInvocationItem>(&item))
                lines.push_back(std::format("{}({} args)", schema.events.at(e->eventId).name, e->arguments.size()));
        }
        return lines;
    }
};

void setState(Humanoid& humanoid, Enum::HumanoidStateType state)
{
    const auto old = humanoid.getState();
    humanoid.assignState(state);
    humanoid.events.stateChanged.fire(old, state);
}

} // namespace

TEST(net_character_sync_sends_the_root_and_the_humanoid)
{
    Session s;
    CharacterSync sync(s.replica);
    setState(*s.humanoid, Enum::HumanoidStateType::Running);
    auto first = sync.frame(*s.character, 1000);
    CHECK_EQ(first.size(), std::size_t{2});

    // The root's motion: one record, the root's assembly as a root-only
    // mechanism with the Humanoid's state, a single PV; unreliable.
    CHECK(first[0].reliability == Reliability::Unreliable);
    auto packet = decodePhysicsPacket(first[0].message, /*serverToClient=*/false);
    CHECK(packet.has_value());
    CHECK_EQ(packet->timestamp, std::uint64_t{1000});
    CHECK(!packet->timestampIdFirst);
    CHECK_EQ(packet->records.size(), std::size_t{1});
    const PhysicsRecord& record = packet->records[0];
    CHECK(record.part == Session::rootId && record.mechanism);
    CHECK_EQ(record.mechanism->humanoidState, std::uint8_t{8});
    CHECK_EQ(record.mechanism->parts.size(), std::size_t{1});
    CHECK(record.mechanism->parts[0].joints.empty());
    const PhysicsFrame& frame = record.mechanism->parts[0].frames.at(0);
    CHECK(frame.rootRef == Session::rootId);
    CHECK((frame.pv.cframe.position - Vector3{588.0f, 0.5f, -232.5f}).magnitude() < 0.01f);
    // Bytes: the record's header byte has the root-only bit and state 8.
    CHECK_EQ(first[0].message[0], std::uint8_t{0x1b});

    // The Humanoid's state, reliable.
    CHECK(first[1].reliability == Reliability::ReliableOrdered);
    CHECK(s.read(first[1]) == std::vector<std::string>{"NetworkHumanoidState = 8"});

    // 15 a second: nothing new 30 ms on, the next packet at 67 ms.
    CHECK(sync.frame(*s.character, 1030).empty());
    auto second = sync.frame(*s.character, 1067);
    CHECK_EQ(second.size(), std::size_t{1});
    auto again = decodePhysicsPacket(second[0].message, false);
    CHECK(again && again->sequence == packet->sequence + 1 && again->timestamp == 1067);

    // A jump within one frame: every state on the way, JumpReplicate, and
    // MoveDirection once it walks.
    s.humanoid->setJump(true);
    setState(*s.humanoid, Enum::HumanoidStateType::Jumping);
    setState(*s.humanoid, Enum::HumanoidStateType::Freefall);
    s.humanoid->updateMovement({0.0f, 0.0f, -1.0f}, Enum::Material::Air);
    auto jump = sync.frame(*s.character, 1090);
    CHECK_EQ(jump.size(), std::size_t{1});
    CHECK(s.read(jump[0]) == (std::vector<std::string>{"NetworkHumanoidState = 3", "NetworkHumanoidState = 5",
                                                       "JumpReplicate = true", "MoveDirectionInternal = 0, 0, -1"}));
    s.humanoid->setJump(false);
    auto released = sync.frame(*s.character, 1100);
    CHECK(s.read(released.at(0)) == std::vector<std::string>{"JumpReplicate = false"});
    CHECK_EQ(sync.propertyChanges, std::size_t{6});

    // Its Animator's tracks.
    const std::vector<std::vector<Value>> tracks{
        {Value(std::string("http://www.roblox.com/asset/?id=507766388")), Value(true), Value(0.1), Value(1.0),
         Value(1.0), Value(0.0), Value(0.0), Value(true), Value(81.0)}};
    auto animated = sync.frame(*s.character, 1110, tracks);
    CHECK(s.read(animated.at(0)) == std::vector<std::string>{"OnCombinedUpdate(9 args)"});
    CHECK_EQ(sync.animatorEvents, std::size_t{1});
}

TEST(net_character_sync_follows_the_connection)
{
    Session s;
    // Capability bit 25 puts the physics id before the sequence.
    s.replica->capabilities |= std::uint64_t{1} << 25;
    CharacterSync sync(s.replica);
    // No state yet (nothing stepped the Humanoid): no motion, no state.
    CHECK(sync.frame(*s.character, 1000).empty());
    setState(*s.humanoid, Enum::HumanoidStateType::Running);
    auto out = sync.frame(*s.character, 1000);
    CHECK_EQ(out.size(), std::size_t{2});
    CHECK_EQ(out[0].message.at(9), std::uint8_t{0x85});
    auto packet = decodePhysicsPacket(out[0].message, false);
    CHECK(packet && packet->timestampIdFirst);
    // The clock going backwards still stamps later than the last packet.
    auto late = sync.frame(*s.character, 900);
    CHECK(late.empty());
    sync.sendPhysicsNow();
    late = sync.frame(*s.character, 900);
    auto stamped = decodePhysicsPacket(late.at(0).message, false);
    CHECK(stamped && stamped->timestamp == 1001);
    // An anchored root is the server's.
    s.root->setAnchored(true);
    sync.sendPhysicsNow();
    CHECK(sync.frame(*s.character, 2000).empty());
    // A character the server never sent: nothing to name it by.
    auto stranger = std::make_shared<Model>();
    auto part = std::make_shared<Part>();
    part->setName("HumanoidRootPart");
    part->setParent(stranger.get());
    auto humanoid = std::make_shared<Humanoid>();
    humanoid->setParent(stranger.get());
    setState(*humanoid, Enum::HumanoidStateType::Running);
    CHECK(sync.frame(*stranger, 3000).empty());
}

// The server moved our root (a CFrame change of it): until we acknowledge it,
// with our count of its changes, the server drops our motion (0x10568712a).
TEST(net_character_sync_acknowledges_the_servers_moves)
{
    Session s;
    CharacterSync sync(s.replica);
    sync.frame(*s.character, 1000);
    auto moved = [&](bool restart) {
        ChangePropertyItem c;
        c.id = Session::rootId;
        c.propertyId = *s.schema.propertyId(*s.schema.classId("Part"), "CFrame");
        c.value = Value(CFrame(Vector3{10.0f, 3.0f, 0.0f}));
        c.flag = restart;
        CHECK(s.replica->handle(encodeData(std::vector<Item>{Item(c)}, s.schema, s.replica->context())));
    };
    auto acks = [&](std::uint64_t ms) {
        std::vector<CFrameAckItem> out;
        for (const auto& message : sync.frame(*s.character, ms)) {
            if (message.reliability != Reliability::ReliableOrdered)
                continue;
            auto items = decodeData(message.message, s.schema, s.replica->context(), nullptr, {},
                                    ItemContext{false, s.replica->capabilities});
            CHECK(items.has_value());
            for (const Item& item : items ? *items : std::vector<Item>{})
                if (const auto* r = std::get_if<RuntimeItem>(&item))
                    if (const auto* a = std::get_if<CFrameAckItem>(&r->body))
                        out.push_back(*a);
        }
        return out;
    };
    moved(true); // the first: the count starts at 0
    auto first = acks(1100);
    CHECK_EQ(first.size(), std::size_t{1});
    CHECK(first.at(0).instance == Session::rootId);
    CHECK(first.at(0).hasVersion);
    CHECK_EQ(first.at(0).version, 0);
    CHECK((s.root->position() - Vector3{10.0f, 3.0f, 0.0f}).magnitude() < 1e-3f);
    moved(false);
    moved(false); // two more in one frame: one ack each, counting up
    auto later = acks(1200);
    CHECK_EQ(later.size(), std::size_t{2});
    CHECK_EQ(later.at(1).version, 2);
    CHECK(acks(1300).empty()); // nothing new, nothing to acknowledge
    CHECK_EQ(sync.cframeAcks, std::size_t{3});
}

// A server with ISR hears our root as the genuine client sends it: ID_ISR
// with our tick and our clock offset from its ticks, and each ID_DATA starts
// with an ISR Timestamp of our tick [observed in a client capture].
TEST(net_character_sync_sends_isr_where_the_server_has_it)
{
    Session s(isrSchema());
    s.replica->capabilities |= caps::CFrameAckStep; // as live servers negotiate
    CharacterSync sync(s.replica);
    setState(*s.humanoid, Enum::HumanoidStateType::Running);
    auto first = sync.frame(*s.character, 1000);
    CHECK_EQ(first.size(), std::size_t{2});
    CHECK(first[0].reliability == Reliability::Unreliable);
    auto isr = decodeIsrPacket(first[0].message);
    CHECK(isr.has_value());
    CHECK_EQ(isr->tick, 0u);
    CHECK_EQ(isr->clockOffset, kIsrNoClockOffset); // no server tick heard yet
    CHECK_EQ(isr->groups.size(), 1u);
    CHECK_EQ(isr->groups.at(0).kind, 0);
    CHECK_EQ(isr->groups.at(0).age, 0);
    const IsrInstanceState& state = isr->groups.at(0).instances.at(0);
    CHECK(state.instance == Session::rootId);
    CHECK_EQ(state.classId, *s.schema.classId("Part"));
    CHECK_EQ(state.propertyCount, 3);
    CHECK_EQ(state.dirty, 4u);
    bool complete = false;
    const auto properties = isrProperties(state, s.schema, &complete);
    CHECK(complete && properties.size() == 1);
    NetworkStream pvBytes(properties.at(0).bytes);
    CHECK((readReplicationPV(pvBytes).cframe.position - Vector3{588.0f, 0.5f, -232.5f}).magnitude() < 0.01f);
    CHECK_EQ(sync.isrPackets, std::size_t{1});
    CHECK_EQ(sync.physicsPackets, std::size_t{0});

    auto stamps = [&](const CharacterSync::Outgoing& out) {
        std::vector<std::int32_t> ticks;
        auto items = decodeData(out.message, s.schema, s.replica->context(), nullptr, {},
                                ItemContext{false, s.replica->capabilities});
        CHECK(items.has_value());
        for (const Item& item : items ? *items : std::vector<Item>{})
            if (const auto* r = std::get_if<RuntimeItem>(&item))
                if (const auto* t = std::get_if<IsrTimestampItem>(&r->body))
                    ticks.push_back(t->timestamp);
        return ticks;
    };
    CHECK(first[1].reliability == Reliability::ReliableOrdered);
    CHECK(stamps(first[1]) == std::vector<std::int32_t>{0});
    CHECK(s.read(first[1]) == std::vector<std::string>{"NetworkHumanoidState = 8"});

    // The server's tick, as its ISR Timestamp items carry it: our offset from it.
    auto serverTick = [&](std::int32_t tick) {
        CHECK(s.replica->handle(
            encodeData(std::vector<Item>{Item(RuntimeItem{IsrTimestampItem{tick}})}, s.schema, s.replica->context())));
    };
    serverTick(5000000);
    auto second = sync.frame(*s.character, 1067);
    auto packet = decodeIsrPacket(second.at(0).message);
    CHECK(packet && packet->tick == 16 && packet->clockOffset == 16 - 5000000);
    // Its tick runs on with ours: the offset holds.
    packet = decodeIsrPacket(sync.frame(*s.character, 1134).at(0).message);
    CHECK(packet && packet->tick == 32 && packet->clockOffset == 16 - 5000000);
    // A tick that says we were 68 ticks off: the offset moves toward it a little at a time.
    serverTick(5000100);
    packet = decodeIsrPacket(sync.frame(*s.character, 1201).at(0).message);
    CHECK(packet && packet->tick == 48 && packet->clockOffset == 16 - 5000000 - 8);
    CHECK(sync.clockOffset() == std::optional<std::int32_t>(16 - 5000000 - 8));

    // An acknowledgement's step is our tick.
    ChangePropertyItem moved;
    moved.id = Session::rootId;
    moved.propertyId = *s.schema.propertyId(*s.schema.classId("Part"), "CFrame");
    moved.value = Value(CFrame(Vector3{10.0f, 3.0f, 0.0f}));
    moved.flag = true;
    CHECK(s.replica->handle(encodeData(std::vector<Item>{Item(moved)}, s.schema, s.replica->context())));
    std::optional<CFrameAckItem> ack;
    for (const auto& out : sync.frame(*s.character, 1250))
        if (out.reliability == Reliability::ReliableOrdered) {
            CHECK(stamps(out) == std::vector<std::int32_t>{60});
            auto items = decodeData(out.message, s.schema, s.replica->context(), nullptr, {},
                                    ItemContext{false, s.replica->capabilities});
            for (const Item& item : items ? *items : std::vector<Item>{})
                if (const auto* r = std::get_if<RuntimeItem>(&item))
                    if (const auto* a = std::get_if<CFrameAckItem>(&r->body))
                        ack = *a;
        }
    CHECK(ack && ack->step == 60u && ack->hasVersion && ack->version == 0);

    // The older motion packet when asked for (LUNAR_NET_PHYSICS=85).
    CharacterSync older(s.replica);
    older.motionForm = CharacterSync::MotionForm::Physics;
    auto out = older.frame(*s.character, 2000);
    CHECK(!out.empty() && decodePhysicsPacket(out.at(0).message, false).has_value());
    CHECK_EQ(older.isrPackets, std::size_t{0});
}

// A part the server gives us besides our character (its NetworkOwnerV3 our
// peer id): its motion goes out with the root's, in the same ISR group or
// the same 0x85 packet, and the server moving it is acknowledged as for the
// root. The sUNC test game hands each client a part to move that way.
TEST(net_character_sync_sends_the_parts_the_server_gives_us)
{
    Schema schema = isrSchema();
    addProperty(schema, "Part", "NetworkOwnerV3", nettype::PeerAddress);
    Session s(schema);
    const std::uint16_t partClass = *s.schema.classId("Part");
    constexpr InstanceId givenId{1, 20};
    NewInstanceItem n;
    n.id = givenId;
    n.classId = partClass;
    n.properties.emplace_back(*s.schema.propertyId(partClass, "Name"), Value(std::string("itsstarting")));
    n.properties.emplace_back(*s.schema.propertyId(partClass, "NetworkOwnerV3"), Value(914.0));
    n.parent = {1, 1};
    CHECK(s.replica->handle(encodeData(std::vector<Item>{Item(n)}, s.schema, s.replica->context())));
    auto* given = dynamic_cast<BasePart*>(s.replica->find(givenId).get());
    CHECK(given && s.replica->ownedHere(givenId));
    if (!given)
        return;
    given->setPosition({20.0f, 420.0f, 67.0f});
    const std::vector<const BasePart*> owned{given};

    CharacterSync sync(s.replica);
    setState(*s.humanoid, Enum::HumanoidStateType::Running);
    auto out = sync.frame(*s.character, 1000, {}, owned);
    auto isr = decodeIsrPacket(out.at(0).message);
    CHECK(isr && isr->groups.size() == 1);
    const auto& instances = isr->groups.at(0).instances;
    CHECK_EQ(instances.size(), std::size_t{2});
    CHECK(instances.at(0).instance == Session::rootId);
    CHECK(instances.at(1).instance == givenId);
    const auto properties = isrProperties(instances.at(1), s.schema);
    CHECK_EQ(properties.size(), std::size_t{1});
    NetworkStream pvBytes(properties.at(0).bytes);
    CHECK((readReplicationPV(pvBytes).cframe.position - Vector3{20.0f, 420.0f, 67.0f}).magnitude() < 0.01f);
    // While the server moves our root, the part still goes out; anchored, it's the server's.
    sync.ownsRoot = false;
    isr = decodeIsrPacket(sync.frame(*s.character, 1067, {}, owned).at(0).message);
    CHECK(isr && isr->groups.at(0).instances.size() == 1 && isr->groups.at(0).instances.at(0).instance == givenId);
    given->setAnchored(true);
    CHECK(sync.frame(*s.character, 1134, {}, owned).empty());
    given->setAnchored(false);

    // The server moving it: acknowledged.
    ChangePropertyItem moved;
    moved.id = givenId;
    moved.propertyId = *s.schema.propertyId(partClass, "CFrame");
    moved.value = Value(CFrame(Vector3{67.0f, 350.0f, 67.0f}));
    moved.flag = true;
    CHECK(s.replica->handle(encodeData(std::vector<Item>{Item(moved)}, s.schema, s.replica->context())));
    std::optional<CFrameAckItem> ack;
    for (const auto& message : sync.frame(*s.character, 1201, {}, owned))
        if (message.reliability == Reliability::ReliableOrdered) {
            auto items = decodeData(message.message, s.schema, s.replica->context(), nullptr, {},
                                    ItemContext{false, s.replica->capabilities});
            for (const Item& item : items ? *items : std::vector<Item>{})
                if (const auto* r = std::get_if<RuntimeItem>(&item))
                    if (const auto* a = std::get_if<CFrameAckItem>(&r->body))
                        ack = *a;
        }
    CHECK(ack && ack->instance == givenId && ack->version == 0);

    // The older motion packet: a record for each, the root's with the Humanoid's state.
    CharacterSync older(s.replica);
    older.motionForm = CharacterSync::MotionForm::Physics;
    auto physics = decodePhysicsPacket(older.frame(*s.character, 2000, {}, owned).at(0).message, false);
    CHECK(physics && physics->records.size() == 2);
    CHECK(physics->records.at(0).part == Session::rootId && physics->records.at(0).mechanism->humanoidState == 8);
    CHECK(physics->records.at(1).part == givenId);
}

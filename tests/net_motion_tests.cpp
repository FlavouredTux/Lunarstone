#include "framework.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/net/Motion.hpp>
#include <lunar/net/NetworkStream.hpp>
#include <lunar/net/Physics.hpp>
#include <lunar/net/Replication.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string_view>
#include <vector>

using namespace lunar;
using namespace lunar::net;

namespace {

std::vector<std::uint8_t> fromHex(std::string_view hex)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    return out;
}

bool near(Vector3 a, Vector3 b, float tolerance = 1e-3f)
{
    return (a - b).magnitude() <= tolerance;
}

bool near(const CFrame& a, const CFrame& b, float tolerance = 1e-3f)
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::abs(a.r[i][j] - b.r[i][j]) > tolerance)
                return false;
    return near(a.position, b.position, tolerance);
}

// ISR packets a live server sent (place 18667984660, 0.742-era server): one
// player's HumanoidRootPart (1:14499927) walking, at ticks 8609269, 8609285
// and 8609301, ages 47, 46 and 30.
constexpr std::string_view kWalk[] = {
    "a1f55d830001000000ffffff7f0100002f015740dd00630103000400000000000000180000535abaa4ff92f30000da3a0040562300108520004e0400",
    "a1055e830001000000ffffff7f0100002e015740dd00630103000400000000000000180000531bbaa4ffabf300006f3700405e3500f08b20004e0600",
    "a1155e830001000000ffffff7f0100001e015740dd006301030004000000000000001800005399b9a4ffecf30000023200405b3900d08d1000b50400",
};
// Two Animators' FacsReplicationData in one packet, a group each.
constexpr std::string_view kFacs
    = "a1d845630002000000ffffff7f010001ff01a4e054001c0001000100000000000000110005000000000000090000000000003fff01010001"
      "0001950092001c00010001000000000000001200050000000000000a0002101080000003ff01";

PhysicsPV pvOf(const IsrInstanceState& state)
{
    NetworkStream in(state.data);
    return readReplicationPV(in);
}

// A character-shaped model: a root with a torso on a Motor6D, a hat welded
// to the torso, and a part nearby that isn't jointed to anything.
struct Rig {
    std::shared_ptr<DataModel> game = DataModel::create();
    InstancePtr root, torso, hat, loose;

    Rig()
    {
        auto* workspace = game->getService("Workspace");
        auto model = ClassRegistry::create("Model");
        model->setName("Guy");
        model->setParent(workspace);
        auto part = [&](const char* name, CFrame at, Instance* parent) {
            auto p = ClassRegistry::create("Part");
            p->setName(name);
            p->set("CFrame", Value(at));
            p->setParent(parent);
            return p;
        };
        root = part("HumanoidRootPart", CFrame(0, 3, 0), model.get());
        torso = part("Torso", CFrame(0, 3.5f, 0) * CFrame::angles(0, 0.3f, 0), model.get());
        hat = part("Hat", CFrame(0.2f, 5, 0.1f), model.get());
        loose = part("Loose", CFrame(10, 1, 0), workspace);
        auto motor = ClassRegistry::create("Motor6D");
        motor->set("Part0", Value(root));
        motor->set("Part1", Value(torso));
        motor->set("C0", Value(CFrame(0, 0.5f, 0)));
        motor->set("C1", Value(CFrame::angles(0, -0.3f, 0)));
        motor->setParent(root.get());
        auto weld = ClassRegistry::create("WeldConstraint");
        weld->set("Part0", Value(torso));
        weld->set("Part1", Value(hat));
        weld->setParent(hat.get());
    }
};

CFrame cframeOf(const InstancePtr& part)
{
    return std::get<CFrame>(part->get("CFrame"));
}

// The server's schema with Part's ISR properties as a live server has them
// (PhysicsRepRootPart, PhysicsRepRootRef, ReplicationPV) appended to Part's
// properties: New Instance items name properties relative to their class, so
// the server's join still reads.
Schema withPartIsr(const Schema& base, std::uint16_t& pvProperty, std::uint16_t* ownerProperty = nullptr)
{
    Schema s = base;
    s.properties.clear();
    const auto part = *base.classId("Part");
    for (std::size_t c = 0; c < s.classes.size(); ++c) {
        SchemaClass& cls = s.classes[c];
        const SchemaClass& from = base.classes[c];
        cls.firstProperty = static_cast<std::uint16_t>(s.properties.size());
        for (std::uint16_t i = 0; i < from.propertyCount; ++i)
            s.properties.push_back(base.properties[from.firstProperty + i]);
        if (c == part) {
            const std::uint32_t first = static_cast<std::uint32_t>(s.properties.size());
            s.properties.push_back({"PhysicsRepRootPart", nettype::Object, 0xffff, 0, part});
            s.properties.push_back({"PhysicsRepRootRef", 0x45, 0xffff, 0, part});
            s.properties.push_back({"ReplicationPV", nettype::ReplicationPV, 0xffff, 0, part});
            cls.propertyCount = static_cast<std::uint16_t>(cls.propertyCount + 3);
            s.isrClasses.push_back({part, {first, first + 1, first + 2}});
            pvProperty = static_cast<std::uint16_t>(first + 2);
            if (ownerProperty) {
                *ownerProperty = static_cast<std::uint16_t>(s.properties.size());
                s.properties.push_back({"NetworkOwnerV3", nettype::PeerAddress, 0xffff, 0, part});
                cls.propertyCount = static_cast<std::uint16_t>(cls.propertyCount + 1);
            }
        }
    }
    return s;
}

// A parts' assembly kept its shape: each part where it was relative to the root.
void checkFollowed(const CFrame& rootBefore, const CFrame& rootAfter, const CFrame& before, const CFrame& after)
{
    CHECK(near(rootBefore.inverse() * before, rootAfter.inverse() * after, 2e-3f));
}

} // namespace

// What a live server sends: every byte of the packet read, the part's motion
// a ReplicationPV, and the same bytes written back.
TEST(net_isr_decodes_a_live_servers_packets)
{
    const auto bytes = fromHex(kWalk[1]);
    auto packet = decodeIsrPacket(bytes);
    CHECK(packet.has_value());
    CHECK_EQ(packet->tick, 8609285u);
    CHECK_EQ(packet->clockOffset, kIsrNoClockOffset); // a server has none
    CHECK_EQ(packet->groups.size(), 1u);
    const IsrGroup& group = packet->groups.at(0);
    CHECK_EQ(group.kind, 0);
    CHECK_EQ(group.age, 46);
    CHECK_EQ(group.instances.size(), 1u);
    const IsrInstanceState& state = group.instances.at(0);
    CHECK(state.instance == (InstanceId{1, 14499927}));
    CHECK_EQ(state.classId, 355); // Part in that server's schema
    CHECK_EQ(state.propertyCount, 3);
    CHECK_EQ(state.dirty, 4u); // the third: ReplicationPV
    NetworkStream in(state.data);
    const PhysicsPV pv = readReplicationPV(in);
    CHECK_EQ(in.offset(), state.data.size());
    CHECK(near(pv.cframe.position, {744.4332f, -1.4375219f, -49.328876f}));
    CHECK(near(pv.linearVelocity, {-14.5149f, 0.0f, 6.7376647f}));
    CHECK(near(pv.angularVelocity, {0.0f, 1.5769418f, 0.0f}));
    CHECK(encodeIsrPacket(*packet) == bytes);

    const auto facsBytes = fromHex(kFacs);
    auto facs = decodeIsrPacket(facsBytes);
    CHECK(facs.has_value());
    CHECK_EQ(facs->groups.size(), 2u);
    CHECK_EQ(facs->groups.at(0).kind, 1);
    CHECK_EQ(facs->groups.at(0).age, 255);
    CHECK_EQ(facs->groups.at(1).age, 0);
    CHECK_EQ(facs->groups.at(1).instances.at(0).classId, 28); // Animator
    CHECK_EQ(facs->groups.at(1).instances.at(0).data.size(), 18u);
    CHECK(encodeIsrPacket(*facs) == facsBytes);

    CHECK(!decodeIsrPacket(std::span(bytes).first(bytes.size() - 1)).has_value());
}

// The group's age is how long before the packet's tick the state was taken:
// with it, a walking character's samples lie as far apart as its velocity
// carries it in between; with the bare ticks they don't (the server sent the
// third packet a send interval after the second, with two intervals' worth
// of walking in it).
TEST(net_isr_age_dates_the_state)
{
    std::vector<IsrPacket> packets;
    for (std::string_view hex : kWalk)
        packets.push_back(*decodeIsrPacket(fromHex(hex)));
    for (std::size_t i = 1; i < packets.size(); ++i) {
        const IsrPacket& a = packets[i - 1];
        const IsrPacket& b = packets[i];
        const PhysicsPV pa = pvOf(a.groups[0].instances[0]);
        const PhysicsPV pb = pvOf(b.groups[0].instances[0]);
        const double distance = (pb.cframe.position - pa.cframe.position).magnitude();
        const double speed = (pa.linearVelocity.magnitude() + pb.linearVelocity.magnitude()) / 2;
        const double aged = ((static_cast<double>(b.tick) - b.groups[0].age)
                             - (static_cast<double>(a.tick) - a.groups[0].age))
                            / kIsrTicksPerSecond;
        CHECK(std::abs(distance / aged - speed) < 1.5);
        if (i == 2) {
            const double bare = (b.tick - a.tick) / kIsrTicksPerSecond;
            CHECK(std::abs(distance / bare - speed) > 10.0);
        }
    }
}

// What the genuine client sends of its own parts (in a client capture,
// vriltestaccount): its HumanoidRootPart at its tick 123 and a part it
// owned at tick 8279. The header's i32 is its clock offset: its tick minus
// the server's (5622522 then 5630922; the server's own ISR ticks were
// 5622752 and 5630916 when these left). Their ReplicationPV data is in a
// newer form than writeReplicationPV's, which servers use with clients
// that negotiate more capabilities than Lunarstone (not read here).
TEST(net_isr_reads_the_genuine_clients_packets)
{
    struct Sample {
        std::string_view hex;
        std::uint32_t tick;
        std::int32_t offset;
        InstanceId part;
        std::size_t dataBytes;
    };
    const Sample samples[]
        = {
            {"a17b000000010000008135aaff0100000001cd6b47006301030004000000000000001300504942b71acf62ff5e0000910100d9c3f"
             "f" "af8a",
             123,
             -5622399,
             {1, 4680653},
             19},
            {"a157200000010000008d34aaff010000000150734700630103000400000000000000180010d9f5b716cf828161d001dc6c0370e8c"
             "e" "2ff9bde8fa6b9f",
             8279,
             -5622643,
             {1, 4682576},
             24},
        };
    for (const Sample& sample : samples) {
        const auto bytes = fromHex(sample.hex);
        auto packet = decodeIsrPacket(bytes);
        CHECK(packet.has_value());
        CHECK_EQ(packet->tick, sample.tick);
        CHECK_EQ(packet->clockOffset, sample.offset);
        CHECK_EQ(packet->groups.size(), 1u);
        const IsrGroup& group = packet->groups.at(0);
        CHECK_EQ(group.kind, 0);
        CHECK_EQ(group.age, 0);
        CHECK_EQ(group.instances.size(), 1u);
        const IsrInstanceState& state = group.instances.at(0);
        CHECK(state.instance == sample.part);
        CHECK_EQ(state.classId, 355); // Part in that server's schema
        CHECK_EQ(state.propertyCount, 3);
        CHECK_EQ(state.dirty, 4u); // ReplicationPV alone
        CHECK_EQ(state.data.size(), sample.dataBytes);
        CHECK(encodeIsrPacket(*packet) == bytes);
    }
}

// Ours: the same frame around a ReplicationPV that writeReplicationPV wrote.
TEST(net_isr_writes_our_own_part)
{
    PhysicsPV pv;
    pv.cframe = CFrame(366.5f, 414.2f, -1.25f);
    pv.linearVelocity = {0, 0, -16};
    const IsrPacket packet = ownPartIsrPacket(8279, -5622643, {1, 4680653}, 355, 3, 2, pv);
    const auto bytes = encodeIsrPacket(packet);
    const auto head = fromHex("a157200000010000008d34aaff0100000001cd6b4700630103000400000000000000");
    CHECK(bytes.size() > head.size() && std::equal(head.begin(), head.end(), bytes.begin()));
    auto back = decodeIsrPacket(bytes);
    CHECK(back.has_value() && *back == packet);
    NetworkStream in(back->groups.at(0).instances.at(0).data);
    const PhysicsPV read = readReplicationPV(in);
    CHECK_EQ(in.left(), 0u);
    CHECK(near(read.cframe.position, pv.cframe.position, 1e-2f));
    CHECK(near(read.linearVelocity, pv.linearVelocity, 1e-2f));
}

// An instance state's data split by the schema's ISR properties, in dirty-bit order.
TEST(net_isr_properties_follow_the_schema)
{
    Schema schema;
    schema.classes.push_back({"Part", 0, 3, 0, 0, 0xffff});
    schema.properties = {{"Anchor", nettype::Object, 0xffff, 0, 0},
                         {"Size", nettype::Vector3, 0xffff, 0, 0},
                         {"ReplicationPV", nettype::ReplicationPV, 0xffff, 0, 0}};
    schema.isrClasses.push_back({0, {0, 1, 2}});
    PhysicsPV pv;
    pv.cframe = CFrame(1, 2, 3);
    pv.linearVelocity = {4, 0, 0};
    NetworkStream data;
    CHECK(writeValue(data, nettype::Vector3, Value(Vector3{1, 2, 3})));
    const std::size_t sizeBytes = data.bytes().size();
    writeReplicationPV(data, pv);
    IsrInstanceState state{{1, 7}, 0, 3, 0b110, {data.bytes().begin(), data.bytes().end()}};
    bool complete = false;
    const auto properties = isrProperties(state, schema, &complete);
    CHECK(complete);
    CHECK_EQ(properties.size(), 2u);
    CHECK_EQ(properties.at(0).bit, 1u);
    CHECK(properties.at(0).propertyId == std::optional<std::uint32_t>(1));
    CHECK_EQ(properties.at(0).bytes.size(), sizeBytes);
    CHECK(properties.at(1).propertyId == std::optional<std::uint32_t>(2));
    NetworkStream in(properties.at(1).bytes);
    CHECK(near(readReplicationPV(in).cframe.position, {1, 2, 3}, 1e-2f)); // quantized

    // A class the schema has no ISR properties for: nothing, not complete.
    state.classId = 5;
    CHECK(isrProperties(state, schema, &complete).empty());
    CHECK(!complete);
}

// ID_PHYSICS (0x85) from the server moves the record's part, and the parts
// jointed to it keep their place; a part not jointed to it stays put.
TEST(net_motion_replica_moves_an_assembly_from_physics_packets)
{
    Rig rig;
    ReplicationServer server(rig.game, {"Workspace"});
    Replica client;
    for (const auto& m : server.joinMessages())
        CHECK(client.handle(m));
    auto root = client.find(server.idOf(rig.root.get()));
    auto torso = client.find(server.idOf(rig.torso.get()));
    auto hat = client.find(server.idOf(rig.hat.get()));
    auto loose = client.find(server.idOf(rig.loose.get()));
    CHECK(root && torso && hat && loose);
    auto* rootPart = dynamic_cast<BasePart*>(root.get());
    CHECK(rootPart != nullptr);
    const auto assembly = ReplicatedMotion::assemblyOf(*rootPart);
    CHECK_EQ(assembly.size(), 2u); // torso and hat, not the loose part

    const CFrame rootBefore = cframeOf(root), torsoBefore = cframeOf(torso), hatBefore = cframeOf(hat),
                 looseBefore = cframeOf(loose);
    PhysicsPV pv;
    pv.cframe = CFrame(20, 3, -5) * CFrame::angles(0, 0.5f, 0);
    pv.linearVelocity = {16, 0, 0};
    pv.angularVelocity = {0, 1, 0};
    const auto wire = encodePhysicsPacket(onePartPacket(5000, 1, true, server.idOf(rig.root.get()), pv));
    const PhysicsPV sent = decodePhysicsPacket(wire, true)->records.at(0).mechanism->parts.at(0).frames.at(0).pv;
    client.decodingServerToClient = true;
    client.receiveTime = 100.0;
    CHECK(client.handle(wire));
    CHECK_EQ(client.motion().roots(), 1u);
    // Nothing steps the playback yet, so the sample applies as it comes.
    CHECK(near(cframeOf(root), sent.cframe));
    CHECK(near(rootPart->assemblyLinearVelocity(), sent.linearVelocity));
    checkFollowed(rootBefore, cframeOf(root), torsoBefore, cframeOf(torso));
    checkFollowed(rootBefore, cframeOf(root), hatBefore, cframeOf(hat));
    CHECK(cframeOf(loose) == looseBefore);

    // Once something steps it, the playback places the root: a single sample
    // runs on by its velocity, for at most maxExtrapolation.
    client.stepMotion(100.0 + 10.0);
    const double ran = client.motion().maxExtrapolation;
    CHECK(near(cframeOf(root).position, sent.cframe.position + sent.linearVelocity * static_cast<float>(ran), 1e-2f));
    checkFollowed(rootBefore, cframeOf(root), torsoBefore, cframeOf(torso));
    checkFollowed(rootBefore, cframeOf(root), hatBefore, cframeOf(hat));
}

// The same through ISR, with a schema that has Part's ISR properties: the
// ReplicationPV moves the part and its assembly; a packet for an instance
// the replica doesn't have is counted and dropped.
TEST(net_motion_replica_moves_an_assembly_from_isr)
{
    Rig rig;
    ReplicationServer server(rig.game, {"Workspace"});
    std::uint16_t pvProperty = 0;
    const Schema schema = withPartIsr(server.schema(), pvProperty);
    Replica client;
    for (const auto& m : server.joinMessages(16 * 1024, ReplicationServer::JoinStyle::NewItems))
        CHECK(client.handle(!m.empty() && m[0] == kSchemaPacketId ? encodeSchemaPacket(schema) : m));
    auto root = client.find(server.idOf(rig.root.get()));
    auto torso = client.find(server.idOf(rig.torso.get()));
    CHECK(root && torso);
    const CFrame rootBefore = cframeOf(root), torsoBefore = cframeOf(torso);

    PhysicsPV pv;
    pv.cframe = CFrame(-7, 4, 12) * CFrame::angles(0, -1.0f, 0);
    pv.linearVelocity = {0, 0, 16};
    NetworkStream data;
    writeReplicationPV(data, pv);
    NetworkStream back(data.bytes());
    const PhysicsPV sent = readReplicationPV(back);
    IsrPacket packet;
    packet.tick = 24000;
    const auto partClass = *schema.classId("Part");
    packet.groups.push_back(
        {0, 16, {{server.idOf(rig.root.get()), partClass, 3, 4, {data.bytes().begin(), data.bytes().end()}}}});
    packet.groups.push_back({0, 16, {{{1, 999999}, partClass, 3, 4, {data.bytes().begin(), data.bytes().end()}}}});
    Node tree;
    client.receiveTime = 50.0;
    CHECK(client.handle(encodeIsrPacket(packet), &tree));
    CHECK(near(cframeOf(root), sent.cframe));
    checkFollowed(rootBefore, cframeOf(root), torsoBefore, cframeOf(torso));
    CHECK_EQ(client.motionForUnknown, 1u);
    CHECK(render(tree).find("ReplicationPV: pos (-7") != std::string::npos);
}

// The playback: behind the latest lateness by the averaged sample spacing
// (0.9 old + 0.1 new, as the client keeps it), Hermite between samples (a
// straight line when the velocities agree with it), slerp for the rotation,
// a capped run-on past the newest sample, the first sample before it. A
// sample older than the newest is dropped, as the client drops it.
TEST(net_motion_playback_interpolates_and_runs_on)
{
    ReplicatedMotion motion;
    const InstanceId id{1, 1};
    for (int i = 0; i <= 2; ++i) {
        PhysicsPV pv;
        pv.cframe = CFrame(static_cast<float>(i), 0, 0)
                    * CFrame::angles(0, static_cast<float>(i) * std::numbers::pi_v<float> / 4, 0);
        pv.linearVelocity = {10, 0, 0};
        motion.receive(id, i * 0.1, i * 0.1 + 0.05, pv); // 50 ms late, each
    }
    PhysicsPV stale;
    stale.cframe = CFrame(100, 0, 0);
    motion.receive(id, 0.05, 0.3, stale);
    // spacing: 0.1 * 0.1, then 0.9 * 0.01 + 0.1 * 0.1
    CHECK(std::abs(*motion.shownAt(id, 1.0) - (1.0 - 0.019 - 0.05)) < 1e-9);
    // The local time a server time is shown at.
    auto at = [&](double shown) { return *motion.at(id, shown - *motion.shownAt(id, 0.0)); };
    CHECK(near(at(0.05).cframe.position, {0.5f, 0, 0}));
    CHECK(near(at(0.1).cframe, CFrame(1, 0, 0) * CFrame::angles(0, std::numbers::pi_v<float> / 4, 0)));
    CHECK(near(at(0.15).cframe, CFrame(1.5f, 0, 0) * CFrame::angles(0, 3 * std::numbers::pi_v<float> / 8, 0)));
    CHECK(near(at(0.3).cframe.position, {3, 0, 0}));
    CHECK(near(at(5.0).cframe.position, {2 + 10 * static_cast<float>(motion.maxExtrapolation), 0, 0}));
    CHECK(near(at(-1.0).cframe.position, {0, 0, 0}));
    CHECK(!motion.at({1, 2}, 0.0).has_value());
}

// The server sends nothing while a part rests: across such a pause the part
// holds, then glides to the next sample instead of creeping all the way.
TEST(net_motion_playback_holds_through_a_pause)
{
    ReplicatedMotion motion;
    const InstanceId id{1, 1};
    PhysicsPV rest;
    PhysicsPV walking;
    walking.cframe = CFrame(1, 0, 0);
    walking.linearVelocity = {16, 0, 0};
    motion.receive(id, 0.0, 0.05, rest);
    motion.receive(id, 2.0, 2.05, walking);
    auto x = [&](double shown) { return motion.at(id, shown - *motion.shownAt(id, 0.0))->cframe.position.x; };
    CHECK_EQ(x(1.0), 0.0f);
    CHECK_EQ(x(1.89), 0.0f);
    float last = 0.0f;
    for (double shown = 1.9; shown <= 2.0; shown += 0.01) {
        const float now = x(shown);
        CHECK(now >= last - 1e-4f && now <= 1.0001f);
        last = now;
    }
    CHECK(near(Vector3{x(2.0), 0, 0}, {1, 0, 0}));
}

// A sample that comes in later than the playback expected moves the path
// under what was shown: stepping eases into it instead of jumping, and
// settles where the samples say.
TEST(net_motion_step_eases_into_a_late_sample)
{
    auto part = ClassRegistry::create("Part");
    const InstanceId id{1, 1};
    auto resolve = [&](InstanceId want) { return want == id ? part : nullptr; };
    ReplicatedMotion motion;
    auto sample = [&](double t, float x, float v, double lateness) {
        PhysicsPV pv;
        pv.cframe = CFrame(x, 0, 0);
        pv.linearVelocity = {v, 0, 0};
        motion.receive(id, t, t + lateness, pv);
    };
    for (int i = 0; i <= 5; ++i)
        sample(i * 0.1, static_cast<float>(i), 10, 0.05);
    // Played up to the newest sample and beyond, running on at 10 studs/s ...
    double now = 0.0;
    float last = 0.0f, worst = 0.0f;
    auto stepTo = [&](double until) {
        for (; now < until; now += 1.0 / 60) {
            motion.step(now, resolve);
            const float x = std::get<CFrame>(part->get("CFrame")).position.x;
            if (now > 0.2)
                worst = std::max(worst, std::abs(x - last));
            last = x;
        }
    };
    stepTo(0.75);
    // ... the part had stopped short of where it ran on to, and says so late.
    sample(0.6, 5.3f, 0, 0.2);
    stepTo(2.0);
    CHECK(worst < 0.3f); // 10 studs/s is 0.17 a frame; no jump on top of it
    CHECK(near(std::get<CFrame>(part->get("CFrame")).position, {5.3f, 0, 0}, 1e-3f));
}

// A character that starts walking after a rest often comes much later than
// before it (its owner's samples age more on the way [capture]): the
// playback takes the new lateness at once, while the part still stands,
// so it walks off smoothly instead of running ahead and being pulled back.
TEST(net_motion_step_walks_off_smoothly_after_a_rest)
{
    auto part = ClassRegistry::create("Part");
    const InstanceId id{1, 1};
    auto resolve = [&](InstanceId want) { return want == id ? part : nullptr; };
    ReplicatedMotion motion;
    PhysicsPV rest;
    motion.receive(id, 0.0, 0.05, rest);
    struct Pending {
        double time, arrival;
        float x;
    };
    std::vector<Pending> walk;
    for (int i = 0; i <= 30; ++i) {
        const double t = 5.0 + i / 15.0;
        walk.push_back({t, t + 0.4, 1.0f + 16.0f * static_cast<float>(i) / 15.0f});
    }
    std::size_t next = 0;
    float last = 0.0f, worst = 0.0f;
    for (double now = 0.0; now < 8.0; now += 1.0 / 60) {
        for (; next < walk.size() && walk[next].arrival <= now; ++next) {
            PhysicsPV pv;
            pv.cframe = CFrame(walk[next].x, 0, 0);
            pv.linearVelocity = {16, 0, 0};
            motion.receive(id, walk[next].time, walk[next].arrival, pv);
        }
        motion.step(now, resolve);
        const float x = std::get<CFrame>(part->get("CFrame")).position.x;
        worst = std::max(worst, std::abs(x - last));
        last = x;
    }
    CHECK(worst < 0.45f); // 16 studs/s is 0.27 a frame
    CHECK(last > 30.0f);  // and it got there
}

// How many samples the server has sent for each root: the live client takes
// its character back once the server's samples for its root stop coming
// (the game hands the character back after a ragdoll).
TEST(net_motion_counts_each_roots_samples)
{
    ReplicatedMotion motion;
    const InstanceId ours{1, 1}, theirs{1, 2};
    CHECK_EQ(motion.receivedFor(ours), std::size_t{0});
    PhysicsPV pv;
    motion.receive(ours, 0.0, 0.05, pv);
    motion.receive(ours, 0.1, 0.15, pv);
    motion.receive(theirs, 0.1, 0.15, pv);
    CHECK_EQ(motion.receivedFor(ours), std::size_t{2});
    CHECK_EQ(motion.receivedFor(theirs), std::size_t{1});
    CHECK_EQ(motion.received(), std::size_t{3});
}

// The game's ragdoll: the Motor6D lets go (Part0 nil), so the torso comes as a
// root of its own; once the joint is back only the root's samples come, and
// the torso's stale track gives way instead of carrying the whole rig back
// and forth (which grew it thousands of studs away within a frame [capture]).
TEST(net_motion_a_part_that_rejoins_follows_its_new_root)
{
    Rig rig;
    const InstanceId rootId{1, 1}, torsoId{1, 2};
    const auto resolve = [&](InstanceId id) {
        return id == rootId ? rig.root : id == torsoId ? rig.torso : InstancePtr{};
    };
    Instance* motor = rig.root->findFirstChildOfClass("Motor6D");
    motor->set("Part0", Value());
    ReplicatedMotion motion;
    const auto sample = [&](InstanceId id, double t, CFrame at) {
        PhysicsPV pv;
        pv.cframe = at;
        pv.angularVelocity = {0, 20, 5};
        motion.receive(id, t, t + 0.05, pv);
    };
    double t = 0;
    for (; t < 1.0; t += 0.05) {
        sample(rootId, t, CFrame(static_cast<float>(t) * 10, 3, 0) * CFrame::angles(0.1f, 0.2f, 0.3f));
        sample(torsoId, t, CFrame(static_cast<float>(t) * 10, 2, 1) * CFrame::angles(1.0f, 0.5f, 0.2f));
        motion.step(t + 0.05, resolve);
    }
    CHECK(near(cframeOf(rig.torso).position, motion.at(torsoId, t)->cframe.position, 0.5f));
    CHECK_EQ(motion.roots(), std::size_t{2});

    motor->set("Part0", Value(rig.root));
    for (const double end = t + 1.0; t < end; t += 0.05) {
        sample(rootId, t, CFrame(static_cast<float>(t) * 10, 3, 0) * CFrame::angles(0.1f, 0.2f, 0.3f));
        motion.step(t + 0.05, resolve);
    }
    CHECK_EQ(motion.roots(), std::size_t{1});
    CHECK(!motion.at(torsoId, t).has_value());
    // Held where it was against the root, and still a rotation.
    const CFrame torso = cframeOf(rig.torso), root = cframeOf(rig.root);
    CHECK((torso.position - root.position).magnitude() < 5.0f);
    CHECK(std::abs(torso.rightVector().cross(torso.upVector()).dot(torso.lookVector()) + 1.0f) < 1e-4f);
}

// The server gives a client a part to simulate by setting its NetworkOwnerV3
// to the client's peer id (the game hands a room's side door to the player who
// opens it [capture]). The replica keeps who owns what, and the playback
// leaves what the client simulates to it, whatever the server still sends,
// until the server takes it back.
TEST(net_motion_leaves_the_parts_the_server_gives_us_to_us)
{
    Rig rig;
    ReplicationServer server(rig.game, {"Workspace"});
    std::uint16_t pvProperty = 0, ownerProperty = 0;
    const Schema schema = withPartIsr(server.schema(), pvProperty, &ownerProperty);
    Replica client;
    for (const auto& m : server.joinMessages(16 * 1024, ReplicationServer::JoinStyle::NewItems))
        CHECK(client.handle(!m.empty() && m[0] == kSchemaPacketId ? encodeSchemaPacket(schema) : m));
    if (!client.globals)
        client.globals = server.globals(); // ID_SET_GLOBALS: our peer id
    CHECK(client.globals && client.globals->peerId > 1);
    const std::uint32_t us = client.globals ? client.globals->peerId : 2;
    const InstanceId looseId = server.idOf(rig.loose.get());
    const InstancePtr loose = client.find(looseId);
    CHECK(loose != nullptr);
    auto give = [&](std::uint32_t peer) {
        ChangePropertyItem c;
        c.id = looseId;
        c.propertyId = ownerProperty;
        c.value = Value(static_cast<double>(peer));
        CHECK(client.handle(encodeData(std::vector<Item>{Item(c)}, schema, client.context())));
    };
    const std::uint64_t changes = client.ownershipChanges();
    give(1); // the server's
    CHECK(!client.ownedHere(looseId));
    CHECK(client.partsOwnedHere().empty());
    give(us);
    CHECK(client.ownedHere(looseId));
    CHECK(client.partsOwnedHere() == std::vector<InstanceId>{looseId});
    CHECK_EQ(client.ownershipChanges(), changes + 2);

    client.simulatedHere = [&](const BasePart& part) {
        const auto id = client.idOf(&part);
        return id && client.ownedHere(*id);
    };
    const auto partClass = *schema.classId("Part");
    auto sample = [&](std::uint32_t tick, Vector3 at, double arrival) {
        PhysicsPV pv;
        pv.cframe = CFrame(at);
        NetworkStream data;
        writeReplicationPV(data, pv);
        IsrPacket packet;
        packet.tick = tick;
        packet.groups.push_back({0, 0, {{looseId, partClass, 3, 4, {data.bytes().begin(), data.bytes().end()}}}});
        client.receiveTime = arrival;
        CHECK(client.handle(encodeIsrPacket(packet)));
    };
    client.stepMotion(0.0); // stepping: samples wait for the playback
    const CFrame before = cframeOf(loose);
    sample(2400, {20, 1, 0}, 10.0);
    sample(2424, {21, 1, 0}, 10.1);
    client.stepMotion(10.5);
    CHECK(cframeOf(loose) == before); // ours: what the server sends isn't played
    // Taken back: its next samples play.
    give(1);
    sample(2640, {30, 1, 0}, 11.0);
    client.stepMotion(12.0);
    CHECK(near(cframeOf(loose).position, {30, 1, 0}, 1e-2f));
}

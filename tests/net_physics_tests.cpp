#include "framework.hpp"

#include <lunar/assets/Json.hpp>
#include <lunar/net/NetworkStream.hpp>
#include <lunar/net/Physics.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace lunar;
using namespace lunar::net;

namespace {

std::string toHex(std::span<const std::uint8_t> bytes)
{
    std::string out;
    for (std::uint8_t b : bytes)
        out += std::format("{:02x}", b);
    return out;
}

assets::json::Value vectors()
{
    const std::filesystem::path path
        = std::filesystem::path(LUNAR_RESEARCH_DIR) / "net/emu/vectors/replication_pv.json";
    if (!std::filesystem::exists(path))
        SKIP("no emulator vectors here");
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = assets::json::parse(ss.str());
    if (!doc)
        lt::fail(std::format("cannot read {}", path.string()), std::source_location::current());
    return *doc;
}

std::vector<float> floats(const assets::json::Value& v)
{
    std::vector<float> out;
    if (const auto* a = v.array())
        for (const auto& e : *a)
            out.push_back(static_cast<float>(e.number().value_or(0.0)));
    return out;
}

Vector3 vec3(const assets::json::Value& v)
{
    const auto f = floats(v);
    return {f.at(0), f.at(1), f.at(2)};
}

} // namespace

// Every position/velocity/PV the client's own quantizers produced under
// emulation (emu/t_pv.py) must come out of ours byte for byte.
TEST(net_physics_position_matches_client)
{
    const auto v = vectors();
    std::size_t n = 0;
    for (const auto& c : *v["position"]->array()) {
        NetworkStream out;
        writePvPosition(out, vec3(*c["v"]));
        CHECK_EQ(toHex(out.bytes()), *c["wire"]->string());
        NetworkStream in(out.bytes());
        const Vector3 back = readPvPosition(in);
        CHECK_EQ(in.offset(), out.bytes().size());
        const auto want = vec3(*c["decoded"]);
        CHECK(back == want);
        ++n;
    }
    CHECK(n > 0);
}

TEST(net_physics_velocity_matches_client)
{
    const auto v = vectors();
    for (const auto& c : *v["velocity"]->array()) {
        NetworkStream out;
        writePvVelocity(out, vec3(*c["v"]));
        CHECK_EQ(toHex(out.bytes()), *c["wire"]->string());
        NetworkStream in(out.bytes());
        CHECK(readPvVelocity(in) == vec3(*c["decoded"]));
        CHECK_EQ(in.offset(), out.bytes().size());
    }
    for (const auto& c : *v["velocity6"]->array()) {
        NetworkStream out;
        writePvVelocity6(out, vec3(*c["v"]));
        CHECK_EQ(toHex(out.bytes()), *c["wire"]->string());
        NetworkStream in(out.bytes());
        CHECK(readPvVelocity6(in) == vec3(*c["decoded"]));
        CHECK_EQ(in.offset(), out.bytes().size());
    }
}

TEST(net_physics_quaternion10_matches_client)
{
    const auto v = vectors();
    for (const auto& c : *v["quat10"]->array()) {
        const auto& rows = *c["rows"]->array();
        CFrame cf;
        for (int i = 0; i < 3; ++i) {
            const auto r = floats(rows.at(static_cast<std::size_t>(i)));
            for (int j = 0; j < 3; ++j)
                cf.r[i][j] = r.at(static_cast<std::size_t>(j));
        }
        NetworkStream out;
        writeQuaternion10(out, cf);
        CHECK_EQ(toHex(out.bytes()), *c["wire"]->string());
    }
}

TEST(net_physics_replication_pv_matches_client)
{
    const auto v = vectors();
    for (const auto& c : *v["pv"]->array()) {
        const auto pv16 = floats(*c["pv"]);
        PhysicsPV pv;
        pv.cframe.position = {pv16.at(0), pv16.at(1), pv16.at(2)};
        // rot7 = col0 (R00,R10,R20), col1 (R01,R11,R21), R02; rebuild the matrix.
        const float a = pv16.at(3), b = pv16.at(4), cc = pv16.at(5), d = pv16.at(6), e = pv16.at(7), ff = pv16.at(8),
                    g = pv16.at(9);
        pv.cframe.r[0][0] = a;
        pv.cframe.r[1][0] = b;
        pv.cframe.r[2][0] = cc;
        pv.cframe.r[0][1] = d;
        pv.cframe.r[1][1] = e;
        pv.cframe.r[2][1] = ff;
        pv.cframe.r[0][2] = g;
        pv.cframe.r[1][2] = static_cast<float>(static_cast<double>(cc) * static_cast<double>(d)
                                               - static_cast<double>(ff) * static_cast<double>(a));
        pv.cframe.r[2][2] = static_cast<float>(static_cast<double>(e) * static_cast<double>(a)
                                               - static_cast<double>(b) * static_cast<double>(d));
        pv.linearVelocity = {pv16.at(10), pv16.at(11), pv16.at(12)};
        pv.angularVelocity = {pv16.at(13), pv16.at(14), pv16.at(15)};

        NetworkStream out;
        writeReplicationPV(out, pv);
        CHECK_EQ(toHex(out.bytes()), *c["wire"]->string());

        NetworkStream in(out.bytes());
        const PhysicsPV back = readReplicationPV(in);
        CHECK_EQ(in.offset(), out.bytes().size());
        const auto decoded = floats(*c["decoded"]);
        // Position and velocities round-trip to the same quantized floats.
        CHECK(back.cframe.position == Vector3{decoded.at(0), decoded.at(1), decoded.at(2)});
        CHECK(back.linearVelocity == Vector3{decoded.at(10), decoded.at(11), decoded.at(12)});
        CHECK(back.angularVelocity == Vector3{decoded.at(13), decoded.at(14), decoded.at(15)});
    }
}

namespace {

PhysicsPV pvFrom16(const std::vector<float>& p)
{
    PhysicsPV pv;
    pv.cframe.position = {p.at(0), p.at(1), p.at(2)};
    const double a = p.at(3), b = p.at(4), cc = p.at(5), d = p.at(6), e = p.at(7), ff = p.at(8), g = p.at(9);
    pv.cframe.r[0][0] = static_cast<float>(a);
    pv.cframe.r[1][0] = static_cast<float>(b);
    pv.cframe.r[2][0] = static_cast<float>(cc);
    pv.cframe.r[0][1] = static_cast<float>(d);
    pv.cframe.r[1][1] = static_cast<float>(e);
    pv.cframe.r[2][1] = static_cast<float>(ff);
    pv.cframe.r[0][2] = static_cast<float>(g);
    pv.cframe.r[1][2] = static_cast<float>(cc * d - ff * a);
    pv.cframe.r[2][2] = static_cast<float>(e * a - b * d);
    pv.linearVelocity = {p.at(10), p.at(11), p.at(12)};
    pv.angularVelocity = {p.at(13), p.at(14), p.at(15)};
    return pv;
}

} // namespace

// The whole physics packet, both directions, against the layout model's bytes
// (vectors/replication_pv.json physics_packet_model).
TEST(net_physics_packet_matches_model)
{
    const auto v = vectors();
    const auto& model = *v["physics_packet_model"];
    const PhysicsPV pv = pvFrom16(floats(*model["pv"]));
    const InstanceId part{1, 42};

    const PhysicsPacket s2c = onePartPacket(123456, 1, true, part, pv);
    CHECK_EQ(toHex(encodePhysicsPacket(s2c)), *model["s2c"]->string());

    const PhysicsPacket c2s = onePartPacket(123457, 7, false, part, pv);
    CHECK_EQ(toHex(encodePhysicsPacket(c2s)), *model["c2s"]->string());

    // Round trip both ways.
    for (const PhysicsPacket& p : {s2c, c2s}) {
        const auto wire = encodePhysicsPacket(p);
        auto back = decodePhysicsPacket(wire, p.serverToClient);
        CHECK(back.has_value());
        if (!back)
            continue;
        CHECK_EQ(back->timestamp, p.timestamp);
        CHECK_EQ(back->sequence, p.sequence);
        CHECK_EQ(back->records.size(), 1u);
        CHECK(back->records.at(0).part == part);
        const auto& mech = back->records.at(0).mechanism;
        CHECK(mech.has_value());
        if (!mech)
            continue;
        const auto& frames = mech->parts.at(0).frames;
        CHECK_EQ(frames.size(), 1u);
        // The decoded PV equals the re-encoded PV (quantization is idempotent).
        NetworkStream a1;
        writeReplicationPV(a1, frames.at(0).pv);
        NetworkStream a2;
        writeReplicationPV(a2, pv);
        CHECK(std::vector<std::uint8_t>(a1.bytes().begin(), a1.bytes().end())
              == std::vector<std::uint8_t>(a2.bytes().begin(), a2.bytes().end()));
    }
}

#include <lunar/net/Physics.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>

// The quantizers RobloxPlayer 0.742 uses for physics replication, ported from
// the client's own code (pinned under unicorn: emu/t_pv.py, matching 400/400
// positions, 400/400 velocities, 300/300 PVs both ways, 300/300 compact
// velocities, 200/200 10-bit quaternions). All float arithmetic is kept in
// f32 the same places the binary does, so the wire bytes come out identical.

namespace lunar::net {

namespace {

float f32(double x)
{
    return static_cast<float>(x);
}

// frexpf's exponent, 0 for zero or non-finite (the binary's _frexpf wrapper).
int frexpExponent(float x)
{
    if (x == 0.0f || !std::isfinite(x))
        return 0;
    int e = 0;
    std::frexp(x, &e);
    return e;
}

// clamp(v/s, -1, 1) * k, truncated toward zero with a half-ULP nudge; the
// cvttss2si of `sub_1057acf..`-style quantizers (qround in t_pv.py).
int quantize(double v, double s, double k)
{
    float c = f32(v / s);
    c = std::clamp(c, -1.0f, 1.0f);
    const float scaled = f32(static_cast<double>(c) * k);
    const float nudged = f32(static_cast<double>(scaled) + (v >= 0 ? 0.5 : -0.5));
    return static_cast<int>(nudged);
}

float maxAbs(Vector3 v)
{
    return std::max({std::abs(f32(v.x)), std::abs(f32(v.y)), std::abs(f32(v.z))});
}

int signExtend(std::uint32_t v, int bits)
{
    v &= (1u << bits) - 1;
    return (v >> (bits - 1)) ? static_cast<int>(v) - (1 << bits) : static_cast<int>(v);
}

double pow2(int e)
{
    return std::ldexp(1.0, e);
}

// f32(f32(q / k) * s): the dequantization order the binary uses.
float dequant(int q, double k, double s)
{
    return f32(static_cast<double>(f32(static_cast<double>(q) / k)) * s);
}

// -- position (0x1054e7088 / 0x1054e73fc)
void writePosition(NetworkStream& out, Vector3 v)
{
    const float m = maxAbs(v);
    const int raw = frexpExponent(m);
    const int ex = std::clamp(raw, 0, 31);
    const double s = pow2(ex);
    const std::array<double, 3> comp{v.x, v.y, v.z};
    if (raw <= 4) {
        std::array<int, 3> q{};
        for (std::size_t i = 0; i < 3; ++i)
            q[i] = quantize(comp[i], s, 1023.0);
        const auto h = static_cast<std::uint8_t>(ex << 3 | ((q[0] >> 10) & 1) << 2 | ((q[1] >> 10) & 1) << 1
                                                 | ((q[2] >> 10) & 1));
        out.writeU8(h);
        out.writeU32((static_cast<std::uint32_t>(q[0]) << 20 & 0x3ff00000u)
                     | (static_cast<std::uint32_t>(q[1]) << 10 & 0xffc00u)
                     | (static_cast<std::uint32_t>(q[2]) & 0x3ffu));
    } else if (raw <= 10) {
        std::array<int, 3> q{};
        for (std::size_t i = 0; i < 3; ++i)
            q[i] = quantize(comp[i], s, 65535.0);
        const auto h = static_cast<std::uint8_t>(ex << 3 | ((q[0] >> 16) & 1) << 2 | ((q[1] >> 16) & 1) << 1
                                                 | ((q[2] >> 16) & 1));
        out.writeU8(h);
        out.writeU16(static_cast<std::uint16_t>(q[0]));
        out.writeU16(static_cast<std::uint16_t>(q[1]));
        out.writeU16(static_cast<std::uint16_t>(q[2]));
    } else {
        std::array<int, 3> q{};
        for (std::size_t i = 0; i < 3; ++i)
            q[i] = quantize(comp[i], s, 2097151.0);
        const auto h = static_cast<std::uint8_t>(ex << 3 | ((q[0] >> 21) & 1) << 2 | ((q[1] >> 21) & 1) << 1
                                                 | ((q[2] >> 21) & 1));
        out.writeU8(h);
        const std::uint32_t a
            = (static_cast<std::uint32_t>(q[0]) << 11 | (static_cast<std::uint32_t>(q[1]) >> 10 & 0x7ff));
        const std::uint32_t b
            = (static_cast<std::uint32_t>(q[1]) << 21 & 0x7fe00000u) | (static_cast<std::uint32_t>(q[2]) & 0x1fffffu);
        out.writeU32(a);
        out.writeU32(b);
    }
}

Vector3 readPosition(NetworkStream& in)
{
    const std::uint8_t h = in.readU8();
    const int ex = h >> 3;
    const std::array<int, 3> sg{(h >> 2) & 1, (h >> 1) & 1, h & 1};
    std::array<int, 3> q{};
    double k = 0;
    if (h <= 0x27) {
        const std::uint32_t w = in.readU32();
        k = 1023.0;
        q = {signExtend(static_cast<std::uint32_t>(sg[0]) << 10 | (w >> 20 & 0x3ff), 11),
             signExtend(static_cast<std::uint32_t>(sg[1]) << 10 | (w >> 10 & 0x3ff), 11),
             signExtend(static_cast<std::uint32_t>(sg[2]) << 10 | (w & 0x3ff), 11)};
    } else if (h <= 0x57) {
        const std::uint32_t a = in.readU16(), b = in.readU16(), c = in.readU16();
        k = 65535.0;
        q = {signExtend(static_cast<std::uint32_t>(sg[0]) << 16 | a, 17),
             signExtend(static_cast<std::uint32_t>(sg[1]) << 16 | b, 17),
             signExtend(static_cast<std::uint32_t>(sg[2]) << 16 | c, 17)};
    } else {
        const std::uint32_t a = in.readU32(), b = in.readU32();
        k = 2097151.0;
        q = {signExtend(static_cast<std::uint32_t>(sg[0]) << 21 | (a >> 11), 22),
             signExtend(static_cast<std::uint32_t>(sg[1]) << 21 | (a & 0x7ff) << 10 | (b >> 21 & 0x3ff), 22),
             signExtend(static_cast<std::uint32_t>(sg[2]) << 21 | (b & 0x1fffff), 22)};
    }
    const double s = pow2(ex);
    return {dequant(q[0], k, s), dequant(q[1], k, s), dequant(q[2], k, s)};
}

// -- velocity, 11-bit (0x1054e7b92 / 0x1054e7d36)
void writeVelocity(NetworkStream& out, Vector3 v)
{
    const float m = maxAbs(v);
    if (f32(0.001) > m) {
        out.writeU8(0);
        return;
    }
    const int ex = std::clamp(frexpExponent(m), 0, 14);
    const double s = pow2(ex);
    const std::array<int, 3> q{quantize(v.x, s, 2047.0), quantize(v.y, s, 2047.0), quantize(v.z, s, 2047.0)};
    out.writeU8(static_cast<std::uint8_t>((ex + 1) << 4 | (q[2] & 0xf)));
    out.writeU32((static_cast<std::uint32_t>(q[0]) & 0xfff) << 20 | (static_cast<std::uint32_t>(q[1]) & 0xfff) << 8
                 | (static_cast<std::uint32_t>(q[2]) >> 4 & 0xff));
}

Vector3 readVelocity(NetworkStream& in)
{
    const std::uint8_t h = in.readU8();
    if (h == 0)
        return {};
    const std::uint32_t w = in.readU32();
    const double s = pow2((h >> 4) - 1);
    const std::array<int, 3> q{signExtend(w >> 20, 12), signExtend(w >> 8, 12),
                               signExtend((w & 0xff) << 4 | (h & 0xf), 12)};
    return {dequant(q[0], 2047.0, s), dequant(q[1], 2047.0, s), dequant(q[2], 2047.0, s)};
}

// -- compact 6-bit velocity (0x1054e7dda / 0x1054e7f8c)
void writeVelocity6(NetworkStream& out, Vector3 v)
{
    const float m = maxAbs(v);
    if (f32(0.001) > m) {
        out.writeU8(0);
        return;
    }
    const int ex = std::clamp(frexpExponent(m), -16, 15);
    const double s = pow2(ex);
    const std::array<int, 3> q{quantize(v.x, s, 31.0), quantize(v.y, s, 31.0), quantize(v.z, s, 31.0)};
    const int qy = q[1] & 63, qz = q[2] & 63;
    out.writeU8(static_cast<std::uint8_t>(((ex << 2) + ((q[0] & 0x30) >> 4)) | 0x80));
    out.writeU8(static_cast<std::uint8_t>((q[0] << 4) | (qy >> 2)));
    out.writeU8(static_cast<std::uint8_t>(qz | (qy << 6)));
}

Vector3 readVelocity6(NetworkStream& in)
{
    const std::uint8_t h = in.readU8();
    if (h == 0)
        return {};
    const std::uint8_t b1 = in.readU8(), b2 = in.readU8();
    const double s = pow2(signExtend(h >> 2, 5));
    const std::array<int, 3> q{signExtend((h & 3) << 4 | b1 >> 4, 6), signExtend((b1 & 15) << 2 | b2 >> 6, 6),
                               signExtend(b2, 6)};
    return {dequant(q[0], 31.0, s), dequant(q[1], 31.0, s), dequant(q[2], 31.0, s)};
}

// -- 10-bit smallest-three quaternion (0x1054e78de / 0x1054e7a98)
std::array<float, 4> matrixToQuaternion(const CFrame& cf)
{
    const float m00 = cf.r[0][0], m01 = cf.r[0][1], m02 = cf.r[0][2];
    const float m10 = cf.r[1][0], m11 = cf.r[1][1], m12 = cf.r[1][2];
    const float m20 = cf.r[2][0], m21 = cf.r[2][1], m22 = cf.r[2][2];
    const float tr = m00 + m11 + m22;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1) * 2;
        return {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / 4};
    }
    if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        return {s / 4, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    }
    if (m11 > m22) {
        const float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        return {(m01 + m10) / s, s / 4, (m12 + m21) / s, (m02 - m20) / s};
    }
    const float s = std::sqrt(1 + m22 - m00 - m11) * 2;
    return {(m02 + m20) / s, (m12 + m21) / s, s / 4, (m10 - m01) / s};
}

constexpr float kQuatScale = 0.70710677f; // 1/sqrt(2), the value in the binary

// A quaternion to a row-major rotation (the standard x2/y2/z2 formula).
CFrame matrixFromQuaternion(float x, float y, float z, float w)
{
    CFrame cf;
    cf.r[0][0] = 1 - 2 * (y * y + z * z);
    cf.r[0][1] = 2 * (x * y - w * z);
    cf.r[0][2] = 2 * (x * z + w * y);
    cf.r[1][0] = 2 * (x * y + w * z);
    cf.r[1][1] = 1 - 2 * (x * x + z * z);
    cf.r[1][2] = 2 * (y * z - w * x);
    cf.r[2][0] = 2 * (x * z - w * y);
    cf.r[2][1] = 2 * (y * z + w * x);
    cf.r[2][2] = 1 - 2 * (x * x + y * y);
    return cf;
}

} // namespace

void writePvPosition(NetworkStream& out, Vector3 v)
{
    writePosition(out, v);
}
Vector3 readPvPosition(NetworkStream& in)
{
    return readPosition(in);
}
void writePvVelocity(NetworkStream& out, Vector3 v)
{
    writeVelocity(out, v);
}
Vector3 readPvVelocity(NetworkStream& in)
{
    return readVelocity(in);
}
void writePvVelocity6(NetworkStream& out, Vector3 v)
{
    writeVelocity6(out, v);
}
Vector3 readPvVelocity6(NetworkStream& in)
{
    return readVelocity6(in);
}

void writeQuaternion10(NetworkStream& out, const CFrame& cf)
{
    std::array<float, 4> q = matrixToQuaternion(cf);
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& c : q)
        c = f32(c / len);
    std::size_t i = std::abs(q[1]) > std::abs(q[0]) ? 1 : 0;
    i = std::abs(q[2]) > std::abs(q[i]) ? 2 : i;
    i = std::abs(q[3]) > std::abs(q[i]) ? 3 : i;
    if (q[i] < 0)
        for (float& c : q)
            c = -c;
    static constexpr std::array<std::array<std::size_t, 3>, 4> rows{{{1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}}};
    auto qz = [](float c) {
        const float t = f32(static_cast<double>(f32(static_cast<double>(c) / static_cast<double>(kQuatScale))) * 511.0);
        return static_cast<std::uint32_t>(static_cast<int>(f32(static_cast<double>(t) + (c >= 0 ? 0.5 : -0.5))))
               & 0x3ffu;
    };
    const auto& r = rows[i];
    out.writeU32(static_cast<std::uint32_t>(i) << 30 | qz(q[r[0]]) << 20 | qz(q[r[1]]) << 10 | qz(q[r[2]]));
}

CFrame readQuaternion10(NetworkStream& in)
{
    const std::uint32_t w = in.readU32();
    const std::size_t i = w >> 30;
    auto unq = [](std::uint32_t v) { return f32(f32(static_cast<double>(signExtend(v, 10)) / 511.0) * kQuatScale); };
    const std::array<float, 3> others{unq(w >> 20), unq(w >> 10), unq(w)};
    static constexpr std::array<std::array<std::size_t, 3>, 4> rows{{{1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}}};
    std::array<float, 4> q{};
    float sum = 0;
    for (std::size_t k = 0; k < 3; ++k) {
        q[rows[i][k]] = others[k];
        sum += others[k] * others[k];
    }
    q[i] = std::sqrt(std::max(0.0f, 1.0f - sum));
    return matrixFromQuaternion(q[0], q[1], q[2], q[3]);
}

// -- ReplicationPV (0x1057ad024 / 0x1057ad1de)

namespace {

// The in-memory PV keeps 7 matrix entries (column 0, column 1, R02); the
// writer rebuilds R12/R22 by cross product (pv_rot_matrix in t_pv.py).
std::array<float, 7> rot7Of(const CFrame& cf)
{
    return {cf.r[0][0], cf.r[1][0], cf.r[2][0], cf.r[0][1], cf.r[1][1], cf.r[2][1], cf.r[0][2]};
}

CFrame cframeFromRot7(const std::array<float, 7>& rot7, Vector3 position)
{
    const float a = rot7[0], b = rot7[1], c = rot7[2], d = rot7[3], e = rot7[4], ff = rot7[5], g = rot7[6];
    CFrame cf;
    cf.position = position;
    cf.r[0][0] = a;
    cf.r[0][1] = d;
    cf.r[0][2] = g;
    cf.r[1][0] = b;
    cf.r[1][1] = e;
    cf.r[1][2]
        = f32(static_cast<double>(c) * static_cast<double>(d) - static_cast<double>(ff) * static_cast<double>(a));
    cf.r[2][0] = c;
    cf.r[2][1] = ff;
    cf.r[2][2] = f32(static_cast<double>(e) * static_cast<double>(a) - static_cast<double>(b) * static_cast<double>(d));
    return cf;
}

// index 0..5 for an axis-aligned unit column (x+,y+,z+,x-,y-,z-), else 6.
int axisOf(float c0, float c1, float c2, float tol)
{
    const std::array<float, 3> v{c0, c1, c2};
    for (std::size_t i = 0; i < 3; ++i)
        if (tol >= std::abs(f32(std::abs(v[i]) - 1.0f)))
            return static_cast<int>(i) + (v[i] > 0 ? 0 : 3);
    return 6;
}

// The 36-entry index (6*axisOf(col0) + axisOf(col1)) -> the low-5 flag value
// (0 means "use the quaternion"). From the tolerance table in the binary.
constexpr std::array<int, 36> kOrient{0, 1,  2,  0, 3,  4,  5,  0, 6,  7,  0, 8,  9,  10, 0, 11, 12, 0,
                                      0, 13, 14, 0, 15, 16, 17, 0, 18, 19, 0, 20, 21, 22, 0, 23, 24, 0};

int orientId(const std::array<float, 7>& rot7, float tol)
{
    const int a = axisOf(rot7[0], rot7[1], rot7[2], tol);
    if (a == 6)
        return 0xff;
    const int b = axisOf(rot7[3], rot7[4], rot7[5], tol);
    return b == 6 ? 0xff : 6 * a + b;
}

Vector3 axisVector(int a)
{
    static constexpr std::array<Vector3, 6> v{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}}};
    return v[static_cast<std::size_t>(a)];
}

} // namespace

std::uint8_t replicationPvFlags(const PhysicsPV& pv)
{
    std::uint8_t f = 0;
    const float posCut = f32(std::pow(10.0, std::clamp(std::min(kPvTranslationZeroCutoffExponent, 0), -10, 0)));
    if (posCut > maxAbs(pv.cframe.position))
        f |= 0x80;
    if (f32(std::pow(10.0, kPvLinearVelocityZeroCutoffExponent)) > maxAbs(pv.linearVelocity))
        f |= 0x40;
    if (f32(std::pow(10.0, kPvRotationalVelocityZeroCutoffExponent)) > maxAbs(pv.angularVelocity))
        f |= 0x20;
    const int id = orientId(rot7Of(pv.cframe), f32(std::pow(10.0, kPvRotationOrientIdToleranceExponent)));
    if (id <= 0x23)
        f |= static_cast<std::uint8_t>(kOrient[static_cast<std::size_t>(id)]);
    return f;
}

namespace {

// The ReplicationPV body with the velocity form chosen by `compact`: the
// 0x39 value type is never compact, the physics packet is when its mechanism
// header has bit 6.
void writePvBody(NetworkStream& out, const PhysicsPV& pv, bool compact)
{
    const std::uint8_t f = replicationPvFlags(pv);
    out.writeU8(f);
    if (!(f & 0x80))
        writePosition(out, pv.cframe.position);
    const std::uint8_t low = f & 0x1f;
    if (low == 0)
        writeQuaternion(out, pv.cframe); // 0x1054e75c6, the 16383-scale quaternion
    else if (low == 0x1f) {
        const auto rot7 = rot7Of(pv.cframe);
        for (float c : rot7)
            out.writeF32(c);
    }
    if (!(f & 0x40))
        (compact ? writeVelocity6 : writeVelocity)(out, pv.linearVelocity);
    if (!(f & 0x20))
        (compact ? writeVelocity6 : writeVelocity)(out, pv.angularVelocity);
}

PhysicsPV readPvBody(NetworkStream& in, bool compact)
{
    PhysicsPV pv;
    const std::uint8_t f = in.readU8();
    Vector3 position;
    if (!(f & 0x80))
        position = readPosition(in);
    const std::uint8_t low = f & 0x1f;
    if (low == 0x1f) {
        std::array<float, 7> rot7{};
        for (float& c : rot7)
            c = in.readF32();
        pv.cframe = cframeFromRot7(rot7, position);
    } else if (low == 0) {
        pv.cframe = readQuaternion(in);
    } else {
        // Reverse the orient table: low -> 36-index -> two axis columns.
        const auto it = std::ranges::find(kOrient, static_cast<int>(low));
        const int code = (it == kOrient.end() || it == kOrient.begin())
                             ? -1
                             : static_cast<int>(std::distance(kOrient.begin(), it));
        CFrame cf;
        if (code >= 0) {
            const Vector3 c0 = axisVector(code / 6), c1 = axisVector(code % 6), c2 = c0.cross(c1);
            const std::array<Vector3, 3> cols{c0, c1, c2};
            for (std::size_t c = 0; c < 3; ++c) {
                cf.r[0][c] = cols[c].x;
                cf.r[1][c] = cols[c].y;
                cf.r[2][c] = cols[c].z;
            }
        }
        pv.cframe = cf;
    }
    pv.cframe.position = position;
    if (!(f & 0x40))
        pv.linearVelocity = (compact ? readVelocity6 : readVelocity)(in);
    if (!(f & 0x20))
        pv.angularVelocity = (compact ? readVelocity6 : readVelocity)(in);
    return pv;
}

} // namespace

void writeReplicationPV(NetworkStream& out, const PhysicsPV& pv)
{
    writePvBody(out, pv, false);
}

PhysicsPV readReplicationPV(NetworkStream& in)
{
    return readPvBody(in, false);
}

std::string pvText(const PhysicsPV& pv)
{
    const Vector3 p = pv.cframe.position;
    const Vector3 look = pv.cframe.lookVector();
    return std::format("pos ({}, {}, {}) look ({:.3f}, {:.3f}, {:.3f}) lin ({}, {}, {}) ang ({}, {}, {})", p.x, p.y,
                       p.z, look.x, look.y, look.z, pv.linearVelocity.x, pv.linearVelocity.y, pv.linearVelocity.z,
                       pv.angularVelocity.x, pv.angularVelocity.y, pv.angularVelocity.z);
}

// -- the physics packet

namespace {

constexpr std::uint8_t kTimestampId = 0x1b;
constexpr std::uint8_t kPhysicsId = 0x85;

void refToStream(NetworkStream& out, InstanceId id)
{
    writeInstanceId(out, id);
}

} // namespace

std::vector<std::uint8_t> encodePhysicsPacket(const PhysicsPacket& packet, const Framing& framing)
{
    NetworkStream out;
    out.writeU8(kTimestampId); // < 0x80, never remapped
    out.writeU64(packet.timestamp);
    const std::uint8_t physId = framing.remapIds ? encodePacketId(kPhysicsId) : kPhysicsId;
    if (packet.timestampIdFirst) {
        out.writeU8(physId);
        out.writeU64(packet.sequence);
    } else {
        out.writeU64(packet.sequence);
        out.writeU8(physId);
    }
    for (const PhysicsRecord& rec : packet.records) {
        refToStream(out, rec.part);
        out.writeU8(0); // no FACS tracker data
        if (!rec.mechanism) {
            out.writeU8(0);
            continue;
        }
        out.writeU8(1);
        const PhysicsMechanism& mech = *rec.mechanism;
        std::uint8_t h = mech.humanoidState & 0x1f;
        if (mech.parts.size() == 1)
            h |= 0x20;
        if (mech.compactVelocities)
            h |= 0x40;
        if (mech.refBasis)
            h |= 0x80;
        out.writeU8(h);
        for (std::size_t k = 0; k < mech.parts.size(); ++k) {
            const PhysicsPartState& part = mech.parts[k];
            if (k != 0)
                refToStream(out, part.ref);
            out.writeVarint(part.joints.size());
            for (const PhysicsJoint& j : part.joints) {
                out.writeU8(static_cast<std::uint8_t>((j.hasTranslation ? 1 : 0) | (j.hasRotation ? 2 : 0)));
                if (j.hasTranslation)
                    writePosition(out, j.translation);
                if (j.hasRotation)
                    writeQuaternion10(out, j.rotation);
            }
            if (packet.serverToClient)
                out.writeU8(static_cast<std::uint8_t>(part.frames.size()));
            for (const PhysicsFrame& fr : part.frames) {
                if (packet.serverToClient)
                    out.writeF32(fr.age);
                writePvBody(out, fr.pv, mech.compactVelocities);
                if (k == 0 || mech.refBasis)
                    refToStream(out, fr.rootRef);
                else
                    out.writeU8(fr.basis);
            }
        }
        if (mech.parts.size() > 1)
            out.writeU8(0); // null ref ends the other-parts list
    }
    out.writeU8(0); // null ref ends the packet
    return std::move(out).take();
}

std::expected<PhysicsPacket, std::string> decodePhysicsPacket(std::span<const std::uint8_t> message,
                                                              bool serverToClient, const Framing& framing, Node* tree)
{
    NetworkStream in(message);
    if (in.readU8() != kTimestampId)
        return std::unexpected("physics packet does not start with ID_TIMESTAMP");
    PhysicsPacket packet;
    packet.serverToClient = serverToClient;
    packet.timestamp = in.readU64();
    const std::uint8_t physId = framing.remapIds ? encodePacketId(kPhysicsId) : kPhysicsId;
    // The 0x85 is before or after the sequence (capability bit 25).
    const std::size_t save = in.offset();
    std::uint8_t first = in.readU8();
    if (first == physId) {
        packet.timestampIdFirst = true;
        packet.sequence = in.readU64();
    } else {
        in.seek(save);
        packet.sequence = in.readU64();
        if (in.readU8() != physId)
            return std::unexpected("physics packet has no ID_PHYSICS after the timestamp");
    }
    // A server sends frame lists, the owner client a single PV; the mode is
    // not on the wire, so the caller states it.
    const bool frames = packet.serverToClient;
    Node* recordsNode = tree ? &tree->add("ID_PHYSICS (0x85)") : nullptr;
    if (recordsNode)
        recordsNode->add(std::format("timestamp: {} ms, sequence: {}", packet.timestamp, packet.sequence));
    while (!in.bad()) {
        const InstanceId part = readInstanceId(in);
        if (part.null())
            break;
        PhysicsRecord rec;
        rec.part = part;
        const std::uint8_t hasFacs = in.readU8();
        if (hasFacs)
            return std::unexpected("physics packet FACS tracker data not supported");
        const std::uint8_t hasMech = in.readU8();
        if (hasMech) {
            PhysicsMechanism mech;
            const std::uint8_t h = in.readU8();
            mech.humanoidState = h & 0x1f;
            mech.compactVelocities = (h & 0x40) != 0;
            mech.refBasis = (h & 0x80) != 0;
            const bool onlyRoot = (h & 0x20) != 0;
            bool firstPart = true;
            while (!in.bad()) {
                PhysicsPartState ps;
                if (!firstPart) {
                    ps.ref = readInstanceId(in);
                    if (ps.ref.null())
                        break;
                }
                const std::uint64_t nJoints = in.readVarint();
                for (std::uint64_t j = 0; j < nJoints && !in.bad(); ++j) {
                    PhysicsJoint joint;
                    const std::uint8_t jf = in.readU8();
                    joint.hasTranslation = (jf & 1) != 0;
                    joint.hasRotation = (jf & 2) != 0;
                    if (joint.hasTranslation)
                        joint.translation = readPosition(in);
                    if (joint.hasRotation)
                        joint.rotation = readQuaternion10(in);
                    ps.joints.push_back(joint);
                }
                const std::uint8_t nFrames = frames ? in.readU8() : 1;
                for (std::uint8_t fi = 0; fi < nFrames && !in.bad(); ++fi) {
                    PhysicsFrame fr;
                    if (frames)
                        fr.age = in.readF32();
                    fr.pv = readPvBody(in, mech.compactVelocities);
                    if (firstPart || mech.refBasis)
                        fr.rootRef = readInstanceId(in);
                    else
                        fr.basis = in.readU8();
                    ps.frames.push_back(fr);
                }
                mech.parts.push_back(std::move(ps));
                if (firstPart && onlyRoot)
                    break;
                firstPart = false;
            }
            rec.mechanism = std::move(mech);
        }
        if (recordsNode) {
            Node& rn = recordsNode->add(std::format("part scope {} index {}", rec.part.scope, rec.part.index));
            if (rec.mechanism)
                for (const PhysicsPartState& ps : rec.mechanism->parts)
                    for (const PhysicsFrame& fr : ps.frames)
                        rn.add(pvText(fr.pv));
        }
        packet.records.push_back(std::move(rec));
    }
    if (in.bad())
        return std::unexpected("physics packet truncated");
    if (in.offset() != message.size())
        return std::unexpected("physics packet has trailing bytes");
    return packet;
}

PhysicsPacket onePartPacket(std::uint64_t timestamp, std::uint64_t sequence, bool serverToClient, InstanceId part,
                            const PhysicsPV& pv, std::uint8_t humanoidState, float age)
{
    PhysicsPacket packet;
    packet.timestamp = timestamp;
    packet.sequence = sequence;
    packet.serverToClient = serverToClient;
    PhysicsFrame fr;
    fr.age = age;
    fr.pv = pv;
    fr.rootRef = part; // the single part is its own replication root
    PhysicsPartState ps;
    ps.ref = part;
    ps.frames.push_back(fr);
    PhysicsMechanism mech;
    mech.humanoidState = humanoidState;
    mech.parts.push_back(std::move(ps));
    packet.records.push_back({part, std::move(mech)});
    return packet;
}

// -- ISR (0xa1)

namespace {

constexpr std::uint8_t kIsrId = 0xa1;

} // namespace

std::vector<std::uint8_t> encodeIsrPacket(const IsrPacket& packet)
{
    NetworkStream out;
    out.writeU8(kIsrId);
    out.writeU32(packet.tick);
    out.writeU32(static_cast<std::uint32_t>(packet.groups.size()));
    out.writeI32(packet.clockOffset);
    for (const IsrGroup& group : packet.groups) {
        out.writeU16(static_cast<std::uint16_t>(group.instances.size()));
        out.writeU8(group.kind);
        out.writeU8(group.age);
        for (const IsrInstanceState& state : group.instances) {
            writeInstanceId(out, state.instance);
            out.writeU16(state.classId);
            out.writeU16(state.propertyCount);
            out.writeU64(state.dirty);
            out.writeU16(static_cast<std::uint16_t>(state.data.size()));
            out.writeBytes(state.data);
        }
    }
    return std::move(out).take();
}

std::expected<IsrPacket, std::string> decodeIsrPacket(std::span<const std::uint8_t> message)
{
    NetworkStream in(message);
    if (in.readU8() != kIsrId)
        return std::unexpected("not an ISR packet (0xa1)");
    IsrPacket packet;
    packet.tick = in.readU32();
    const std::uint32_t groups = in.readU32();
    packet.clockOffset = in.readI32();
    for (std::uint32_t g = 0; g < groups && !in.bad(); ++g) {
        IsrGroup group;
        const std::uint16_t count = in.readU16();
        group.kind = in.readU8();
        group.age = in.readU8();
        if (group.kind >= 4)
            return std::unexpected(std::format("ISR group kind {} (the client takes 0..3)", group.kind));
        for (std::uint16_t i = 0; i < count && !in.bad(); ++i) {
            IsrInstanceState state;
            state.instance = readInstanceId(in);
            state.classId = in.readU16();
            state.propertyCount = in.readU16();
            state.dirty = in.readU64();
            const std::uint16_t length = in.readU16();
            if (length > in.left())
                return std::unexpected("ISR instance state runs past the packet");
            state.data = in.readBytes(length);
            group.instances.push_back(std::move(state));
        }
        packet.groups.push_back(std::move(group));
    }
    if (in.bad())
        return std::unexpected("ISR packet truncated");
    if (in.left() != 0)
        return std::unexpected(std::format("ISR packet has {} trailing bytes", in.left()));
    return packet;
}

IsrInstanceState ownPartIsrState(InstanceId part, std::uint16_t classId, std::uint16_t propertyCount, std::size_t pvBit,
                                 const PhysicsPV& pv)
{
    NetworkStream data;
    writeReplicationPV(data, pv);
    IsrInstanceState state;
    state.instance = part;
    state.classId = classId;
    state.propertyCount = propertyCount;
    state.dirty = std::uint64_t{1} << pvBit;
    state.data = std::move(data).take();
    return state;
}

IsrPacket ownPartIsrPacket(std::uint32_t tick, std::int32_t clockOffset, InstanceId part, std::uint16_t classId,
                           std::uint16_t propertyCount, std::size_t pvBit, const PhysicsPV& pv)
{
    IsrGroup group;
    group.instances.push_back(ownPartIsrState(part, classId, propertyCount, pvBit, pv));
    IsrPacket packet;
    packet.tick = tick;
    packet.clockOffset = clockOffset;
    packet.groups.push_back(std::move(group));
    return packet;
}

std::vector<IsrProperty> isrProperties(const IsrInstanceState& state, const Schema& schema, bool* complete)
{
    std::vector<IsrProperty> out;
    if (complete)
        *complete = false;
    const auto cls = std::ranges::find(schema.isrClasses, static_cast<std::uint32_t>(state.classId),
                                       &Schema::IsrClass::classId);
    NetworkStream in(state.data);
    for (std::size_t bit = 0; bit < 64; ++bit) {
        if (!(state.dirty >> bit & 1))
            continue;
        IsrProperty property;
        property.bit = bit;
        if (cls != schema.isrClasses.end() && bit < cls->properties.size())
            property.propertyId = cls->properties[bit];
        if (!property.propertyId || *property.propertyId >= schema.properties.size())
            return out;
        const std::uint8_t type = schema.properties[*property.propertyId].type;
        const std::size_t start = in.offset();
        if (type == nettype::ReplicationPV)
            (void)readReplicationPV(in);
        else if (!readValue(in, type))
            return out;
        if (in.bad())
            return out;
        property.bytes = std::span(state.data).subspan(start, in.offset() - start);
        out.push_back(property);
    }
    if (complete)
        *complete = in.left() == 0;
    return out;
}

} // namespace lunar::net

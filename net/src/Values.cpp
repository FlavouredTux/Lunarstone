#include <lunar/net/Values.hpp>

#include <lunar/net/JoinData.hpp>

#include <lunar/Instance.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <format>

namespace lunar::net {

namespace {

void writeVector3(NetworkStream& out, Vector3 v)
{
    out.writeF32(v.x);
    out.writeF32(v.y);
    out.writeF32(v.z);
}

Vector3 readVector3(NetworkStream& in)
{
    Vector3 v;
    v.x = in.readF32();
    v.y = in.readF32();
    v.z = in.readF32();
    return v;
}

std::string vecText(Vector3 v)
{
    return std::format("{}, {}, {}", v.x, v.y, v.z);
}

double asNumber(const Value& v)
{
    if (const double* d = std::get_if<double>(&v))
        return *d;
    if (const bool* b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    if (const EnumItem* e = std::get_if<EnumItem>(&v))
        return e->value;
    return 0.0;
}

bool isNumber(const Value& v)
{
    return std::holds_alternative<double>(v) || std::holds_alternative<EnumItem>(v);
}

// Matrix to quaternion (x, y, z, w), the textbook branch order the client
// matches (ROBLOX-LAYER.md 4.2, emu/model_quat.py).
std::array<float, 4> toQuaternion(const CFrame& cf)
{
    double m[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i][j] = static_cast<double>(cf.r[i][j]);
    const double tr = m[0][0] + m[1][1] + m[2][2];
    double x = 0, y = 0, z = 0, w = 1;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;
        x = (m[2][1] - m[1][2]) / s;
        y = (m[0][2] - m[2][0]) / s;
        z = (m[1][0] - m[0][1]) / s;
        w = s / 4.0;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
        x = s / 4.0;
        y = (m[0][1] + m[1][0]) / s;
        z = (m[0][2] + m[2][0]) / s;
        w = (m[2][1] - m[1][2]) / s;
    } else if (m[1][1] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
        x = (m[0][1] + m[1][0]) / s;
        y = s / 4.0;
        z = (m[1][2] + m[2][1]) / s;
        w = (m[0][2] - m[2][0]) / s;
    } else {
        const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
        x = (m[0][2] + m[2][0]) / s;
        y = (m[1][2] + m[2][1]) / s;
        z = s / 4.0;
        w = (m[1][0] - m[0][1]) / s;
    }
    const double len = std::sqrt(x * x + y * y + z * z + w * w);
    return {static_cast<float>(x / len), static_cast<float>(y / len), static_cast<float>(z / len),
            static_cast<float>(w / len)};
}

CFrame fromQuaternion(float x, float y, float z, float w)
{
    CFrame cf;
    cf.r[0][0] = 1 - 2 * (y * y + z * z);
    cf.r[0][1] = 2 * (x * y - z * w);
    cf.r[0][2] = 2 * (x * z + y * w);
    cf.r[1][0] = 2 * (x * y + z * w);
    cf.r[1][1] = 1 - 2 * (x * x + z * z);
    cf.r[1][2] = 2 * (y * z - x * w);
    cf.r[2][0] = 2 * (x * z - y * w);
    cf.r[2][1] = 2 * (y * z + x * w);
    cf.r[2][2] = 1 - 2 * (x * x + y * y);
    return cf;
}

std::string cframeText(const CFrame& cf)
{
    return std::format("pos ({}) rot [{} {} {} | {} {} {} | {} {} {}]", vecText(cf.position), cf.r[0][0], cf.r[0][1],
                       cf.r[0][2], cf.r[1][0], cf.r[1][1], cf.r[1][2], cf.r[2][0], cf.r[2][1], cf.r[2][2]);
}

} // namespace

const char* netTypeName(std::uint8_t type)
{
    switch (type) {
    case 0x00:
    case nettype::Nil:
        return "nil";
    case nettype::String:
        return "string";
    case nettype::Void:
        return "void";
    case nettype::ProtectedString:
        return "ProtectedString";
    case nettype::Enum:
        return "Enum";
    case nettype::SharedString:
        return "SharedString";
    case nettype::Bool:
        return "bool";
    case nettype::Int:
        return "int";
    case nettype::Float:
        return "float";
    case nettype::Double:
        return "double";
    case nettype::UDim:
        return "UDim";
    case nettype::UDim2:
        return "UDim2";
    case nettype::Ray:
        return "Ray";
    case nettype::Faces:
        return "Faces";
    case nettype::Axes:
        return "Axes";
    case nettype::BrickColor:
        return "BrickColor";
    case nettype::Color3:
        return "Color3";
    case nettype::Color3uint8:
        return "Color3uint8";
    case nettype::Vector2:
        return "Vector2";
    case nettype::Vector3:
        return "Vector3";
    case nettype::Vector2int16:
        return "Vector2int16";
    case nettype::Vector3int16:
        return "Vector3int16";
    case nettype::CFrame:
        return "CoordinateFrame";
    case nettype::CFrameCompressed:
        return "CoordinateFrame (compressed)";
    case nettype::Object:
        return "Object";
    case nettype::Tuple:
        return "Tuple";
    case nettype::Array:
        return "Array";
    case nettype::Dictionary:
        return "Dictionary";
    case nettype::Map:
        return "Map";
    case nettype::NumberSequence:
        return "NumberSequence";
    case nettype::NumberSequenceKeypoint:
        return "NumberSequenceKeypoint";
    case nettype::NumberRange:
        return "NumberRange";
    case nettype::ColorSequence:
        return "ColorSequence";
    case nettype::ColorSequenceKeypoint:
        return "ColorSequenceKeypoint";
    case nettype::Rect2D:
        return "Rect2D";
    case nettype::PhysicalProperties:
        return "PhysicalProperties";
    case nettype::Region3int16:
        return "Region3int16";
    case nettype::Int64:
        return "int64";
    case nettype::DateTime:
        return "DateTime";
    case nettype::OptionalCFrame:
    case nettype::OptionalCFrameCompressed:
        return "OptionalCoordinateFrame";
    case nettype::User:
        return "User";
    case nettype::AnimTrackMetadata:
        return "AnimTrackMetadata";
    case nettype::TypedNumber:
        return "TypedNumber";
    case nettype::AnimTrackWeight:
        return "AnimTrackWeight";
    case nettype::AnimTrackPlayState:
        return "AnimTrackPlayState";
    case nettype::Font:
        return "Font";
    case nettype::SecurityCapabilities:
        return "SecurityCapabilities";
    case nettype::Random:
        return "Random";
    case nettype::ReplicationPV:
        return "ReplicationPV";
    case nettype::ContentId:
        return "ContentId";
    case nettype::PeerAddress:
        return "SystemAddress";
    case nettype::BinaryString:
        return "BinaryString";
    case nettype::ProtectedStringBytecode:
        return "ProtectedString(bytecode)";
    case nettype::DictionaryString:
        return "string(dictionary)";
    case nettype::Buffer:
        return "buffer";
    case nettype::NetAssetHandle:
        return "NetAssetHandle";
    case nettype::NetAssetRef:
        return "NetAssetRef";
    case nettype::Content:
        return "Content";
    default:
        return "?";
    }
}

const char* luauTypeName(std::uint8_t type)
{
    switch (type) {
    case nettype::String:
    case nettype::ProtectedString:
    case nettype::SharedString:
    case nettype::ContentId:
    case nettype::BinaryString:
    case nettype::DictionaryString:
        return "string";
    case nettype::Bool:
        return "boolean";
    case nettype::Int:
    case nettype::Float:
    case nettype::Double:
    case nettype::Int64:
        return "number";
    case nettype::Enum:
        return "EnumItem";
    case nettype::Object:
        return "Instance";
    case nettype::Color3:
    case nettype::Color3uint8:
        return "Color3";
    case nettype::CFrame:
    case nettype::CFrameCompressed:
        return "CFrame";
    case nettype::Rect2D:
        return "Rect";
    case nettype::UDim:
    case nettype::UDim2:
    case nettype::Ray:
    case nettype::Faces:
    case nettype::Axes:
    case nettype::BrickColor:
    case nettype::Vector2:
    case nettype::Vector3:
    case nettype::Vector2int16:
    case nettype::Vector3int16:
    case nettype::NumberSequence:
    case nettype::NumberRange:
    case nettype::ColorSequence:
    case nettype::PhysicalProperties:
    case nettype::Region3int16:
    case nettype::DateTime:
    case nettype::Font:
        return netTypeName(type); // named as the type is
    case nettype::Array:
    case nettype::Dictionary:
    case nettype::Map:
        return "table";
    default:
        return nullptr;
    }
}

void writeInstanceId(NetworkStream& out, InstanceId id)
{
    if (id.null()) {
        out.writeVarint(0);
        return;
    }
    out.writeVarint(id.scope);
    out.writeU32(id.index);
}

InstanceId readInstanceId(NetworkStream& in)
{
    InstanceId id;
    id.scope = static_cast<std::uint32_t>(in.readVarint());
    if (id.scope != 0)
        id.index = in.readU32();
    return id;
}

std::optional<CFrame> rotationFromId(std::uint8_t id)
{
    static constexpr std::array<Vector3, 6> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}}};
    if (id < 0x02 || id > 0x23)
        return std::nullopt;
    const std::size_t right = (id - 1u) / 6, up = (id - 1u) % 6;
    if (right % 3 == up % 3)
        return std::nullopt;
    const std::array<Vector3, 3> columns{axes[right], axes[up], axes[right].cross(axes[up])};
    CFrame cf;
    for (std::size_t c = 0; c < 3; ++c) {
        cf.r[0][c] = columns[c].x;
        cf.r[1][c] = columns[c].y;
        cf.r[2][c] = columns[c].z;
    }
    return cf;
}

std::uint8_t rotationId(const CFrame& cf)
{
    for (std::uint8_t id = 0x02; id <= 0x23; ++id) {
        auto r = rotationFromId(id);
        if (!r)
            continue;
        bool same = true;
        for (int i = 0; i < 3 && same; ++i)
            for (int j = 0; j < 3 && same; ++j)
                same = r->r[i][j] == cf.r[i][j];
        if (same)
            return id;
    }
    return 0;
}

void writeQuaternion(NetworkStream& out, const CFrame& cf)
{
    std::array<float, 4> q = toQuaternion(cf);
    std::size_t i = std::abs(q[1]) > std::abs(q[0]) ? 1 : 0;
    i = std::abs(q[2]) > std::abs(q[i]) ? 2 : i;
    i = std::abs(q[3]) > std::abs(q[i]) ? 3 : i;
    if (q[i] < 0)
        for (float& c : q)
            c = -c;
    std::array<std::uint32_t, 3> packed{};
    std::size_t k = 0;
    for (std::size_t j = 0; j < 4; ++j) {
        if (j == i)
            continue;
        const float c = q[j];
        float t = c / 0.70710677f * 16383.0f;
        t += c >= 0 ? 0.5f : -0.5f;
        packed[k++] = static_cast<std::uint32_t>(static_cast<std::int32_t>(t)) & 0x7fff;
    }
    out.writeU16(static_cast<std::uint16_t>(packed[0]));
    out.writeU32(static_cast<std::uint32_t>(i) << 30 | packed[1] << 15 | packed[2]);
}

CFrame readQuaternion(NetworkStream& in)
{
    const std::uint16_t a = in.readU16();
    const std::uint32_t rest = in.readU32();
    const std::size_t largest = rest >> 30;
    auto unq = [](std::uint32_t v) {
        v &= 0x7fff;
        const int s = (v & 0x4000) ? static_cast<int>(v) - 0x8000 : static_cast<int>(v);
        return static_cast<float>(s) / 16383.0f * 0.70710677f;
    };
    const std::array<float, 3> others{unq(a), unq(rest >> 15), unq(rest)};
    std::array<float, 4> q{};
    float sum = 0;
    for (std::size_t j = 0, k = 0; j < 4; ++j) {
        if (j == largest)
            continue;
        q[j] = others[k++];
        sum += q[j] * q[j];
    }
    q[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
    return fromQuaternion(q[0], q[1], q[2], q[3]);
}

bool writeValue(NetworkStream& out, std::uint8_t type, const Value& value, const ValueContext& context)
{
    switch (type) {
    case nettype::String:
    case nettype::SharedString:
        if (const auto* s = std::get_if<std::string>(&value)) {
            out.writeString(*s);
            return true;
        }
        return false;
    case nettype::Bool:
        if (const bool* b = std::get_if<bool>(&value)) {
            out.writeU8(*b ? 1 : 0);
            return true;
        }
        return false;
    case nettype::Int:
        if (!isNumber(value))
            return false;
        out.writeZigzag32(static_cast<std::int32_t>(asNumber(value)));
        return true;
    case nettype::Int64:
        if (!isNumber(value))
            return false;
        out.writeZigzag64(static_cast<std::int64_t>(asNumber(value)));
        return true;
    case nettype::Float:
        if (!isNumber(value))
            return false;
        out.writeF32(static_cast<float>(asNumber(value)));
        return true;
    case nettype::Double:
        if (!isNumber(value))
            return false;
        out.writeF64(asNumber(value));
        return true;
    case nettype::Enum:
        if (!isNumber(value))
            return false;
        out.writeVarint(static_cast<std::uint64_t>(asNumber(value)));
        return true;
    case nettype::UDim:
        if (const auto* u = std::get_if<lunar::UDim>(&value)) {
            out.writeF32(u->scale);
            out.writeI32(u->offset);
            return true;
        }
        return false;
    case nettype::UDim2:
        if (const auto* u = std::get_if<lunar::UDim2>(&value)) {
            out.writeF32(u->x.scale);
            out.writeI32(u->x.offset);
            out.writeF32(u->y.scale);
            out.writeI32(u->y.offset);
            return true;
        }
        return false;
    case nettype::Faces:
        if (const auto* f = std::get_if<lunar::Faces>(&value)) {
            out.writeU32(f->bits());
            return true;
        }
        return false;
    case nettype::Axes:
        if (const auto* a = std::get_if<lunar::Axes>(&value)) {
            out.writeU32(a->bits());
            return true;
        }
        return false;
    case nettype::BrickColor:
        if (const auto* b = std::get_if<lunar::BrickColor>(&value)) {
            out.writeU16(b->number);
            return true;
        }
        return false;
    case nettype::Color3:
        if (const auto* c = std::get_if<lunar::Color3>(&value)) {
            out.writeF32(c->r);
            out.writeF32(c->g);
            out.writeF32(c->b);
            return true;
        }
        return false;
    case nettype::Color3uint8:
        if (const auto* c = std::get_if<lunar::Color3>(&value)) {
            auto byte
                = [](float f) { return static_cast<std::uint8_t>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
            out.writeU8(byte(c->r));
            out.writeU8(byte(c->g));
            out.writeU8(byte(c->b));
            return true;
        }
        return false;
    case nettype::Vector2:
        if (const auto* v = std::get_if<lunar::Vector2>(&value)) {
            out.writeF32(v->x);
            out.writeF32(v->y);
            return true;
        }
        return false;
    case nettype::Vector3:
        if (const auto* v = std::get_if<lunar::Vector3>(&value)) {
            writeVector3(out, *v);
            return true;
        }
        return false;
    case nettype::Vector3int16:
        if (const auto* v = std::get_if<lunar::Vector3int16>(&value)) {
            out.writeI16(v->x);
            out.writeI16(v->y);
            out.writeI16(v->z);
            return true;
        }
        return false;
    case nettype::CFrame:
    case nettype::CFrameCompressed:
        if (const auto* cf = std::get_if<lunar::CFrame>(&value)) {
            writeVector3(out, cf->position);
            const std::uint8_t id = rotationId(*cf);
            out.writeU8(id);
            if (id == 0) {
                if (type == nettype::CFrame) {
                    for (const auto& row : cf->r)
                        for (float cell : row)
                            out.writeF32(cell);
                } else {
                    writeQuaternion(out, *cf);
                }
            }
            return true;
        }
        return false;
    case nettype::Object:
        if (const auto* inst = std::get_if<InstancePtr>(&value)) {
            writeInstanceId(out, *inst && context.idOf ? context.idOf(*inst) : InstanceId{});
            return true;
        }
        if (std::holds_alternative<std::monostate>(value)) {
            writeInstanceId(out, {});
            return true;
        }
        return false;
    case nettype::OptionalCFrame:
        // 0x1057af548: u8 flag (0 = none), rotation, then position. GUESS
        // for the flag of an axis-aligned rotation: (id + 1) | 0x80.
        if (std::holds_alternative<std::monostate>(value)) {
            out.writeU8(0);
            return true;
        }
        if (const auto* cf = std::get_if<lunar::CFrame>(&value)) {
            const std::uint8_t id = rotationId(*cf);
            out.writeU8(static_cast<std::uint8_t>(id ? (id + 1) | 0x80 : 0x80));
            if (id == 0)
                for (const auto& row : cf->r)
                    for (float cell : row)
                        out.writeF32(cell);
            writeVector3(out, cf->position);
            return true;
        }
        return false;
    case nettype::NumberSequence:
        if (const auto* s = std::get_if<lunar::NumberSequence>(&value)) {
            out.writeU32(static_cast<std::uint32_t>(s->keypoints.size()));
            for (const auto& k : s->keypoints) {
                out.writeF32(k.time);
                out.writeF32(k.value);
                out.writeF32(k.envelope);
            }
            return true;
        }
        return false;
    case nettype::ColorSequence:
        if (const auto* s = std::get_if<lunar::ColorSequence>(&value)) {
            out.writeU32(static_cast<std::uint32_t>(s->keypoints.size()));
            for (const auto& k : s->keypoints) {
                out.writeF32(k.time);
                out.writeF32(k.value.r);
                out.writeF32(k.value.g);
                out.writeF32(k.value.b);
                out.writeF32(0.0f); // envelope, unused by ColorSequence
            }
            return true;
        }
        return false;
    case nettype::NumberRange:
        if (const auto* r = std::get_if<lunar::NumberRange>(&value)) {
            out.writeF32(r->min);
            out.writeF32(r->max);
            return true;
        }
        return false;
    case nettype::Rect2D:
        if (const auto* r = std::get_if<lunar::Rect>(&value)) {
            out.writeF32(r->min.x);
            out.writeF32(r->min.y);
            out.writeF32(r->max.x);
            out.writeF32(r->max.y);
            return true;
        }
        return false;
    case nettype::PhysicalProperties:
        if (std::holds_alternative<std::monostate>(value)) {
            out.writeU8(2); // not custom
            return true;
        }
        if (const auto* p = std::get_if<lunar::PhysicalProperties>(&value)) {
            out.writeU8(3);
            out.writeF32(p->density);
            out.writeF32(p->elasticity); // the client swaps these two on the wire
            out.writeF32(p->friction);
            out.writeF32(p->frictionWeight);
            out.writeF32(p->elasticityWeight);
            out.writeF32(p->acousticAbsorption);
            return true;
        }
        return false;
    case nettype::Font:
        if (const auto* f = std::get_if<lunar::Font>(&value)) {
            out.writeU16(static_cast<std::uint16_t>(f->weight));
            out.writeU8(static_cast<std::uint8_t>(f->style));
            out.writeString32(f->family);
            out.writeString32(""); // cached face id
            return true;
        }
        return false;
    // -- join-time types (JoinData.hpp, notes-joindata.md 4)
    case nettype::ContentId:
    case nettype::Content:
    case nettype::PeerAddress:
    case nettype::BinaryString:
    case nettype::ProtectedString:
    case nettype::ProtectedStringBytecode:
    case nettype::DictionaryString:
    case nettype::Buffer:
    case nettype::NetAssetHandle:
    case nettype::NetAssetRef:
        return writeJoinValue(out, type, value, context);
    default:
        return false;
    }
}

std::optional<Value> readValue(NetworkStream& in, std::uint8_t type, const ValueContext& context, std::string* text)
{
    std::string scratch;
    std::string& t = text ? *text : scratch;
    Value v;
    switch (type) {
    case 0x00:
    case nettype::Nil:
    case nettype::Void:
        if (text)
            t = "nil";
        break;
    case nettype::String:
    case nettype::SharedString: {
        std::string s = in.readString();
        if (text)
            t = std::format("\"{}\"", s.size() > 200 ? s.substr(0, 200) + "..." : s);
        v = std::move(s);
        break;
    }
    // -- join-time types (JoinData.hpp, notes-joindata.md 4)
    case nettype::ProtectedString:
    case nettype::ProtectedStringBytecode:
    case nettype::BinaryString:
    case nettype::ContentId:
    case nettype::Content:
    case nettype::PeerAddress:
    case nettype::DictionaryString:
    case nettype::Buffer:
    case nettype::NetAssetHandle:
    case nettype::NetAssetRef: {
        auto r = readJoinValue(in, type, context, text);
        if (!r)
            return std::nullopt;
        v = std::move(*r);
        break;
    }
    case nettype::Enum: {
        const auto n = static_cast<std::uint32_t>(in.readVarint());
        const EnumItem* item = context.enumType ? context.enumType->find(n) : nullptr;
        if (item) {
            v = *item;
            if (text)
                t = std::format("Enum.{}.{} ({})", context.enumType->name, item->name, n);
        } else {
            v = static_cast<double>(n);
            if (text)
                t = std::format("enum value {}", n);
        }
        break;
    }
    case nettype::Bool: {
        const bool b = in.readU8() != 0;
        v = b;
        if (text)
            t = b ? "true" : "false";
        break;
    }
    case nettype::Int: {
        const std::int32_t n = in.readZigzag32();
        v = static_cast<double>(n);
        if (text)
            t = std::to_string(n);
        break;
    }
    case nettype::Int64: {
        const std::int64_t n = in.readZigzag64();
        v = static_cast<double>(n);
        if (text)
            t = std::to_string(n);
        break;
    }
    case nettype::Float: {
        const float f = in.readF32();
        v = static_cast<double>(f);
        if (text)
            t = std::format("{}", f);
        break;
    }
    case nettype::Double: {
        const double d = in.readF64();
        v = d;
        if (text)
            t = std::format("{}", d);
        break;
    }
    case nettype::UDim: {
        lunar::UDim u;
        u.scale = in.readF32();
        u.offset = in.readI32();
        v = u;
        if (text)
            t = std::format("UDim({}, {})", u.scale, u.offset);
        break;
    }
    case nettype::UDim2: {
        lunar::UDim2 u;
        u.x.scale = in.readF32();
        u.x.offset = in.readI32();
        u.y.scale = in.readF32();
        u.y.offset = in.readI32();
        v = u;
        if (text)
            t = std::format("UDim2({}, {}, {}, {})", u.x.scale, u.x.offset, u.y.scale, u.y.offset);
        break;
    }
    case nettype::Ray: {
        const Vector3 o = readVector3(in), d = readVector3(in);
        v = o;
        if (text)
            t = std::format("Ray(origin {}, direction {})", vecText(o), vecText(d));
        break;
    }
    case nettype::Faces: {
        const std::uint32_t bits = in.readU32();
        v = lunar::Faces::fromBits(static_cast<std::uint8_t>(bits));
        if (text)
            t = std::format("Faces {:#x}", bits);
        break;
    }
    case nettype::Axes: {
        const std::uint32_t bits = in.readU32();
        v = lunar::Axes::fromBits(static_cast<std::uint8_t>(bits));
        if (text)
            t = std::format("Axes {:#x}", bits);
        break;
    }
    case nettype::BrickColor: {
        const std::uint16_t n = in.readU16();
        const lunar::BrickColor b = lunar::BrickColor::fromNumber(n);
        v = b;
        if (text)
            t = std::format("BrickColor {} ({})", n, b.name());
        break;
    }
    case nettype::Color3: {
        lunar::Color3 c;
        c.r = in.readF32();
        c.g = in.readF32();
        c.b = in.readF32();
        v = c;
        if (text)
            t = std::format("Color3({}, {}, {})", c.r, c.g, c.b);
        break;
    }
    case nettype::Color3uint8: {
        const std::uint8_t r = in.readU8(), g = in.readU8(), b = in.readU8();
        v = lunar::Color3::fromRGB(r, g, b);
        if (text)
            t = std::format("Color3.fromRGB({}, {}, {})", r, g, b);
        break;
    }
    case nettype::Vector2: {
        lunar::Vector2 p;
        p.x = in.readF32();
        p.y = in.readF32();
        v = p;
        if (text)
            t = std::format("Vector2({}, {})", p.x, p.y);
        break;
    }
    case nettype::Vector3: {
        const Vector3 p = readVector3(in);
        v = p;
        if (text)
            t = std::format("Vector3({})", vecText(p));
        break;
    }
    case nettype::Vector2int16: {
        const std::int16_t x = in.readI16(), y = in.readI16();
        v = lunar::Vector2{static_cast<float>(x), static_cast<float>(y)};
        if (text)
            t = std::format("Vector2int16({}, {})", x, y);
        break;
    }
    case nettype::Vector3int16: {
        lunar::Vector3int16 p;
        p.x = in.readI16();
        p.y = in.readI16();
        p.z = in.readI16();
        v = p;
        if (text)
            t = std::format("Vector3int16({}, {}, {})", p.x, p.y, p.z);
        break;
    }
    case nettype::Region3int16: {
        lunar::Vector3int16 a, b;
        a.x = in.readI16();
        a.y = in.readI16();
        a.z = in.readI16();
        b.x = in.readI16();
        b.y = in.readI16();
        b.z = in.readI16();
        v = a;
        if (text)
            t = std::format("Region3int16(({}, {}, {}), ({}, {}, {}))", a.x, a.y, a.z, b.x, b.y, b.z);
        break;
    }
    case nettype::CFrame:
    case nettype::CFrameCompressed: {
        const Vector3 pos = readVector3(in);
        const std::uint8_t id = in.readU8();
        lunar::CFrame cf;
        if (id == 0) {
            if (type == nettype::CFrame) {
                for (auto& row : cf.r)
                    for (float& cell : row)
                        cell = in.readF32();
            } else {
                cf = readQuaternion(in);
            }
        } else if (auto r = rotationFromId(id)) {
            cf = *r;
        }
        cf.position = pos;
        v = cf;
        if (text)
            t = id ? std::format("CFrame {} (rotation id {:#04x})", cframeText(cf), id) : "CFrame " + cframeText(cf);
        break;
    }
    case nettype::OptionalCFrame:
    case nettype::OptionalCFrameCompressed: {
        const std::uint8_t flag = in.readU8();
        if (flag == 0) {
            if (text)
                t = "nil";
            break;
        }
        lunar::CFrame cf;
        const std::uint8_t id = flag & 0x7f;
        if (id == 0) {
            if (type == nettype::OptionalCFrame) {
                for (auto& row : cf.r)
                    for (float& cell : row)
                        cell = in.readF32();
            } else {
                cf = readQuaternion(in);
            }
        } else if (auto r = rotationFromId(static_cast<std::uint8_t>(id - 1))) {
            cf = *r;
        }
        cf.position = readVector3(in);
        v = cf;
        if (text)
            t = "CFrame " + cframeText(cf);
        break;
    }
    case nettype::TypedNumber:
    case nettype::AnimTrackPlayState: {
        // GUESS meaning of the fields; the engine keeps no value for them.
        const std::uint8_t kind = in.readU8();
        const std::uint32_t word = in.readU32();
        const double number = in.readF64();
        if (text)
            t = std::format("{}({}, {:#x}, {})", type == nettype::TypedNumber ? "TypedNumber" : "AnimTrackPlayState",
                            kind, word, number);
        break;
    }
    case nettype::AnimTrackWeight: {
        // GUESS meaning of the fields; the engine keeps no value for them.
        const float a = in.readF32();
        const float b = in.readF32();
        const double c = in.readF64();
        if (text)
            t = std::format("AnimTrackWeight({}, {}, {})", a, b, c);
        break;
    }
    case nettype::AnimTrackMetadata: {
        // GUESS meaning; the engine keeps no value for it.
        const std::uint8_t a = in.readU8();
        const std::uint16_t b = in.readU16();
        if (text)
            t = std::format("AnimTrackMetadata({}, {})", a, b);
        break;
    }
    case nettype::User: {
        // GUESS meaning: the user id first; the engine keeps no value for it.
        const std::int64_t userId = in.readZigzag64();
        const std::uint64_t second = in.readVarint();
        const std::int64_t third = in.readZigzag64();
        const std::int64_t fourth = in.readZigzag64();
        if (text)
            t = std::format("User({}, {}, {}, {})", userId, second, third, fourth);
        break;
    }
    case nettype::Object: {
        const InstanceId id = readInstanceId(in);
        InstancePtr inst = !id.null() && context.resolve ? context.resolve(id) : nullptr;
        if (inst) {
            if (text)
                t = std::format("{} ({}:{})", inst->fullName(), id.scope, id.index);
            v = inst;
        } else {
            if (text)
                t = id.null() ? "nil" : std::format("instance {}:{}", id.scope, id.index);
            if (!id.null() && context.sawRef)
                context.sawRef(id);
        }
        break;
    }
    case nettype::NumberSequence: {
        const std::uint32_t n = in.readU32();
        std::vector<NumberSequenceKeypoint> points;
        if (text)
            t = "NumberSequence(";
        for (std::uint32_t i = 0; i < n && !in.bad() && i < 1024; ++i) {
            NumberSequenceKeypoint k;
            k.time = in.readF32();
            k.value = in.readF32();
            k.envelope = in.readF32();
            if (text)
                t += std::format("{}{} {} {}", i ? ", " : "", k.time, k.value, k.envelope);
            points.push_back(k);
        }
        if (text)
            t += ')';
        v = lunar::NumberSequence(std::move(points));
        break;
    }
    case nettype::ColorSequence: {
        const std::uint32_t n = in.readU32();
        std::vector<ColorSequenceKeypoint> points;
        if (text)
            t = "ColorSequence(";
        for (std::uint32_t i = 0; i < n && !in.bad() && i < 1024; ++i) {
            ColorSequenceKeypoint k;
            k.time = in.readF32();
            k.value.r = in.readF32();
            k.value.g = in.readF32();
            k.value.b = in.readF32();
            in.readF32(); // envelope
            if (text)
                t += std::format("{}{} ({} {} {})", i ? ", " : "", k.time, k.value.r, k.value.g, k.value.b);
            points.push_back(k);
        }
        if (text)
            t += ')';
        v = lunar::ColorSequence(std::move(points));
        break;
    }
    case nettype::NumberRange: {
        lunar::NumberRange r;
        r.min = in.readF32();
        r.max = in.readF32();
        v = r;
        if (text)
            t = std::format("NumberRange({}, {})", r.min, r.max);
        break;
    }
    case nettype::Rect2D: {
        lunar::Rect r;
        r.min.x = in.readF32();
        r.min.y = in.readF32();
        r.max.x = in.readF32();
        r.max.y = in.readF32();
        v = r;
        if (text)
            t = std::format("Rect({}, {}, {}, {})", r.min.x, r.min.y, r.max.x, r.max.y);
        break;
    }
    case nettype::PhysicalProperties: {
        const std::uint8_t flag = in.readU8();
        if (flag >= 3) {
            lunar::PhysicalProperties p;
            p.density = in.readF32();
            p.elasticity = in.readF32();
            p.friction = in.readF32();
            p.frictionWeight = in.readF32();
            p.elasticityWeight = in.readF32();
            p.acousticAbsorption = in.readF32();
            v = p;
            if (text)
                t = std::format("PhysicalProperties(density {}, friction {}, elasticity {}, {}, {}, {})", p.density,
                                p.friction, p.elasticity, p.frictionWeight, p.elasticityWeight, p.acousticAbsorption);
        } else {
            if (text)
                t = "PhysicalProperties(default)";
        }
        break;
    }
    case nettype::DateTime: {
        const auto ms = static_cast<std::int64_t>(in.readU64());
        v = static_cast<double>(ms);
        if (text)
            t = std::format("DateTime {} ms", ms);
        break;
    }
    case nettype::SecurityCapabilities:
    case nettype::Random: {
        const std::uint64_t n = in.readU64();
        v = static_cast<double>(n);
        if (text)
            t = std::format("{} {:#x}", netTypeName(type), n);
        break;
    }
    case nettype::Font: {
        lunar::Font f;
        f.weight = static_cast<Enum::FontWeight>(in.readU16());
        f.style = static_cast<Enum::FontStyle>(in.readU8());
        f.family = in.readString32();
        const std::string face = in.readString32();
        if (text)
            t = std::format("Font(\"{}\", weight {}, style {}{})", f.family, static_cast<int>(f.weight),
                            static_cast<int>(f.style), face.empty() ? "" : ", face " + face);
        v = std::move(f);
        break;
    }
    case nettype::Tuple:
    case nettype::Array: {
        const std::uint64_t n = in.readVarint();
        if (text)
            t = std::format("{}[{}](", netTypeName(type), n);
        for (std::uint64_t i = 0; i < n && !in.bad() && i < 4096; ++i) {
            std::string item;
            readVariant(in, context, text ? &item : nullptr);
            if (text)
                t += (i ? ", " : "") + item;
        }
        if (text)
            t += ')';
        break;
    }
    case nettype::Dictionary:
    case nettype::Map: {
        const std::uint64_t n = in.readVarint();
        if (text)
            t = std::format("{}[{}](", netTypeName(type), n);
        for (std::uint64_t i = 0; i < n && !in.bad() && i < 4096; ++i) {
            const std::string key = in.readString();
            std::string item;
            readVariant(in, context, text ? &item : nullptr);
            if (text)
                t += std::format("{}{} = {}", i ? ", " : "", key, item);
        }
        if (text)
            t += ')';
        break;
    }
    default:
        if (text)
            t = std::format("(type {:#04x} not decoded)", type);
        return std::nullopt;
    }
    if (in.bad())
        return std::nullopt;
    return v;
}

std::uint8_t variantType(const Value& value)
{
    switch (value.index()) {
    case 0:
        return 0;
    case 1:
        return nettype::Bool;
    case 2:
        return nettype::Double;
    case 3:
        return nettype::String;
    case 4:
        return nettype::Vector3;
    case 5:
        return nettype::Color3;
    case 6:
        return nettype::CFrame;
    case 7:
        return nettype::Object;
    case 8:
        return nettype::Vector2;
    case 9:
        return nettype::UDim;
    case 10:
        return nettype::UDim2;
    case 11:
        return nettype::Rect2D;
    case 12:
        return nettype::NumberRange;
    case 13:
        return nettype::NumberSequence;
    case 14:
        return nettype::ColorSequence;
    case 15:
        return nettype::BrickColor;
    case 16:
        return nettype::Enum;
    case 17:
        return nettype::Faces;
    case 18:
        return nettype::Axes;
    case 19:
        return nettype::PhysicalProperties;
    case 20:
        return nettype::Vector3int16;
    case 21:
        return nettype::Font;
    case 22:
        return nettype::Buffer;
    default:
        return 0;
    }
}

bool writeVariant(NetworkStream& out, const Value& value, const ValueContext& context)
{
    const std::uint8_t type = variantType(value);
    out.writeU8(type);
    if (type == 0)
        return true;
    if (type == nettype::Enum) {
        const EnumInfo* e = std::get<EnumItem>(value).enumType;
        out.writeU16(e && context.enumIdOf ? context.enumIdOf(*e).value_or(0) : 0);
    }
    return writeValue(out, type, value, context);
}

bool writeRemoteTable(NetworkStream& out, std::uint8_t type, const RemoteValue& value, const ValueContext& context)
{
    if (!value.table || !isTableType(type))
        return false;
    const RemoteTable& t = *value.table;
    const bool keyed = type != nettype::Array;
    out.writeVarint(t.members.size());
    bool ok = true;
    for (std::size_t i = 0; i < t.members.size(); ++i) {
        const auto& [key, member] = t.members[i];
        if (keyed)
            out.writeString(t.dictionary ? key : std::to_string(i + 1));
        ok = writeVariant(out, member, context) && ok;
    }
    return ok;
}

bool writeVariant(NetworkStream& out, const RemoteValue& value, const ValueContext& context)
{
    if (value.random) {
        out.writeU8(nettype::Random);
        out.writeU64(*value.random);
        return true;
    }
    if (!value.table)
        return writeVariant(out, value.value, context);
    const RemoteTable& t = *value.table;
    out.writeU8(t.dictionary ? nettype::Dictionary : nettype::Array);
    out.writeVarint(t.members.size());
    bool ok = true;
    for (const auto& [key, member] : t.members) {
        if (t.dictionary)
            out.writeString(key);
        ok = writeVariant(out, member, context) && ok;
    }
    return ok;
}

namespace {

// Roblox refuses deeper tables ("Tables are too deeply nested"); a reader stops past it.
constexpr int kMaxTableDepth = 64;

std::optional<RemoteValue> readVariantAt(NetworkStream& in, const ValueContext& context, std::string* text, int depth);

// A table's count and members, its type already known.
std::optional<RemoteValue> readTableAt(NetworkStream& in, std::uint8_t type, const ValueContext& context,
                                       std::string* text, int depth)
{
    if (depth >= kMaxTableDepth)
        return std::nullopt;
    auto table = std::make_shared<RemoteTable>();
    table->dictionary = type != nettype::Array;
    const std::uint64_t n = in.readVarint();
    std::string inner = text ? std::format("{}[{}](", netTypeName(type), n) : std::string();
    for (std::uint64_t i = 0; i < n && !in.bad(); ++i) {
        std::string key = table->dictionary ? in.readString() : std::string();
        std::string item;
        auto member = readVariantAt(in, context, text ? &item : nullptr, depth + 1);
        if (!member)
            return std::nullopt;
        if (text)
            inner += std::format("{}{}{}", i ? ", " : "", table->dictionary ? key + " = " : "", item);
        table->members.emplace_back(std::move(key), std::move(*member));
    }
    if (in.bad())
        return std::nullopt;
    if (text)
        *text = std::format("{}: {})", netTypeName(type), inner);
    return RemoteValue{Value{}, std::move(table)};
}

std::optional<RemoteValue> readVariantAt(NetworkStream& in, const ValueContext& context, std::string* text, int depth)
{
    const std::uint8_t type = in.readU8();
    if (type == 0) {
        if (text)
            *text = "nil";
        return RemoteValue{};
    }
    if (type == nettype::Enum) {
        const std::uint16_t enumId = in.readU16();
        ValueContext c = context;
        c.enumType = context.enumById ? context.enumById(enumId) : nullptr;
        auto v = readValue(in, type, c, text);
        if (text)
            *text += std::format(" (enum type {})", enumId);
        return v ? std::optional(remoteValue(std::move(*v))) : std::nullopt;
    }
    if (isTableType(type))
        return readTableAt(in, type, context, text, depth);
    if (type == nettype::Random) {
        RemoteValue random;
        random.random = in.readU64();
        if (text)
            *text = std::format("Random: {:#x}", *random.random);
        return in.bad() ? std::nullopt : std::optional(std::move(random));
    }
    std::string inner;
    auto v = readValue(in, type, context, text ? &inner : nullptr);
    if (text)
        *text = std::format("{}: {}", netTypeName(type), inner);
    return v ? std::optional(remoteValue(std::move(*v))) : std::nullopt;
}

} // namespace

std::optional<RemoteValue> readRemoteVariant(NetworkStream& in, const ValueContext& context, std::string* text)
{
    return readVariantAt(in, context, text, 0);
}

std::optional<RemoteValue> readRemoteTable(NetworkStream& in, std::uint8_t type, const ValueContext& context,
                                           std::string* text)
{
    if (!isTableType(type))
        return std::nullopt;
    return readTableAt(in, type, context, text, 0);
}

std::optional<Value> readVariant(NetworkStream& in, const ValueContext& context, std::string* text)
{
    auto v = readVariantAt(in, context, text, 0);
    return v ? std::optional(std::move(v->value)) : std::nullopt;
}

} // namespace lunar::net

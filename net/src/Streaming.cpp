#include <lunar/net/Streaming.hpp>

#include <lunar/net/JoinData.hpp> // writeCompressedBlock / readCompressedBlock

#include <zstd.h>

#include <algorithm>
#include <bit>
#include <format>

namespace lunar::net {

const char* itemTypeName(std::uint8_t type); // Replication.cpp

// -- regions

void writeRegionId(NetworkStream& out, RegionId id)
{
    const auto fits = [](std::int32_t v) { return v >= -128 && v <= 127; };
    if (fits(id.x) && fits(id.y) && fits(id.z)) {
        out.writeU8(0);
        out.writeU8(static_cast<std::uint8_t>(static_cast<std::int8_t>(id.x)));
        out.writeU8(static_cast<std::uint8_t>(static_cast<std::int8_t>(id.y)));
        out.writeU8(static_cast<std::uint8_t>(static_cast<std::int8_t>(id.z)));
        return;
    }
    out.writeU8(1);
    out.writeI32(id.x);
    out.writeI32(id.y);
    out.writeI32(id.z);
}

RegionId readRegionId(NetworkStream& in)
{
    RegionId id;
    if (in.readU8() == 0) {
        id.x = static_cast<std::int8_t>(in.readU8());
        id.y = static_cast<std::int8_t>(in.readU8());
        id.z = static_cast<std::int8_t>(in.readU8());
    } else {
        id.x = in.readI32();
        id.y = in.readI32();
        id.z = in.readI32();
    }
    return id;
}

std::string regionText(RegionId id)
{
    return std::format("[{}, {}, {}]", id.x, id.y, id.z);
}

// -- zstd blocks

std::vector<std::uint8_t> compressBlock(std::span<const std::uint8_t> raw)
{
    std::vector<std::uint8_t> packed(ZSTD_compressBound(raw.size()));
    const std::size_t n = ZSTD_compress(packed.data(), packed.size(), raw.data(), raw.size(), 3);
    packed.resize(ZSTD_isError(n) ? 0 : n);
    return packed;
}

// -- helpers

namespace {

std::string idText(InstanceId id)
{
    return id.null() ? "null" : std::format("{}:{}", id.scope, id.index);
}

// Pings: the flags that decide the optional fields.
constexpr std::uint8_t kPingServerBody = 0x06;
constexpr std::uint8_t kPingXorred = 0x04;
constexpr std::uint8_t kPingTracer = 0x08;
constexpr std::uint8_t kPingMemory = 0x10;
constexpr std::uint8_t kPingBackFlag = 0x01;

void writeStreamData(NetworkStream& out, const StreamDataItem& d)
{
    out.writeU32(d.count);
    if (d.count != 0)
        writeCompressedBlock(out, d.blob);
    for (std::size_t i = 0; i < d.regions.size(); ++i) {
        const StreamRegionRef& r = d.regions[i];
        out.writeU8(static_cast<std::uint8_t>((r.code & 0xf7) | (i + 1 < d.regions.size() ? 0x08 : 0)));
        if ((r.code & 0xf7) == 0)
            writeRegionId(out, r.region);
    }
}

bool readStreamData(NetworkStream& in, StreamDataItem& d, std::string& error)
{
    d.count = in.readU32();
    if (d.count != 0) {
        auto blob = readCompressedBlock(in);
        if (!blob) {
            error = "stream data: bad compressed block";
            return false;
        }
        d.blob = std::move(*blob);
    }
    for (int guard = 0; guard < 1 << 16; ++guard) {
        const std::uint8_t f = in.readU8();
        if (in.bad())
            break;
        StreamRegionRef r;
        r.code = f & 0xf7;
        if (r.code == 0)
            r.region = readRegionId(in);
        d.regions.push_back(r);
        if (!(f & 0x08))
            break;
    }
    return !in.bad();
}

void describeStreamData(const StreamDataItem& d, Node& n)
{
    n.add(std::format("instances: {} ({} decompressed join-data bytes, layout: Join Data item 0x0b)", d.count,
                      d.blob.size()));
    if (!d.blob.empty())
        addHex(n.add("join data body"), d.blob, 0, 128);
    Node& regs = n.add(std::format("regions ({})", d.regions.size()));
    for (const StreamRegionRef& r : d.regions) {
        if (r.code == 0)
            regs.add(regionText(r.region));
        else
            regs.add(std::format("delta code {} ({})", r.code,
                                 r.code == 1   ? "previous, y + 1"
                                 : r.code == 2 ? "previous, z + 1"
                                 : r.code == 3 ? "previous, x + 1"
                                 : r.code == 4 ? "sentinel"
                                               : "?"));
    }
}

template <class T>
RuntimeItem wrap(T&& body)
{
    return RuntimeItem{RuntimeBody(std::forward<T>(body))};
}

} // namespace

std::uint8_t runtimeItemType(const RuntimeItem& item)
{
    return std::visit(
        [](const auto& b) -> std::uint8_t {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, PingItem>)
                return item::Ping;
            else if constexpr (std::is_same_v<T, PingBackItem>)
                return item::PingBack;
            else if constexpr (std::is_same_v<T, RequestCharacterItem>)
                return item::RequestCharacter;
            else if constexpr (std::is_same_v<T, PmcConfigItem>)
                return item::PmcConfig;
            else if constexpr (std::is_same_v<T, CFrameAckItem>)
                return item::CFrameAck;
            else if constexpr (std::is_same_v<T, StreamDataItem>)
                return item::StreamData;
            else if constexpr (std::is_same_v<T, RegionRemovalItem>)
                return item::RegionRemoval;
            else if constexpr (std::is_same_v<T, InstanceRemovalItem>)
                return item::InstanceRemoval;
            else if constexpr (std::is_same_v<T, StatsItem>)
                return item::Stats;
            else if constexpr (std::is_same_v<T, StreamingInfoItem>)
                return item::StreamingInfo;
            else if constexpr (std::is_same_v<T, StreamPrefetchInfoItem>)
                return item::StreamPrefetchInfo;
            else if constexpr (std::is_same_v<T, StreamPrefetchRequestItem>)
                return item::StreamPrefetchRequest;
            else if constexpr (std::is_same_v<T, StreamFocusItem>)
                return item::StreamFocus;
            else if constexpr (std::is_same_v<T, StreamFocusRemoveItem>)
                return item::StreamFocusRemove;
            else if constexpr (std::is_same_v<T, StreamFocusFrustumItem>)
                return item::StreamFocusFrustum;
            else if constexpr (std::is_same_v<T, StreamFocusFrustumRemoveItem>)
                return item::StreamFocusFrustumRemove;
            else if constexpr (std::is_same_v<T, DestroyItem>)
                return item::Destroy;
            else if constexpr (std::is_same_v<T, ClientCapacityUpdateItem>)
                return item::ClientCapacityUpdate;
            else if constexpr (std::is_same_v<T, PersistentPlayerItem>)
                return b.add ? item::PersistentPlayerAdd : item::PersistentPlayerRemove;
            else if constexpr (std::is_same_v<T, ClientQoSItem>)
                return item::ClientQoS;
            else if constexpr (std::is_same_v<T, IsrTimestampItem>)
                return item::IsrTimestamp;
            else if constexpr (std::is_same_v<T, StreamPinDataItem>)
                return item::StreamPinData;
            else if constexpr (std::is_same_v<T, InstanceIntentItem>)
                return item::InstanceIntent;
            else
                return item::RegionTerrainLodRemoval;
        },
        item.body);
}

bool isRuntimeItemType(std::uint8_t type)
{
    switch (type) {
    case item::Ping:
    case item::PingBack:
    case item::RequestCharacter:
    case item::PmcConfig:
    case item::CFrameAck:
    case item::StreamData:
    case item::RegionRemoval:
    case item::InstanceRemoval:
    case item::Stats:
    case item::StreamingInfo:
    case item::StreamPrefetchInfo:
    case item::StreamPrefetchRequest:
    case item::StreamFocus:
    case item::StreamFocusRemove:
    case item::StreamFocusFrustum:
    case item::StreamFocusFrustumRemove:
    case item::Destroy:
    case item::ClientCapacityUpdate:
    case item::PersistentPlayerAdd:
    case item::PersistentPlayerRemove:
    case item::ClientQoS:
    case item::IsrTimestamp:
    case item::StreamPinData:
    case item::InstanceIntent:
    case item::RegionTerrainLodRemoval:
        return true;
    default:
        return false;
    }
}

PingItem makePing(std::uint64_t nowMs, bool fromServer, std::uint8_t kind)
{
    PingItem p;
    p.flags = fromServer ? kPingServerBody : 0;
    p.time = nowMs;
    p.sendMs = static_cast<std::uint32_t>(nowMs);
    p.kind = kind;
    return p;
}

PingBackItem answerPing(const PingItem& ping, std::uint32_t recvMs, std::uint32_t nowMs)
{
    PingBackItem b;
    b.flags = kPingBackFlag;
    b.time = ping.time;
    b.pingSendMs = ping.sendMs;
    b.recvMs = recvMs;
    b.nowMs = nowMs;
    b.kind = ping.kind;
    return b;
}

std::uint32_t pingRoundTripMs(const PingBackItem& back, std::uint32_t nowMs)
{
    return nowMs - static_cast<std::uint32_t>(back.time);
}

bool writeRuntimeItem(NetworkStream& out, const RuntimeItem& item, const ValueContext& /*values*/,
                      const ItemContext& context)
{
    const std::uint64_t caps = context.capabilities;
    return std::visit(
        [&](const auto& b) -> bool {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, PingItem>) {
                if (b.flags & kPingTracer)
                    return false; // the tracer payload (0x104b995df) isn't decoded
                out.writeU8(b.flags);
                out.writeU64(b.time);
                out.writeU32(b.sendMs);
                // Only the ServerReplicator reads them (0x1055d9f21 checks the reader's class):
                // they travel in client pings.
                if (!context.fromServer)
                    for (float f : b.fps)
                        out.writeF32(f);
                out.writeU32(b.mask);
                out.writeU32((b.time & 0x20) ? ~b.extra : b.extra);
                out.writeU8(b.kind);
                if (b.flags & kPingXorred)
                    out.writeU32(b.xorred ^ static_cast<std::uint32_t>(b.time));
                if ((b.flags & kPingMemory) && (caps & caps::PingMemory))
                    out.writeF32(b.memoryMb);
                return true;
            } else if constexpr (std::is_same_v<T, PingBackItem>) {
                if (b.flags & kPingTracer)
                    return false;
                out.writeU8(static_cast<std::uint8_t>(b.flags | kPingBackFlag));
                out.writeU64(b.time);
                out.writeU32(b.pingSendMs);
                out.writeU32(b.recvMs);
                out.writeU32(b.zero);
                out.writeU32(b.nowMs);
                out.writeU32(b.counter);
                out.writeU32(b.lastPacketBytes);
                out.writeU32(b.lastPacketHigh);
                out.writeU32(b.mask);
                out.writeU32((b.time & 0x20) ? ~b.extra : b.extra);
                out.writeU8(b.kind);
                return true;
            } else if constexpr (std::is_same_v<T, RequestCharacterItem>) {
                out.writeU32(b.playerFlags);
                out.writeString32(b.appearance);
                writeInstanceId(out, b.player);
                return true;
            } else if constexpr (std::is_same_v<T, PmcConfigItem>) {
                out.writeU8(b.value);
                return true;
            } else if constexpr (std::is_same_v<T, CFrameAckItem>) {
                writeInstanceId(out, b.instance);
                out.writeU16(b.propertyId);
                out.writeU8(b.hasVersion ? 1 : 0);
                if (b.hasVersion)
                    out.writeZigzag32(b.version);
                if (caps & caps::CFrameAckStep)
                    out.writeU32(b.step);
                return true;
            } else if constexpr (std::is_same_v<T, StreamDataItem>) {
                if (b.regions.empty())
                    return false; // the list is never empty on the wire
                writeStreamData(out, b);
                return true;
            } else if constexpr (std::is_same_v<T, RegionRemovalItem>) {
                writeRegionId(out, b.region);
                out.writeU32(b.sequence);
                out.writeU32(static_cast<std::uint32_t>(b.removed.size()));
                const bool extra = (caps & caps::RegionRemovalC) && b.extra != 0;
                const bool packed = b.removed.size() > kRegionRemovalInlineRefs;
                out.writeU8(static_cast<std::uint8_t>((b.flags & ~0x05) | (packed ? 0x01 : 0) | (extra ? 0x04 : 0)));
                if (extra)
                    out.writeU32(b.extra);
                // The refs, inline or as one compressed block (0x105612fe4) [code: 0x1056d57ee].
                NetworkStream refs;
                for (InstanceId id : b.removed)
                    writeInstanceId(packed ? refs : out, id);
                if (packed)
                    writeCompressedBlock(out, refs.bytes());
                return true;
            } else if constexpr (std::is_same_v<T, InstanceRemovalItem>) {
                writeInstanceId(out, b.instance);
                return true;
            } else if constexpr (std::is_same_v<T, StatsItem>) {
                return false; // read only: the client never writes one
            } else if constexpr (std::is_same_v<T, StreamingInfoItem>) {
                writeRegionId(out, b.region);
                out.writeI32(b.radius);
                out.writeU32(b.version);
                if (caps & caps::StreamInfoExtra)
                    out.writeI32(b.extra);
                return true;
            } else if constexpr (std::is_same_v<T, StreamPrefetchInfoItem>) {
                writeRegionId(out, b.region);
                out.writeI32(b.a);
                out.writeI32(b.b);
                return true;
            } else if constexpr (std::is_same_v<T, StreamPrefetchRequestItem>) {
                out.writeF32(b.position.x);
                out.writeF32(b.position.y);
                out.writeF32(b.position.z);
                out.writeU64(std::bit_cast<std::uint64_t>(b.timeout));
                out.writeI32(b.id);
                return true;
            } else if constexpr (std::is_same_v<T, StreamFocusItem>) {
                out.writeU32(b.focusId);
                writeRegionId(out, b.region);
                out.writeI32(b.completedRadius);
                out.writeF32(b.radius.value_or(-1.0f));
                out.writeU8(b.flagA);
                out.writeU8(b.flagB);
                if (caps & caps::FocusExtra)
                    out.writeU32(b.extra);
                return true;
            } else if constexpr (std::is_same_v<T, StreamFocusRemoveItem>) {
                out.writeU32(b.focusId);
                return true;
            } else if constexpr (std::is_same_v<T, StreamFocusFrustumItem>) {
                out.writeU32(b.focusId);
                for (float f : b.frustum)
                    out.writeF32(f);
                return true;
            } else if constexpr (std::is_same_v<T, StreamFocusFrustumRemoveItem>) {
                out.writeU32(b.focusId);
                return true;
            } else if constexpr (std::is_same_v<T, DestroyItem>) {
                out.writeU32(static_cast<std::uint32_t>(b.instances.size()));
                for (InstanceId id : b.instances)
                    writeInstanceId(out, id);
                return true;
            } else if constexpr (std::is_same_v<T, ClientCapacityUpdateItem>) {
                out.writeU8(b.memoryLimited);
                out.writeI32(b.quotaDelta);
                out.writeU16(static_cast<std::uint16_t>(b.maxRegionRadius));
                if (caps & caps::CapacityUpdateExtra) {
                    out.writeU8(b.bits);
                    out.writeU32(b.extra);
                }
                return true;
            } else if constexpr (std::is_same_v<T, PersistentPlayerItem>) {
                writeInstanceId(out, b.player);
                return true;
            } else if constexpr (std::is_same_v<T, ClientQoSItem>) {
                out.writeU8(b.kind);
                out.writeU8(b.flag);
                out.writeString32(b.payload);
                return true;
            } else if constexpr (std::is_same_v<T, IsrTimestampItem>) {
                out.writeI32(b.timestamp);
                return true;
            } else if constexpr (std::is_same_v<T, StreamPinDataItem>) {
                if (b.data.regions.empty())
                    return false;
                writeStreamData(out, b.data);
                writeInstanceId(out, b.instance);
                out.writeI32(b.value);
                out.writeU8(b.flag);
                return true;
            } else if constexpr (std::is_same_v<T, InstanceIntentItem>) {
                out.writeU32(static_cast<std::uint32_t>(b.intents.size()));
                for (const auto& [id, intent] : b.intents) {
                    writeInstanceId(out, id);
                    out.writeU8(intent);
                }
                return true;
            } else {
                writeRegionId(out, b.region);
                out.writeU8(b.lod);
                return true;
            }
        },
        item.body);
}

std::optional<RuntimeItem> readRuntimeItem(NetworkStream& in, std::uint8_t type, const ValueContext& /*values*/,
                                           const ItemContext& context, Node* tree, std::string& error)
{
    if (!isRuntimeItemType(type))
        return std::nullopt;
    const std::uint64_t caps = context.capabilities;
    auto finish = [&](auto body) -> std::optional<RuntimeItem> {
        if (in.bad()) {
            error = std::format("truncated {} item", itemTypeName(type));
            return std::nullopt;
        }
        RuntimeItem r = wrap(std::move(body));
        if (tree)
            describeRuntimeItem(r, *tree);
        return r;
    };
    switch (type) {
    case item::Ping: {
        PingItem p;
        p.flags = in.readU8();
        p.time = in.readU64();
        p.sendMs = in.readU32();
        if (p.flags & kPingBackFlag) {
            error = "ping item with the PingBack flag";
            return std::nullopt;
        }
        // The fps triple is read by the ServerReplicator only (0x1055d9f21 checks the
        // reader's class), so it travels in client -> server pings [code].
        if (!context.fromServer)
            for (float& f : p.fps)
                f = in.readF32();
        p.mask = in.readU32();
        p.extra = in.readU32();
        if (p.time & 0x20)
            p.extra = ~p.extra;
        p.kind = in.readU8();
        if (p.kind > 1) {
            error = std::format("ping kind {} (the client drops kinds above 1)", p.kind);
            return std::nullopt;
        }
        if (p.flags & kPingXorred)
            p.xorred = in.readU32() ^ static_cast<std::uint32_t>(p.time);
        if ((p.flags & kPingMemory) && (caps & caps::PingMemory))
            p.memoryMb = in.readF32();
        if (p.flags & kPingTracer) {
            error = "ping tracer payload (flag 8) not decoded";
            return std::nullopt;
        }
        return finish(p);
    }
    case item::PingBack: {
        PingBackItem b;
        b.flags = in.readU8();
        b.time = in.readU64();
        b.pingSendMs = in.readU32();
        if (!(b.flags & kPingBackFlag)) {
            error = "PingBack item without the PingBack flag";
            return std::nullopt;
        }
        b.recvMs = in.readU32();
        b.zero = in.readU32();
        b.nowMs = in.readU32();
        b.counter = in.readU32();
        b.lastPacketBytes = in.readU32();
        b.lastPacketHigh = in.readU32();
        b.mask = in.readU32();
        b.extra = in.readU32();
        if (b.time & 0x20)
            b.extra = ~b.extra;
        b.kind = in.readU8();
        if (b.kind > 1) {
            error = std::format("PingBack kind {}", b.kind);
            return std::nullopt;
        }
        if (b.flags & kPingTracer) {
            error = "PingBack tracer payload (flag 8) not decoded";
            return std::nullopt;
        }
        return finish(b);
    }
    case item::RequestCharacter: {
        RequestCharacterItem r;
        r.playerFlags = in.readU32();
        r.appearance = in.readString32(1u << 20);
        r.player = readInstanceId(in);
        return finish(r);
    }
    case item::PmcConfig: {
        PmcConfigItem p;
        p.value = in.readU8();
        if (p.value != 4) {
            error = std::format("PMC config byte {} (the client wants 4)", p.value);
            return std::nullopt;
        }
        return finish(p);
    }
    case item::CFrameAck: {
        CFrameAckItem a;
        a.instance = readInstanceId(in);
        a.propertyId = in.readU16();
        a.hasVersion = in.readU8() != 0;
        if (a.hasVersion)
            a.version = in.readZigzag32();
        if (caps & caps::CFrameAckStep)
            a.step = in.readU32();
        return finish(a);
    }
    case item::StreamData: {
        StreamDataItem d;
        if (!readStreamData(in, d, error))
            return std::nullopt;
        return finish(std::move(d));
    }
    case item::RegionRemoval: {
        RegionRemovalItem r;
        r.region = readRegionId(in);
        r.sequence = in.readU32();
        const std::uint32_t n = in.readU32();
        r.flags = in.readU8();
        if (r.flags & 0x04)
            r.extra = in.readU32();
        const bool packed = (r.flags & 0x01) != 0;
        r.flags = static_cast<std::uint8_t>(r.flags & ~0x05); // the codec's bits: compressed refs, extra follows
        if (packed) {
            auto block = readCompressedBlock(in);
            if (!block) {
                error = "region removal: " + block.error();
                return std::nullopt;
            }
            NetworkStream refs(*block);
            for (std::uint32_t i = 0; i < n && !refs.bad(); ++i)
                r.removed.push_back(readInstanceId(refs));
            if (refs.bad()) {
                error = std::format("region removal: {} refs promised, the block has fewer", n);
                return std::nullopt;
            }
        } else {
            if (n > in.left()) {
                error = std::format("region removal with {} refs in {} bytes", n, in.left());
                return std::nullopt;
            }
            for (std::uint32_t i = 0; i < n && !in.bad(); ++i)
                r.removed.push_back(readInstanceId(in));
        }
        return finish(std::move(r));
    }
    case item::InstanceRemoval: {
        InstanceRemovalItem r;
        r.instance = readInstanceId(in);
        return finish(r);
    }
    case item::Stats: {
        StatsItem s;
        s.version = in.readI32();
        if (s.version >= 9) {
            s.a = in.readF32();
            s.b = in.readF32();
        }
        bool known = true;
        if (s.version >= 8 && known) {
            if (in.readU8() != 0) {
                auto block = readCompressedBlock(in);
                if (!block)
                    known = false;
                else
                    s.block8.assign(block->begin(), block->end());
            }
        }
        if (s.version >= 7 && known) {
            const std::int32_t len = in.readI32();
            auto block = readCompressedBlock(in);
            if (!block || static_cast<std::int32_t>(block->size()) != len)
                known = false;
            else
                s.block7.assign(block->begin(), block->end());
        }
        // Versions 6 and 3 add blocks this doesn't decode (0x1055de88e /
        // 0x1055df360), and a named-value list follows: keep the bytes.
        s.rest = in.readBytes(in.left());
        return finish(std::move(s));
    }
    case item::StreamingInfo: {
        StreamingInfoItem s;
        s.region = readRegionId(in);
        s.radius = in.readI32();
        s.version = in.readU32();
        if (caps & caps::StreamInfoExtra)
            s.extra = in.readI32();
        return finish(s);
    }
    case item::StreamPrefetchInfo: {
        StreamPrefetchInfoItem p;
        p.region = readRegionId(in);
        p.a = in.readI32();
        p.b = in.readI32();
        return finish(p);
    }
    case item::StreamPrefetchRequest: {
        StreamPrefetchRequestItem p;
        p.position.x = in.readF32();
        p.position.y = in.readF32();
        p.position.z = in.readF32();
        p.timeout = std::bit_cast<double>(in.readU64());
        p.id = in.readI32();
        return finish(p);
    }
    case item::StreamFocus: {
        StreamFocusItem f;
        f.focusId = in.readU32();
        f.region = readRegionId(in);
        f.completedRadius = in.readI32();
        const float radius = in.readF32();
        if (radius != -1.0f)
            f.radius = radius;
        f.flagA = in.readU8();
        f.flagB = in.readU8();
        if (caps & caps::FocusExtra)
            f.extra = in.readU32();
        return finish(f);
    }
    case item::StreamFocusRemove: {
        StreamFocusRemoveItem f;
        f.focusId = in.readU32();
        return finish(f);
    }
    case item::StreamFocusFrustum: {
        StreamFocusFrustumItem f;
        f.focusId = in.readU32();
        for (float& v : f.frustum)
            v = in.readF32();
        return finish(f);
    }
    case item::StreamFocusFrustumRemove: {
        StreamFocusFrustumRemoveItem f;
        f.focusId = in.readU32();
        return finish(f);
    }
    case item::Destroy: {
        DestroyItem d;
        const std::uint32_t n = in.readU32();
        if (n > in.left()) {
            error = std::format("destroy item with {} refs in {} bytes", n, in.left());
            return std::nullopt;
        }
        for (std::uint32_t i = 0; i < n && !in.bad(); ++i)
            if (const InstanceId id = readInstanceId(in); !id.null()) // null refs are skipped, as the client does
                d.instances.push_back(id);
        return finish(std::move(d));
    }
    case item::ClientCapacityUpdate: {
        ClientCapacityUpdateItem c;
        c.memoryLimited = in.readU8();
        c.quotaDelta = in.readI32();
        c.maxRegionRadius = static_cast<std::int16_t>(in.readU16());
        if (caps & caps::CapacityUpdateExtra) {
            c.bits = in.readU8();
            c.extra = in.readU32();
        }
        return finish(c);
    }
    case item::PersistentPlayerAdd:
    case item::PersistentPlayerRemove: {
        PersistentPlayerItem p;
        p.add = type == item::PersistentPlayerAdd;
        p.player = readInstanceId(in);
        return finish(p);
    }
    case item::ClientQoS: {
        ClientQoSItem q;
        q.kind = in.readU8();
        q.flag = in.readU8();
        if (q.kind != 1 && q.kind != 2) {
            error = std::format("Client QoS kind {} (the client wants 1 or 2)", q.kind);
            return std::nullopt;
        }
        q.payload = in.readString32(0x40000);
        return finish(std::move(q));
    }
    case item::IsrTimestamp: {
        IsrTimestampItem t;
        t.timestamp = in.readI32();
        return finish(t);
    }
    case item::StreamPinData: {
        StreamPinDataItem p;
        if (!readStreamData(in, p.data, error))
            return std::nullopt;
        p.instance = readInstanceId(in);
        p.value = in.readI32();
        p.flag = in.readU8();
        return finish(std::move(p));
    }
    case item::InstanceIntent: {
        InstanceIntentItem ii;
        const std::uint32_t n = in.readU32();
        if (n > in.left()) {
            error = std::format("instance intent item with {} entries in {} bytes", n, in.left());
            return std::nullopt;
        }
        for (std::uint32_t i = 0; i < n && !in.bad(); ++i) {
            const InstanceId id = readInstanceId(in);
            ii.intents.emplace_back(id, in.readU8());
        }
        return finish(std::move(ii));
    }
    case item::RegionTerrainLodRemoval: {
        RegionTerrainLodRemovalItem r;
        r.region = readRegionId(in);
        r.lod = in.readU8();
        return finish(r);
    }
    default:
        return std::nullopt;
    }
}

void describeRuntimeItem(const RuntimeItem& item, Node& n)
{
    std::visit(
        [&](const auto& b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, PingItem>) {
                n.add(std::format("flags: {:#04x}{}{}{}", b.flags, (b.flags & kPingServerBody) ? " server-body" : "",
                                  (b.flags & kPingTracer) ? " tracer" : "", (b.flags & kPingMemory) ? " memory" : ""));
                n.add(std::format("time: {} ms (echoed by the PingBack)", b.time));
                n.add(std::format("sendMs: {}", b.sendMs));
                if (b.flags & kPingServerBody)
                    n.add(std::format("fps: {}, {}, {} (GUESS: render, physics, heartbeat)", b.fps[0], b.fps[1],
                                      b.fps[2]));
                n.add(std::format("mask: {:#x}", b.mask));
                n.add(std::format("extra: {:#x}", b.extra));
                n.add(std::format("kind: {} ({})", b.kind, b.kind ? "periodic stats ping" : "plain"));
                if (b.flags & kPingXorred)
                    n.add(std::format("xorred: {:#x} (u32 ^ low 32 bits of time)", b.xorred));
                if (b.flags & kPingMemory)
                    n.add(std::format("memory: {} MB", b.memoryMb));
            } else if constexpr (std::is_same_v<T, PingBackItem>) {
                n.add(std::format("flags: {:#04x}", b.flags));
                n.add(std::format("time: {} ms (the ping's)", b.time));
                n.add(std::format("pingSendMs: {}  recvMs: {}  nowMs: {}", b.pingSendMs, b.recvMs, b.nowMs));
                n.add(std::format("zero: {}  counter: {}  lastPacketBytes: {}  high: {}", b.zero, b.counter,
                                  b.lastPacketBytes, b.lastPacketHigh));
                n.add(std::format("mask: {:#x}  extra: {:#x}  kind: {}", b.mask, b.extra, b.kind));
            } else if constexpr (std::is_same_v<T, RequestCharacterItem>) {
                n.add(std::format("playerFlags: {:#x}", b.playerFlags));
                n.add(std::format("appearance: \"{}\" (GUESS: Player character appearance)", b.appearance));
                n.add(std::format("player: {}", idText(b.player)));
            } else if constexpr (std::is_same_v<T, PmcConfigItem>) {
                n.add(std::format("value: {}", b.value));
            } else if constexpr (std::is_same_v<T, CFrameAckItem>) {
                n.add(std::format("instance: {}", idText(b.instance)));
                n.add(std::format("propertyId: {}", b.propertyId));
                n.add(b.hasVersion ? std::format("version: {}", b.version) : std::string("no version"));
                n.add(std::format("step: {} (capability 33)", b.step));
            } else if constexpr (std::is_same_v<T, StreamDataItem>) {
                describeStreamData(b, n);
            } else if constexpr (std::is_same_v<T, RegionRemovalItem>) {
                n.add(std::format("region: {}", regionText(b.region)));
                n.add(std::format("sequence: {} (regions removed so far)  flags: {:#04x}  extra: {}", b.sequence,
                                  b.flags, b.extra));
                Node& list = n.add(std::format("instances removed ({})", b.removed.size()));
                for (std::size_t i = 0; i < b.removed.size() && i < 32; ++i)
                    list.add(idText(b.removed[i]));
                if (b.removed.size() > 32)
                    list.add(std::format("... {} more", b.removed.size() - 32));
            } else if constexpr (std::is_same_v<T, InstanceRemovalItem>) {
                n.add(std::format("instance: {}", idText(b.instance)));
            } else if constexpr (std::is_same_v<T, StatsItem>) {
                n.add(std::format("version: {}", b.version));
                if (b.version >= 9)
                    n.add(std::format("a: {}  b: {}", b.a, b.b));
                if (!b.block8.empty())
                    n.add(std::format("block (v8): \"{}\"", b.block8.substr(0, 120)));
                if (!b.block7.empty())
                    n.add(std::format("block (v7): \"{}\"", b.block7.substr(0, 120)));
                if (!b.rest.empty())
                    addHex(n.add(std::format("rest, {} bytes (not decoded)", b.rest.size())), b.rest, 0, 96);
            } else if constexpr (std::is_same_v<T, StreamingInfoItem>) {
                n.add(std::format("region: {}  radius: {}  version: {}  extra: {}", regionText(b.region), b.radius,
                                  b.version, b.extra));
            } else if constexpr (std::is_same_v<T, StreamPrefetchInfoItem>) {
                n.add(std::format("region: {}  a: {}  id: {}", regionText(b.region), b.a, b.b));
            } else if constexpr (std::is_same_v<T, StreamPrefetchRequestItem>) {
                n.add(std::format("position: ({}, {}, {})  timeout: {} s  id: {}", b.position.x, b.position.y,
                                  b.position.z, b.timeout, b.id));
            } else if constexpr (std::is_same_v<T, StreamFocusItem>) {
                n.add(std::format("focusId: {}  region: {}  completedRadius: {}", b.focusId, regionText(b.region),
                                  b.completedRadius));
                n.add(std::format("radius: {}  flags: {} {}  extra: {}",
                                  b.radius ? std::format("{}", *b.radius) : "none", b.flagA, b.flagB, b.extra));
            } else if constexpr (std::is_same_v<T, StreamFocusRemoveItem>) {
                n.add(std::format("focusId: {}", b.focusId));
            } else if constexpr (std::is_same_v<T, StreamFocusFrustumItem>) {
                n.add(std::format("focusId: {}", b.focusId));
                std::string f;
                for (float v : b.frustum)
                    f += std::format("{}{}", f.empty() ? "" : ", ", v);
                n.add("frustum: " + f);
            } else if constexpr (std::is_same_v<T, StreamFocusFrustumRemoveItem>) {
                n.add(std::format("focusId: {}", b.focusId));
            } else if constexpr (std::is_same_v<T, DestroyItem>) {
                Node& d = n.add(std::format("instances ({})", b.instances.size()));
                for (InstanceId id : b.instances)
                    d.add(idText(id));
            } else if constexpr (std::is_same_v<T, ClientCapacityUpdateItem>) {
                n.add(std::format("memoryLimited: {}  quota change: {:+}  max region radius: {}{}", b.memoryLimited,
                                  b.quotaDelta, b.maxRegionRadius,
                                  b.maxRegionRadius == kUnlimitedRegionRadius ? " (unlimited)" : ""));
                if (b.bits != 0 || b.extra != 0)
                    n.add(std::format("bits: {}  extra: {} (capability 37)", b.bits, b.extra));
            } else if constexpr (std::is_same_v<T, PersistentPlayerItem>) {
                n.add(std::format("{}: {}", b.add ? "add" : "remove", idText(b.player)));
            } else if constexpr (std::is_same_v<T, ClientQoSItem>) {
                n.add(std::format("kind: {}  flag: {}  payload: {} bytes", b.kind, b.flag, b.payload.size()));
            } else if constexpr (std::is_same_v<T, IsrTimestampItem>) {
                n.add(std::format("timestamp: {}", b.timestamp));
            } else if constexpr (std::is_same_v<T, StreamPinDataItem>) {
                describeStreamData(b.data, n);
                n.add(std::format("pinned: {}  value: {}  flag: {}", idText(b.instance), b.value, b.flag));
            } else if constexpr (std::is_same_v<T, InstanceIntentItem>) {
                Node& d = n.add(std::format("intents ({})", b.intents.size()));
                for (const auto& [id, intent] : b.intents)
                    d.add(std::format("{}: {}", idText(id), intent));
            } else {
                n.add(std::format("region: {}  lod: {}", regionText(b.region), b.lod));
            }
        },
        item.body);
}

} // namespace lunar::net

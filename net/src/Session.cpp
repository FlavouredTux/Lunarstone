#include <lunar/net/Session.hpp>

#include <lunar/Instance.hpp>
#include <lunar/classes/World.hpp>
#include <lunar/net/Join.hpp>
#include <lunar/net/Messages.hpp>
#include <lunar/net/Replication.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <format>

#include <zstd.h>

namespace lunar::net {

namespace {

std::string idText(InstanceId id)
{
    return id.null() ? "null" : std::format("{}:{}", id.scope, id.index);
}

NetworkStream body(std::span<const std::uint8_t> message)
{
    NetworkStream in(message);
    in.readU8();
    return in;
}

} // namespace

// -- kick

std::vector<std::uint8_t> encodeKick(const KickMessage& kick, std::uint64_t capabilities)
{
    NetworkStream out;
    out.writeU8(msg::Kick);
    out.writeString32(kick.message);
    if (capabilities & caps::KickMetadata)
        out.writeString32(kick.metadataJson.value_or(""));
    return std::move(out).take();
}

std::optional<KickMessage> decodeKick(std::span<const std::uint8_t> message, std::uint64_t capabilities)
{
    if (message.empty() || message[0] != msg::Kick)
        return std::nullopt;
    NetworkStream in = body(message);
    KickMessage k;
    k.message = in.readString32(1u << 20);
    if ((capabilities & caps::KickMetadata) && in.left() >= 4)
        k.metadataJson = in.readString32(1u << 20);
    if (in.bad())
        return std::nullopt;
    return k;
}

// -- chat

std::vector<std::uint8_t> encodeChat(const ChatMessage& chat)
{
    NetworkStream out;
    out.writeU8(chat.id);
    if (chat.id != msg::ChatGame)
        writeInstanceId(out, chat.sender);
    if (chat.id == msg::ChatPlayer)
        writeInstanceId(out, chat.target);
    out.writeString32(chat.text);
    if (chat.flagA)
        out.writeU8(*chat.flagA);
    if (chat.flagB)
        out.writeU8(*chat.flagB);
    return std::move(out).take();
}

std::optional<ChatMessage> decodeChat(std::span<const std::uint8_t> message)
{
    if (message.empty())
        return std::nullopt;
    const std::uint8_t id = message[0];
    if (id != msg::ChatAll && id != msg::ChatTeam && id != msg::ChatGame && id != msg::ChatPlayer)
        return std::nullopt;
    NetworkStream in = body(message);
    ChatMessage c;
    c.id = id;
    if (id != msg::ChatGame)
        c.sender = readInstanceId(in);
    if (id == msg::ChatPlayer)
        c.target = readInstanceId(in);
    c.text = in.readString32(1u << 20);
    if (in.bad())
        return std::nullopt;
    if (in.left() > 0)
        c.flagA = in.readU8();
    if (in.left() > 0)
        c.flagB = in.readU8();
    return c;
}

// -- cluster

namespace {

void writeChunkDelta(NetworkStream& out, std::uint8_t& header, RegionId delta)
{
    const auto fits8 = [](std::int32_t v) { return v >= -128 && v <= 127; };
    const auto fits16 = [](std::int32_t v) { return v >= -32768 && v <= 32767; };
    header = static_cast<std::uint8_t>(header & ~0x30);
    if (fits8(delta.x) && fits8(delta.y) && fits8(delta.z)) {
        out.writeU8(static_cast<std::uint8_t>(static_cast<std::int8_t>(delta.x)));
        out.writeU8(static_cast<std::uint8_t>(static_cast<std::int8_t>(delta.y)));
        out.writeU8(static_cast<std::uint8_t>(static_cast<std::int8_t>(delta.z)));
    } else if (fits16(delta.x) && fits16(delta.y) && fits16(delta.z)) {
        header |= 0x10;
        out.writeI16(static_cast<std::int16_t>(delta.x));
        out.writeI16(static_cast<std::int16_t>(delta.y));
        out.writeI16(static_cast<std::int16_t>(delta.z));
    } else {
        header |= 0x20;
        out.writeI32(delta.x);
        out.writeI32(delta.y);
        out.writeI32(delta.z);
    }
}

} // namespace

std::vector<std::uint8_t> encodeCluster(const ClusterMessage& cluster)
{
    NetworkStream out;
    out.writeU8(msg::Cluster);
    writeInstanceId(out, cluster.terrain);
    if (cluster.clusterId) {
        out.writeU8(*cluster.clusterId);
        if (*cluster.clusterId == 5)
            out.writeI32(cluster.clusterValue);
    }
    NetworkStream raw;
    RegionId previous;
    for (const ClusterChunk& c : cluster.chunks) {
        NetworkStream delta;
        std::uint8_t header = c.header;
        writeChunkDelta(delta, header,
                        {c.position.x - previous.x, c.position.y - previous.y, c.position.z - previous.z});
        previous = c.position;
        raw.writeU8(header);
        raw.writeBytes(delta.bytes());
        raw.writeU8(c.empty ? 1 : 0);
        if (!c.empty)
            raw.writeBytes(c.voxels);
        raw.writeU8(0);
    }
    raw.writeBytes(cluster.undecoded);
    writeCompressedBlock(out, raw.bytes());
    return std::move(out).take();
}

std::optional<ClusterMessage> decodeCluster(std::span<const std::uint8_t> message)
{
    if (message.empty() || message[0] != msg::Cluster)
        return std::nullopt;
    NetworkStream in = body(message);
    ClusterMessage c;
    c.terrain = readInstanceId(in);
    if (in.bad())
        return std::nullopt;
    // With or without the cluster id byte: whichever leaves a compressed
    // block whose lengths fit (GUESS: the replicator flag at rep+0x3c90 is on).
    std::optional<std::vector<std::uint8_t>> raw;
    const std::size_t at = in.offset();
    if (in.left() >= 1 && in.bytes()[at] <= 5) {
        NetworkStream probe = in;
        const std::uint8_t id = probe.readU8();
        std::int32_t value = 0;
        if (id == 5)
            value = probe.readI32();
        if (auto block = readCompressedBlock(probe); block && probe.left() == 0) {
            c.clusterId = id;
            c.clusterValue = value;
            raw = std::move(*block);
            in = probe;
        }
    }
    if (!raw) {
        auto block = readCompressedBlock(in);
        if (!block)
            return std::nullopt;
        raw = std::move(*block);
    }
    NetworkStream chunks(*raw);
    RegionId position;
    while (chunks.left() > 0) {
        const std::size_t start = chunks.offset();
        ClusterChunk chunk;
        const std::uint8_t header = chunks.readU8();
        chunk.header = static_cast<std::uint8_t>(header & ~0x30); // bits 4-5 are the delta width, the codec's
        RegionId delta;
        switch ((header >> 4) & 3) {
        case 0:
            delta.x = static_cast<std::int8_t>(chunks.readU8());
            delta.y = static_cast<std::int8_t>(chunks.readU8());
            delta.z = static_cast<std::int8_t>(chunks.readU8());
            break;
        case 1:
            delta.x = chunks.readI16();
            delta.y = chunks.readI16();
            delta.z = chunks.readI16();
            break;
        case 2:
            delta.x = chunks.readI32();
            delta.y = chunks.readI32();
            delta.z = chunks.readI32();
            break;
        default:
            break;
        }
        position = {position.x + delta.x, position.y + delta.y, position.z + delta.z};
        chunk.position = position;
        // All air, or runs up to the chunk's cells; then the 0.
        const std::uint8_t kind = chunks.readU8();
        bool fits = !chunks.bad() && (kind == 0 || kind == 1) && (header & 0x0f) <= 8;
        if (fits && kind == 1) {
            chunk.empty = true;
        } else if (fits) {
            const std::size_t cells = std::size_t{1} << (3 * (header & 0x0f));
            const std::size_t from = chunks.offset();
            for (std::size_t filled = 0; filled < cells && fits;) {
                const std::uint8_t run = chunks.readU8();
                if (run & 0x40)
                    chunks.readU8();
                std::size_t count = 1;
                if (run & 0x80) {
                    const std::uint8_t n = chunks.readU8();
                    if (n == 0)
                        chunks.readU8();
                    count = n + 1u;
                }
                fits = !chunks.bad() && count <= cells - filled;
                filled += count;
            }
            if (fits) {
                const std::size_t to = chunks.offset();
                chunks.seek(from);
                chunk.voxels = chunks.readBytes(to - from);
            }
        }
        if (!fits || chunks.readU8() != 0 || chunks.bad()) {
            // Not a chunk as we know them: keep the rest raw.
            chunks.seek(start);
            c.undecoded = chunks.readBytes(chunks.left());
            break;
        }
        c.chunks.push_back(std::move(chunk));
    }
    return c;
}

std::optional<Terrain::Block> clusterBlock(const ClusterChunk& chunk)
{
    const unsigned shift = chunk.header & 0x0f;
    if (shift == 0 || shift > 8)
        return std::nullopt;
    Terrain::Block block;
    block.side = 1 << shift;
    block.origin = {chunk.position.x * block.side, chunk.position.y * block.side, chunk.position.z * block.side};
    if (chunk.empty)
        return block;
    const std::size_t cells = std::size_t{1} << (3 * shift);
    NetworkStream in(chunk.voxels);
    std::size_t filled = 0;
    while (filled < cells && in.left() > 0) {
        const std::uint8_t header = in.readU8();
        const std::uint8_t index = header & 0x3f;
        if (index >= Terrain::materials.size())
            return std::nullopt;
        TerrainChunk::Run run{0, index, 255, 0};
        if (header & 0x40)
            run.occupancy = in.readU8();
        std::size_t count = 1;
        if (header & 0x80) {
            const std::uint8_t n = in.readU8();
            if (n == 0)
                run.water = in.readU8();
            count = n + 1u;
        }
        if (in.bad() || count > cells - filled)
            return std::nullopt;
        // As a place file's: air holds nothing, and water only shares a cell that isn't full.
        if (index == 0)
            run.occupancy = 0;
        if (index < 2 || run.occupancy == 255)
            run.water = 0;
        run.count = static_cast<std::uint16_t>(count);
        block.runs.push_back(run);
        filled += count;
    }
    if (filled != cells || in.left() != 0)
        return std::nullopt;
    return block;
}

// -- touches

std::vector<std::uint8_t> encodeTouches(const TouchesMessage& touches)
{
    NetworkStream out;
    out.writeU8(msg::Touches);
    for (const TouchEvent& t : touches.touches) {
        if (t.partA.null())
            continue;
        writeInstanceId(out, t.partA);
        writeInstanceId(out, t.partB);
        out.writeU8(t.state);
    }
    writeInstanceId(out, {});
    return std::move(out).take();
}

std::optional<TouchesMessage> decodeTouches(std::span<const std::uint8_t> message)
{
    if (message.empty() || message[0] != msg::Touches)
        return std::nullopt;
    NetworkStream in = body(message);
    TouchesMessage t;
    while (in.left() > 0) {
        TouchEvent ev;
        ev.partA = readInstanceId(in);
        if (ev.partA.null())
            break;
        ev.partB = readInstanceId(in);
        ev.state = in.readU8();
        if (in.bad())
            return std::nullopt;
        t.touches.push_back(ev);
    }
    return t;
}

// -- shared string hashes

std::vector<std::uint8_t> encodeSharedStringHashes(const SharedStringHashesMessage& m)
{
    NetworkStream out;
    out.writeU8(msg::SharedStringHashes);
    out.writeU16(static_cast<std::uint16_t>(m.hashes.size()));
    for (const auto& h : m.hashes)
        out.writeBytes(h);
    return std::move(out).take();
}

std::optional<SharedStringHashesMessage> decodeSharedStringHashes(std::span<const std::uint8_t> message)
{
    if (message.empty() || message[0] != msg::SharedStringHashes)
        return std::nullopt;
    NetworkStream in = body(message);
    const std::uint16_t count = in.readU16();
    SharedStringHashesMessage m;
    const bool tagged = in.left() != std::size_t{count} * 16; // the kind-tagged form
    for (std::uint16_t i = 0; i < count; ++i) {
        if (tagged && in.readU8() != 0)
            return std::nullopt; // kind 1 isn't decoded
        if (in.left() < 16)
            return std::nullopt;
        const auto bytes = in.readBytes(16);
        std::array<std::uint8_t, 16> h{};
        std::copy(bytes.begin(), bytes.end(), h.begin());
        m.hashes.push_back(h);
    }
    return m;
}

// -- 0x9a / 0x9b / 0x9c

std::optional<MicroprofilerMessage> decodeMicroprofiler(std::span<const std::uint8_t> message)
{
    if (message.empty() || message[0] != msg::Microprofiler)
        return std::nullopt;
    NetworkStream in = body(message);
    MicroprofilerMessage m;
    m.kind = in.readI32();
    m.length = in.readI32();
    auto block = readCompressedBlock(in);
    if (!block || in.bad())
        return std::nullopt;
    NetworkStream inner(*block);
    m.html = inner.readString32(256u << 20);
    if (inner.bad())
        m.html.assign(block->begin(), block->end());
    return m;
}

std::vector<std::uint8_t> encodeBytecodeChallenge(const BytecodeChallenge& c)
{
    NetworkStream out;
    out.writeU8(msg::BytecodeChallenge);
    out.writeU32(c.a);
    out.writeU32(c.b);
    out.writeString32(c.bytecode);
    return std::move(out).take();
}

std::optional<BytecodeChallenge> decodeBytecodeChallenge(std::span<const std::uint8_t> message)
{
    if (message.empty() || message[0] != msg::BytecodeChallenge)
        return std::nullopt;
    NetworkStream in = body(message);
    BytecodeChallenge c;
    c.a = in.readU32();
    c.b = in.readU32();
    c.bytecode = in.readString32(16u << 20);
    if (in.bad())
        return std::nullopt;
    return c;
}

std::expected<std::string, std::string> decodeProtectedBytecode(std::string_view blob)
{
    static constexpr std::array<std::uint8_t, 4> kSignature{'R', 'S', 'B', '1'};
    if (blob.size() < 8)
        return std::unexpected("protected bytecode shorter than its header");
    std::array<std::uint8_t, 4> key{};
    for (std::size_t i = 0; i < 4; ++i)
        key[i] = static_cast<std::uint8_t>((static_cast<std::uint8_t>(blob[i]) ^ kSignature[i]) - i * 41);
    std::vector<std::uint8_t> plain(blob.size());
    for (std::size_t i = 0; i < blob.size(); ++i)
        plain[i] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(blob[i]) ^ (key[i % 4] + i * 41));
    const std::uint32_t hash = static_cast<std::uint32_t>(key[0]) | static_cast<std::uint32_t>(key[1]) << 8
                               | static_cast<std::uint32_t>(key[2]) << 16 | static_cast<std::uint32_t>(key[3]) << 24;
    if (xxhash32(plain, 42) != hash)
        return std::unexpected("protected bytecode hash mismatch");
    const std::uint32_t size = static_cast<std::uint32_t>(plain[4]) | static_cast<std::uint32_t>(plain[5]) << 8
                               | static_cast<std::uint32_t>(plain[6]) << 16 | static_cast<std::uint32_t>(plain[7]) << 24;
    if (size > (64u << 20))
        return std::unexpected("protected bytecode too large");
    std::string out(size, '\0');
    const std::size_t n = ZSTD_decompress(out.data(), out.size(), plain.data() + 8, plain.size() - 8);
    if (ZSTD_isError(n) || n != size)
        return std::unexpected("protected bytecode did not decompress");
    return out;
}

std::vector<std::uint8_t> encodeBytecodeResponse(const BytecodeResponse& r)
{
    NetworkStream out;
    out.writeU8(msg::BytecodeChallenge);
    out.writeU32(r.b);
    out.writeU32(r.result);
    return std::move(out).take();
}

std::optional<BytecodeResponse> decodeBytecodeResponse(std::span<const std::uint8_t> message)
{
    if (message.size() != 9 || message[0] != msg::BytecodeChallenge)
        return std::nullopt;
    NetworkStream in = body(message);
    BytecodeResponse r;
    r.b = in.readU32();
    r.result = in.readU32();
    return r;
}

std::optional<ScriptBlobMessage> decodeScriptBlob(std::span<const std::uint8_t> message)
{
    if (message.empty() || message[0] != msg::ScriptBlob)
        return std::nullopt;
    NetworkStream in = body(message);
    ScriptBlobMessage s;
    s.kind = in.readU8();
    if (s.kind >= 3)
        s.kind = 2; // the client clamps it and reads nothing more
    else {
        s.id = in.readU32();
        if (s.kind == 0) {
            const std::int16_t n = in.readI16();
            if (n < 0 || static_cast<std::size_t>(n) > in.left())
                return std::nullopt;
            s.blob = in.readBytes(static_cast<std::size_t>(n));
        }
    }
    if (in.bad())
        return std::nullopt;
    return s;
}

// -- describing and applying

namespace {

std::string refText(InstanceId id, const Replica* replica)
{
    if (id.null())
        return "null";
    if (replica)
        if (auto inst = replica->find(id))
            return std::format("{} ({})", inst->fullName(), idText(id));
    return idText(id);
}

void describeCluster(const ClusterMessage& c, Node& n, const Replica* replica)
{
    n.add(std::format("terrain: {}", refText(c.terrain, replica)));
    if (c.clusterId)
        n.add(std::format("clusterId: {}{}", *c.clusterId,
                          *c.clusterId == 5 ? std::format(", value {}", c.clusterValue) : ""));
    Node& chunks = n.add(std::format("chunks ({})", c.chunks.size()));
    for (const ClusterChunk& ch : c.chunks) {
        Node& cn = chunks.add(std::format(
            "chunk at {} (header {:#04x}: size 2^{} cells), {}", regionText(ch.position), ch.header, ch.header & 0x0f,
            ch.empty ? std::string("all air") : std::format("{} run bytes", ch.voxels.size())));
        if (auto block = clusterBlock(ch); block && !ch.empty)
            cn.add(std::format("{} runs from cell {} {} {}", block->runs.size(), block->origin[0], block->origin[1],
                               block->origin[2]));
        else if (!block)
            addHex(cn, ch.voxels, 0, 48);
    }
    if (!c.undecoded.empty())
        addHex(n.add(std::format("undecoded, {} bytes", c.undecoded.size())), c.undecoded, 0, 96);
}

} // namespace

bool describeSessionMessage(std::span<const std::uint8_t> message, Node& n, std::uint64_t capabilities,
                            const Replica* replica)
{
    if (message.empty())
        return false;
    auto dump = [&](std::string_view note) {
        Node& r = n.add(std::string(note));
        addHex(r, message.subspan(1), 1, 256);
        return true;
    };
    switch (message[0]) {
    case msg::Kick: {
        if (auto k = decodeKick(message, capabilities)) {
            n.add(std::format("kick message: \"{}\"", k->message));
            if (k->metadataJson)
                n.add(std::format("metadata: {}", *k->metadataJson));
            return true;
        }
        return dump("kick: undecodable");
    }
    case msg::ChatAll:
    case msg::ChatTeam:
    case msg::ChatGame:
    case msg::ChatPlayer: {
        if (auto c = decodeChat(message)) {
            if (c->id != msg::ChatGame)
                n.add(std::format("sender: {}", refText(c->sender, replica)));
            if (c->id == msg::ChatPlayer)
                n.add(std::format("target: {}", refText(c->target, replica)));
            n.add(std::format("text: \"{}\"", c->text));
            if (c->flagA || c->flagB)
                n.add(std::format("flags: {} {}", c->flagA ? std::format("{}", *c->flagA) : "-",
                                  c->flagB ? std::format("{}", *c->flagB) : "-"));
            return true;
        }
        return dump("chat: undecodable");
    }
    case msg::Cluster: {
        if (auto c = decodeCluster(message)) {
            describeCluster(*c, n, replica);
            return true;
        }
        return dump("cluster: undecodable (bad compressed block)");
    }
    case msg::Microprofiler: {
        if (auto m = decodeMicroprofiler(message)) {
            n.add(std::format("kind: {}  length: {}  html: {} bytes", m->kind, m->length, m->html.size()));
            return true;
        }
        return dump("microprofiler: undecodable");
    }
    case msg::BytecodeChallenge: {
        if (auto r = decodeBytecodeResponse(message)) {
            n.add(std::format("bytecode challenge response: b {}, result {}", r->b, r->result));
            return true;
        }
        if (auto c = decodeBytecodeChallenge(message)) {
            n.add(std::format("bytecode challenge: a {}, b {}, {} bytes of Luau bytecode", c->a, c->b,
                              c->bytecode.size()));
            return true;
        }
        return dump("bytecode challenge: undecodable");
    }
    case msg::ScriptBlob: {
        if (auto s = decodeScriptBlob(message)) {
            n.add(std::format("kind: {}  id: {}  blob: {} bytes", s->kind, s->id, s->blob.size()));
            if (!s->blob.empty())
                addHex(n, s->blob, 0, 64);
            return true;
        }
        return dump("0x9c: undecodable");
    }
    case msg::Rna2:
    case msg::Rna3:
    case msg::Rna4: {
        // The security channel (SecurityChannelJob; parsed by 0x1057a6606):
        // each id orders the header fields its own way, and each field is
        // NOTed and rotated by an amount derived from the id.
        const std::uint8_t id = message[0];
        NetworkStream in = body(message);
        std::uint32_t key = 0, seq = 0;
        std::uint8_t type = 0;
        if (id == msg::Rna2) {
            in.readU32();
            key = in.readU32();
            type = in.readU8();
            seq = in.readU32();
        } else if (id == msg::Rna3) {
            type = in.readU8();
            in.readU32();
            key = in.readU32();
            seq = in.readU32();
        } else {
            type = in.readU8();
            seq = in.readU32();
            in.readU32();
            key = in.readU32();
        }
        if (in.bad())
            return dump("security channel: truncated header");
        key = std::rotr(~key, id + 11);
        type = std::rotr(static_cast<std::uint8_t>(~type), id + 1);
        seq = std::rotl(~seq, id + 9);
        if (type == 'b') {
            n.add(std::format("security channel: ack of report {}", seq));
            return true;
        }
        if (type != 'H')
            return dump(std::format("security channel: unknown type {:#x}", type));
        // A report (ClientQoSImpl::secureHeartbeat, 0x10581f30c): a check
        // byte, seven obfuscated words, the payload length twice over, then
        // the payload. Not decoded further.
        const std::uint8_t check = in.readU8();
        const bool checkOk = check == static_cast<std::uint8_t>(key % 134 + 0x79);
        for (int i = 0; i < 7; ++i)
            in.readU32();
        const std::uint32_t a = std::rotr(~in.readU32(), id + 0x65);
        const std::uint32_t b = std::rotl(~in.readU32(), id + 0x65);
        const std::uint32_t length = a ^ key;
        const bool lengthOk = length == (b ^ std::rotl(key, id + 0x15));
        if (in.bad())
            return dump("security channel: truncated report");
        Node& r = n.add(std::format("security channel: report {}, check byte {}, {} bytes of payload{}", seq,
                                    checkOk ? "ok" : "BAD", length, lengthOk ? "" : " (length check BAD)"));
        if (in.left() > 0)
            addHex(r, message.subspan(in.offset()), in.offset(), 64);
        return true;
    }
    case msg::Isr:
        return dump("ISR packet (0xa1): the replica decodes it (Physics.hpp); bytes:");
    case msg::Touches: {
        if (auto t = decodeTouches(message)) {
            Node& list = n.add(std::format("touches ({})", t->touches.size()));
            for (const TouchEvent& ev : t->touches)
                list.add(std::format("{} / {}: state {}{}", refText(ev.partA, replica), refText(ev.partB, replica),
                                     ev.state & 0x7f, (ev.state & 0x80) ? " (bit 7)" : ""));
            return true;
        }
        return dump("touches: undecodable");
    }
    case msg::ProtocolMismatch:
        return dump("protocol mismatch: the client reads no body (disconnect 0x103)");
    case msg::SecurityKeyMismatch:
        return dump("security key mismatch: the client reads no body (\"SecurityKeyRejected\")");
    case msg::HashMismatch:
        return dump("0x94: no client handler found (Client::OnReceive boundary), bytes:");
    case msg::RequestStats:
        return dump("request stats (client -> server; server case 15), bytes:");
    case msg::Queued5:
        return dump("large replicator packet (0x104b7222c: u8 action, acks / frame ends); not decoded, bytes:");
    case msg::Queued6:
        return dump("ISR receiver packet (0x1055003a4), only read with ISR on; bytes:");
    case msg::SharedStringHashes: {
        if (auto m = decodeSharedStringHashes(message)) {
            Node& list = n.add(std::format("shared string hashes ({})", m->hashes.size()));
            for (const auto& h : m->hashes)
                list.add(hexString(h));
            return true;
        }
        return dump("shared string hashes: undecodable (kind-tagged form with kind 1?), bytes:");
    }
    default:
        return false;
    }
}

namespace {

// Appends to a record of what came that nothing in the client reads back
// (the terrain clusters, the touches): only the latest `kept` stay, or a
// session would hold every one it was ever sent.
template <class T>
void record(std::vector<T>& list, T item, std::size_t kept = RuntimeState::recordLimit)
{
    list.push_back(std::move(item));
    if (list.size() >= kept * 2)
        list.erase(list.begin(), list.end() - static_cast<std::ptrdiff_t>(kept));
}

} // namespace

bool handleSessionMessage(Replica& replica, std::span<const std::uint8_t> message, Node* tree)
{
    if (message.empty())
        return false;
    Node scratch;
    Node& n = tree ? *tree : scratch;
    switch (message[0]) {
    case msg::Kick:
        if (auto k = decodeKick(message, replica.capabilities))
            replica.runtime.kicks.push_back(*k);
        break;
    case msg::ChatAll:
    case msg::ChatTeam:
    case msg::ChatGame:
    case msg::ChatPlayer:
        if (auto c = decodeChat(message))
            replica.runtime.chats.push_back(std::move(*c));
        break;
    case msg::Cluster:
        if (auto c = decodeCluster(message)) {
            // Terrain the server streams: written into the replica's Terrain.
            if (auto inst = replica.find(c->terrain))
                if (auto* terrain = dynamic_cast<Terrain*>(inst.get()))
                    for (const ClusterChunk& ch : c->chunks)
                        if (auto block = clusterBlock(ch))
                            terrain->write(*block);
            record(replica.runtime.clusters, std::move(*c));
        }
        break;
    case msg::BytecodeChallenge:
        if (auto c = decodeBytecodeChallenge(message))
            replica.runtime.challenges.push_back(std::move(*c));
        break;
    case msg::Touches:
        if (auto t = decodeTouches(message))
            record(replica.runtime.touches, std::move(*t));
        break;
    default:
        break;
    }
    return describeSessionMessage(message, n, replica.capabilities, &replica);
}

void applyRuntimeItem(Replica& replica, const RuntimeItem& item)
{
    RuntimeState& rt = replica.runtime;
    std::visit(
        [&](const auto& b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, PingItem>)
                rt.pendingPings.push_back(b);
            else if constexpr (std::is_same_v<T, PingBackItem>)
                rt.pingBacks.push_back(b);
            else if constexpr (std::is_same_v<T, RequestCharacterItem>)
                rt.characterRequests.push_back(b);
            else if constexpr (std::is_same_v<T, DestroyItem>) {
                for (InstanceId id : b.instances)
                    replica.remove(id);
                rt.destroyed += b.instances.size();
            } else if constexpr (std::is_same_v<T, StreamDataItem>)
                ++rt.streamDataItems;
            else if constexpr (std::is_same_v<T, StreamPinDataItem>)
                ++rt.streamDataItems;
            else if constexpr (std::is_same_v<T, StreamFocusItem>)
                rt.focuses.push_back(b);
            else if constexpr (std::is_same_v<T, StreamingInfoItem>)
                rt.streamingInfos.push_back(b);
            else if constexpr (std::is_same_v<T, ClientCapacityUpdateItem>)
                rt.capacityUpdates.push_back(b);
            else if constexpr (std::is_same_v<T, StreamPrefetchInfoItem>)
                rt.prefetchInfos.push_back(b);
            else if constexpr (std::is_same_v<T, StreamPrefetchRequestItem>)
                rt.prefetchRequests.push_back(b);
            else if constexpr (std::is_same_v<T, StreamFocusRemoveItem>)
                rt.focusRemovals.push_back(b.focusId);
            else if constexpr (std::is_same_v<T, IsrTimestampItem>) {
                if (replica.decodingServerToClient)
                    rt.serverIsrTick = static_cast<std::uint32_t>(b.timestamp);
            } else if constexpr (std::is_same_v<T, RegionRemovalItem>) {
                // A decoder's replica follows the client: what it streamed out is gone.
                if (!replica.decodingServerToClient)
                    for (InstanceId id : b.removed)
                        replica.remove(id);
                rt.regionRemovals.push_back(b);
            }
        },
        item.body);
}

// -- ClientSession

ClientSession::ClientSession(ClientSessionOptions options) : options_(std::move(options)) {}

void ClientSession::onMessage(std::span<const std::uint8_t> message)
{
    if (message.empty())
        return;
    switch (message[0]) {
    case kPlaceIdVerifiedId:
        if (message.size() >= 2 && message[1] != 0)
            placeIdVerified_ = true;
        break;
    case msg::Kick:
        if (auto k = decodeKick(message, options_.capabilities))
            kicked = *k;
        break;
    case msg::BytecodeChallenge:
        if (auto c = decodeBytecodeChallenge(message))
            pendingChallenges_.push_back(std::move(*c));
        break;
    default:
        break;
    }
}

std::vector<std::vector<std::uint8_t>> ClientSession::tick(Replica& replica, std::uint64_t nowMs)
{
    std::vector<std::vector<std::uint8_t>> out;
    static const Schema empty;
    const Schema& schema = replica.schema() ? *replica.schema() : empty;
    const ValueContext values = replica.context();
    std::vector<Item> items;
    const ItemContext ctx{false, options_.capabilities};
    auto flushItems = [&] {
        if (!items.empty())
            out.push_back(encodeData(items, schema, values, replica.framing, ctx));
        items.clear();
    };
    // Answers first: PingBacks for the server's pings.
    for (const PingItem& p : replica.runtime.pendingPings) {
        items.emplace_back(
            RuntimeItem{answerPing(p, static_cast<std::uint32_t>(nowMs), static_cast<std::uint32_t>(nowMs))});
        ++pingBacksSent;
    }
    replica.runtime.pendingPings.clear();
    for (const PingBackItem& b : replica.runtime.pingBacks) {
        roundTripsMs.push_back(pingRoundTripMs(b, static_cast<std::uint32_t>(nowMs)));
        ++pingBacksReceived;
    }
    replica.runtime.pingBacks.clear();
    // Markers go back as Marker items.
    if (options_.echoMarkers)
        for (; markersSeen_ < replica.markers.size(); ++markersSeen_) {
            items.emplace_back(MarkerItem{replica.markers[markersSeen_]});
            ++markersEchoed;
        }
    // The character request, once, after the place id is verified and the join's first marker is in.
    if (options_.requestCharacter && placeIdVerified_ && !characterRequested && !replica.markers.empty()) {
        RequestCharacterItem r;
        r.playerFlags = options_.playerFlags;
        r.appearance = options_.appearance;
        // The local Player: the first Player under Players the server gave us, if any.
        if (auto* players = replica.game() ? replica.game()->findService("Players") : nullptr)
            for (const auto& child : players->children())
                if (child->className() == "Player")
                    if (auto id = replica.idOf(child.get())) {
                        r.player = *id;
                        break;
                    }
        items.emplace_back(RuntimeItem{r});
        characterRequested = true;
    }
    // Our own ping, every interval (plus the periodic "special" one).
    if (options_.sendPings && (lastPingMs_ == 0 || nowMs - lastPingMs_ >= options_.pingIntervalMs)) {
        lastPingMs_ = nowMs;
        items.emplace_back(RuntimeItem{makePing(nowMs, false)});
        ++pingsSent;
        if (options_.specialPingEvery >= 0) {
            if (pingsSinceSpecial_ < options_.specialPingEvery) {
                ++pingsSinceSpecial_;
            } else {
                pingsSinceSpecial_ = 0;
                items.emplace_back(RuntimeItem{makePing(nowMs, false, 1)});
                ++pingsSent;
            }
        }
    }
    // The streaming quota once the server streams: the first update raises it
    // from 0 to the initial quota, with no limit on the radius (StreamingClient
    // keeps it up to date in a live session).
    if (options_.sendCapacityUpdate && !capacityUpdateSent && replica.runtime.streamDataItems > 0) {
        ClientCapacityUpdateItem c;
        c.quotaDelta = options_.initialQuota;
        c.maxRegionRadius = kUnlimitedRegionRadius;
        items.emplace_back(RuntimeItem{c});
        capacityUpdateSent = true;
    }
    flushItems();
    // Bytecode challenges, when something can run them.
    for (const BytecodeChallenge& c : pendingChallenges_)
        if (options_.challenge)
            if (auto result = options_.challenge(c)) {
                out.push_back(encodeBytecodeResponse({c.b, *result}));
                ++challengesAnswered;
            }
    pendingChallenges_.clear();
    (void)ctx;
    return out;
}

std::vector<std::uint8_t> ClientSession::chat(InstanceId player, std::string_view text, std::uint8_t id,
                                              InstanceId target)
{
    ChatMessage c;
    c.id = id;
    c.sender = player;
    c.target = target;
    c.text = std::string(text);
    c.flagA = 0;
    c.flagB = 0;
    return encodeChat(c);
}

// -- ServerSession

void ServerSession::onItem(const RuntimeItem& item, std::uint64_t nowMs)
{
    if (const auto* p = std::get_if<PingItem>(&item.body)) {
        due_.emplace_back(
            RuntimeItem{answerPing(*p, static_cast<std::uint32_t>(nowMs), static_cast<std::uint32_t>(nowMs))});
        ++pingBacksSent;
    } else if (const auto* b = std::get_if<PingBackItem>(&item.body)) {
        roundTripsMs.push_back(pingRoundTripMs(*b, static_cast<std::uint32_t>(nowMs)));
    } else if (const auto* r = std::get_if<RequestCharacterItem>(&item.body)) {
        characterRequests.push_back(*r);
    }
}

std::vector<RuntimeItem> ServerSession::take(std::uint64_t nowMs)
{
    if (sendPings_ && (lastPingMs_ == 0 || nowMs - lastPingMs_ >= 500)) {
        lastPingMs_ = nowMs;
        due_.emplace_back(RuntimeItem{makePing(nowMs, true)});
        ++pingsSent;
    }
    (void)capabilities_;
    return std::exchange(due_, {});
}

} // namespace lunar::net

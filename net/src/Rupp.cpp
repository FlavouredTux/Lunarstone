#include <lunar/net/Rupp.hpp>

#include <lunar/net/Describe.hpp>
#include <lunar/net/Socket.hpp>

#include <format>

namespace lunar::net {

std::size_t RuppHeader::length() const
{
    std::size_t n = 4;
    for (const RuppTlv& t : tlvs)
        n += 2 + t.value.size();
    return n;
}

std::vector<std::uint8_t> RuppHeader::encode() const
{
    std::vector<std::uint8_t> out;
    out.reserve(length());
    out.push_back(byte0);
    out.push_back(byte1);
    const std::size_t len = length();
    out.push_back(static_cast<std::uint8_t>(len >> 8));
    out.push_back(static_cast<std::uint8_t>(len));
    for (const RuppTlv& t : tlvs) {
        out.push_back(t.type);
        out.push_back(static_cast<std::uint8_t>(t.value.size()));
        out.insert(out.end(), t.value.begin(), t.value.end());
    }
    return out;
}

std::optional<RuppHeader> decodeRuppHeader(std::span<const std::uint8_t> data)
{
    if (data.size() < 4)
        return std::nullopt;
    const std::size_t len = static_cast<std::size_t>(data[2]) << 8 | data[3];
    if (len < 4 || len > data.size())
        return std::nullopt;
    RuppHeader h;
    h.byte0 = data[0];
    h.byte1 = data[1];
    std::size_t p = 4;
    while (p < len) {
        if (p + 2 > len)
            return std::nullopt;
        RuppTlv t;
        t.type = data[p];
        const std::size_t n = data[p + 1];
        if (p + 2 + n > len)
            return std::nullopt;
        t.value.assign(data.begin() + static_cast<std::ptrdiff_t>(p + 2),
                       data.begin() + static_cast<std::ptrdiff_t>(p + 2 + n));
        h.tlvs.push_back(std::move(t));
        p += 2 + n;
    }
    return h;
}

std::size_t ruppReplyLength(std::span<const std::uint8_t> data)
{
    // [live] the proxy answers with an empty header, 01 00 00 04, in front
    // of every datagram (offline and connected alike).
    if (data.size() < 4 || data[0] != 1)
        return 0;
    const auto h = decodeRuppHeader(data);
    return h && h->length() < data.size() ? h->length() : 0;
}

std::shared_ptr<RuppGenerator> RuppGenerator::fromJoinScript(const JoinScript& script)
{
    if (script.tokenValue.empty())
        return nullptr;
    auto g = std::make_shared<RuppGenerator>();
    g->tokenGenAlgorithm = script.tokenGenAlgorithm;
    g->pepperId = script.pepperId;
    g->tokenValue = decodeBase64(script.tokenValue).value_or(std::vector<std::uint8_t>{});
    if (auto a = resolve(std::format("{}:{}", script.machineAddress, script.serverPort)))
        g->server = *a;
    return g;
}

RuppHeader RuppGenerator::current(std::uint64_t nowMs)
{
    // [code+emu, 0x102430bb4 / 0x1024352e8]: byte0 = 1, byte1 = 0 (the
    // DirectServerReturn flag setter 0x10243060a masks it to 0); TLV 1 is the
    // token: u8 kind = 1, then the 16 decoded TokenValue bytes; TLV 2 is the
    // server behind the proxy: ipv4 in network order, u16 BE port. The
    // 0.742 client doesn't read TokenGenAlgorithm/PepperId at all.
    (void)nowMs;
    RuppHeader h;
    h.byte0 = 1;
    h.byte1 = flags;
    RuppTlv token;
    token.type = 1;
    token.value.push_back(tokenKind);
    token.value.insert(token.value.end(), tokenValue.begin(), tokenValue.end());
    h.tlvs.push_back(std::move(token));
    RuppTlv target;
    target.type = 2;
    target.value.assign(server.ip.begin(), server.ip.end());
    target.value.push_back(static_cast<std::uint8_t>(server.port >> 8));
    target.value.push_back(static_cast<std::uint8_t>(server.port));
    h.tlvs.push_back(std::move(target));
    if (reverseEndpoint) {
        RuppTlv rev;
        rev.type = 6;
        rev.value.assign(reverseEndpoint->ip.begin(), reverseEndpoint->ip.end());
        rev.value.push_back(static_cast<std::uint8_t>(reverseEndpoint->port >> 8));
        rev.value.push_back(static_cast<std::uint8_t>(reverseEndpoint->port));
        h.tlvs.push_back(std::move(rev));
    }
    if (traceId)
        h.tlvs.push_back({8, std::vector<std::uint8_t>(traceId->begin(), traceId->end())});
    return h;
}

std::vector<std::uint8_t> RuppGenerator::header(std::uint64_t nowMs)
{
    const RuppHeader h = current(nowMs);
    return h.tlvs.empty() ? std::vector<std::uint8_t>{} : h.encode();
}

bool RuppGenerator::onReply(std::span<const std::uint8_t> header)
{
    const auto h = decodeRuppHeader(header);
    if (!h)
        return false;
    for (const RuppTlv& t : h->tlvs)
        if (t.type == 1 && t.value.size() == 17 && t.value[0] != 0) {
            std::vector<std::uint8_t> token(t.value.begin() + 1, t.value.end());
            if (token == tokenValue && t.value[0] == tokenKind)
                return false;
            // [live] the 0.741 client sends the reply's kind (2) with its token.
            tokenKind = t.value[0];
            tokenValue = std::move(token);
            ++tokenUpdates;
            return true;
        }
    return false;
}

std::string RuppGenerator::describe() const
{
    return std::format("algorithm {}, pepper {}, token {} bytes, server {}", tokenGenAlgorithm, pepperId,
                       tokenValue.size(), server.toString());
}

} // namespace lunar::net

#include <lunar/net/Capture.hpp>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>

namespace lunar::net {

namespace {

struct Reader {
    const std::vector<std::uint8_t>& d;
    bool swap = false;

    std::uint16_t u16(std::size_t at) const
    {
        const auto v = static_cast<std::uint16_t>(d[at] | d[at + 1] << 8);
        return swap ? static_cast<std::uint16_t>(v >> 8 | v << 8) : v;
    }
    std::uint32_t u32(std::size_t at) const
    {
        const std::uint32_t v = static_cast<std::uint32_t>(d[at]) | static_cast<std::uint32_t>(d[at + 1]) << 8
                                | static_cast<std::uint32_t>(d[at + 2]) << 16
                                | static_cast<std::uint32_t>(d[at + 3]) << 24;
        return swap ? __builtin_bswap32(v) : v;
    }
};

std::uint16_t be16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
}

// A link-layer frame to a UDP datagram, when it is one.
bool fromFrame(std::uint32_t linkType, const std::uint8_t* p, std::size_t n, CapturedDatagram& out)
{
    std::size_t off = 0;
    switch (linkType) {
    case 1: // Ethernet
        if (n < 14)
            return false;
        off = 14;
        if (be16(p + 12) == 0x8100)
            off = 18;
        else if (be16(p + 12) != 0x0800)
            return false;
        break;
    case 101: // raw
    case 228: // raw IPv4
        off = 0;
        break;
    case 113: // Linux SLL
        if (n < 16 || be16(p + 14) != 0x0800)
            return false;
        off = 16;
        break;
    case 276: // Linux SLL2
        if (n < 20 || be16(p) != 0x0800)
            return false;
        off = 20;
        break;
    case 0: // BSD loopback: host-order family
        if (n < 4 || (p[0] != 2 && p[3] != 2))
            return false;
        off = 4;
        break;
    default:
        return false;
    }
    if (n < off + 20 || (p[off] >> 4) != 4)
        return false;
    const std::size_t ihl = static_cast<std::size_t>(p[off] & 0xf) * 4;
    if (p[off + 9] != 17 || n < off + ihl + 8)
        return false;
    const std::uint8_t* ip = p + off;
    const std::uint8_t* udp = ip + ihl;
    std::memcpy(out.from.ip.data(), ip + 12, 4);
    std::memcpy(out.to.ip.data(), ip + 16, 4);
    out.from.port = be16(udp);
    out.to.port = be16(udp + 2);
    const std::size_t udpLen = be16(udp + 4);
    const std::size_t avail = n - off - ihl;
    const std::size_t len = std::min(udpLen, avail);
    if (len < 8)
        return false;
    out.payload.assign(udp + 8, udp + len);
    return true;
}

std::expected<std::vector<CapturedDatagram>, std::string> readPcap(const std::vector<std::uint8_t>& d)
{
    Reader r{d};
    const std::uint32_t magic = r.u32(0);
    bool nano = false;
    if (magic == 0xa1b2c3d4 || magic == 0xa1b23c4d) {
        nano = magic == 0xa1b23c4d;
    } else if (magic == 0xd4c3b2a1 || magic == 0x4d3cb2a1) {
        r.swap = true;
        nano = magic == 0x4d3cb2a1;
    }
    const std::uint32_t link = r.u32(20);
    std::vector<CapturedDatagram> out;
    for (std::size_t at = 24; at + 16 <= d.size();) {
        const std::uint32_t sec = r.u32(at), frac = r.u32(at + 4), incl = r.u32(at + 8);
        if (at + 16 + incl > d.size())
            break;
        CapturedDatagram c;
        c.time = sec + frac / (nano ? 1e9 : 1e6);
        if (fromFrame(link, d.data() + at + 16, incl, c))
            out.push_back(std::move(c));
        at += 16 + incl;
    }
    return out;
}

std::expected<std::vector<CapturedDatagram>, std::string> readPcapng(const std::vector<std::uint8_t>& d)
{
    Reader r{d};
    std::vector<std::uint32_t> links;
    std::vector<double> resolution;
    std::vector<CapturedDatagram> out;
    for (std::size_t at = 0; at + 12 <= d.size();) {
        const std::uint32_t type = r.u32(at);
        if (type == 0x0a0d0d0a) { // section header: byte order magic decides
            r.swap = false;
            if (r.u32(at + 8) != 0x1a2b3c4d)
                r.swap = true;
            links.clear();
            resolution.clear();
        }
        const std::uint32_t len = r.u32(at + 4);
        if (len < 12 || at + len > d.size())
            break;
        if (type == 1) { // interface description
            links.push_back(r.u16(at + 8));
            double res = 1e-6;
            for (std::size_t o = at + 16; o + 4 <= at + len - 4;) {
                const std::uint16_t code = r.u16(o), olen = r.u16(o + 2);
                if (code == 0)
                    break;
                if (code == 9 && olen >= 1) {
                    const std::uint8_t v = d[o + 4];
                    res = (v & 0x80) ? std::ldexp(1.0, -(v & 0x7f)) : std::pow(10.0, -static_cast<int>(v));
                }
                o += 4 + ((olen + 3u) & ~3u);
            }
            resolution.push_back(res);
        } else if (type == 6) { // enhanced packet
            const std::uint32_t iface = r.u32(at + 8);
            const std::uint64_t ts = static_cast<std::uint64_t>(r.u32(at + 12)) << 32 | r.u32(at + 16);
            const std::uint32_t caplen = r.u32(at + 20);
            if (iface < links.size() && at + 28 + caplen <= d.size()) {
                CapturedDatagram c;
                c.time = static_cast<double>(ts) * resolution[iface];
                if (fromFrame(links[iface], d.data() + at + 28, caplen, c))
                    out.push_back(std::move(c));
            }
        } else if (type == 3 && !links.empty()) { // simple packet
            const std::uint32_t orig = r.u32(at + 8);
            CapturedDatagram c;
            if (fromFrame(links[0], d.data() + at + 12, std::min<std::size_t>(orig, len - 16), c))
                out.push_back(std::move(c));
        }
        at += len;
    }
    return out;
}

std::expected<std::vector<CapturedDatagram>, std::string> readHexDump(const std::vector<std::uint8_t>& d)
{
    std::istringstream text(std::string(d.begin(), d.end()));
    std::string line;
    std::vector<CapturedDatagram> out;
    const SystemAddress client{{127, 0, 0, 2}, 50000}, server{{127, 0, 0, 1}, 53640};
    double t = 0.0;
    while (std::getline(text, line)) {
        std::size_t i = 0;
        while (i < line.size() && line[i] == ' ')
            ++i;
        if (i >= line.size() || line[i] == '#')
            continue;
        bool toServer = true;
        if (line[i] == '>' || line[i] == '<') {
            toServer = line[i] == '>';
            ++i;
        }
        CapturedDatagram c;
        std::string hex;
        for (; i < line.size(); ++i)
            if (std::isxdigit(static_cast<unsigned char>(line[i])))
                hex += line[i];
        for (std::size_t j = 0; j + 1 < hex.size(); j += 2)
            c.payload.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(j, 2), nullptr, 16)));
        if (c.payload.empty())
            continue;
        c.from = toServer ? client : server;
        c.to = toServer ? server : client;
        c.time = t;
        t += 0.001;
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace

std::expected<std::vector<CapturedDatagram>, std::string> readCapture(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return std::unexpected(std::format("can't open {}", path.string()));
    // At its size in one read, not a character at a time.
    std::vector<std::uint8_t> d;
    if (const std::streamoff size = f.tellg(); size >= 0) {
        d.resize(static_cast<std::size_t>(size));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(d.data()), size);
        d.resize(static_cast<std::size_t>(f.gcount()));
    } else {
        f.clear();
        d.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (d.size() >= 24) {
        Reader r{d};
        const std::uint32_t magic = r.u32(0);
        if (magic == 0xa1b2c3d4 || magic == 0xd4c3b2a1 || magic == 0xa1b23c4d || magic == 0x4d3cb2a1)
            return readPcap(d);
        if (magic == 0x0a0d0d0a)
            return readPcapng(d);
    }
    return readHexDump(d);
}

PcapWriter::PcapWriter(const std::filesystem::path& path) : file_(std::fopen(path.c_str(), "wb"))
{
    if (!file_)
        return;
    const std::uint32_t header[6] = {0xa1b2c3d4, 0x00040002, 0, 0, 65535, 101};
    std::fwrite(header, sizeof header, 1, file_);
}

PcapWriter::~PcapWriter()
{
    if (file_)
        std::fclose(file_);
}

void PcapWriter::write(const SystemAddress& from, const SystemAddress& to, std::span<const std::uint8_t> payload)
{
    if (!file_)
        return;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    const std::size_t total = 20 + 8 + payload.size();
    const std::uint32_t rec[4] = {static_cast<std::uint32_t>(us / 1000000), static_cast<std::uint32_t>(us % 1000000),
                                  static_cast<std::uint32_t>(total), static_cast<std::uint32_t>(total)};
    std::fwrite(rec, sizeof rec, 1, file_);
    std::uint8_t ip[20]
        = {0x45, 0, static_cast<std::uint8_t>(total >> 8), static_cast<std::uint8_t>(total), 0, 0, 0x40, 0, 64, 17};
    std::memcpy(ip + 12, from.ip.data(), 4);
    std::memcpy(ip + 16, to.ip.data(), 4);
    std::uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2)
        sum += static_cast<std::uint32_t>(ip[i] << 8 | ip[i + 1]);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    ip[10] = static_cast<std::uint8_t>(~sum >> 8);
    ip[11] = static_cast<std::uint8_t>(~sum);
    const std::size_t udpLen = 8 + payload.size();
    const std::uint8_t udp[8] = {static_cast<std::uint8_t>(from.port >> 8),
                                 static_cast<std::uint8_t>(from.port),
                                 static_cast<std::uint8_t>(to.port >> 8),
                                 static_cast<std::uint8_t>(to.port),
                                 static_cast<std::uint8_t>(udpLen >> 8),
                                 static_cast<std::uint8_t>(udpLen),
                                 0,
                                 0};
    std::fwrite(ip, sizeof ip, 1, file_);
    std::fwrite(udp, sizeof udp, 1, file_);
    std::fwrite(payload.data(), 1, payload.size(), file_);
    std::fflush(file_);
}

} // namespace lunar::net

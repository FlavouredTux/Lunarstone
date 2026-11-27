#pragma once

#include <lunar/net/Offline.hpp>

#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace lunar::net {

// One UDP datagram from a capture.
struct CapturedDatagram {
    double time = 0.0; // seconds
    SystemAddress from;
    SystemAddress to;
    std::vector<std::uint8_t> payload;
};

// Reads the UDP datagrams of a capture: pcap (either byte order, micro- or
// nanosecond), pcapng, link types Ethernet / raw IPv4 / Linux cooked (SLL,
// SLL2) / BSD loopback, IPv4 only; or a text dump with one datagram per line
// as hex ("> " client to server, "< " server to client; addresses are made up).
std::expected<std::vector<CapturedDatagram>, std::string> readCapture(const std::filesystem::path& path);

// Writes a pcap (LINKTYPE_RAW, IPv4 + UDP headers made up around the payload)
// that Wireshark and --net-decode open.
class PcapWriter {
public:
    explicit PcapWriter(const std::filesystem::path& path);
    PcapWriter(const PcapWriter&) = delete;
    PcapWriter& operator=(const PcapWriter&) = delete;
    ~PcapWriter();

    void write(const SystemAddress& from, const SystemAddress& to, std::span<const std::uint8_t> payload);
    bool ok() const { return file_ != nullptr; }

private:
    std::FILE* file_ = nullptr;
};

} // namespace lunar::net

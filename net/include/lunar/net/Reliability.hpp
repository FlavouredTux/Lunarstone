#pragma once

#include <lunar/net/Datagram.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace lunar::net {

// RakNet's ReliabilityLayer for one connection, over plaintext datagrams (the
// Peer encrypts them): resends, ACK/NAK, ordering and sequencing channels,
// splitting and reassembly. The wire format is the client's (PROTOCOL.md 4);
// the behaviour follows RakNet 4 as Roblox ships it. GUESS: the timer values
// (RTO bounds, ACK cadence, the window), which the client takes from FInts
// the image doesn't hold.
class ReliabilityLayer {
public:
    struct Options {
        std::size_t mtu = 1492;
        HeaderOptions header;
        std::uint64_t minRtoMs = 100;  // GUESS (RakNetResendMinThresholdTimeInUs)
        std::uint64_t maxRtoMs = 2000; // GUESS (RakNetResendMaxThresholdTimeInUs)
        std::uint64_t initialRtoMs = 1000;
        std::size_t windowMessages = 4096; // GUESS: unacknowledged reliable messages in flight
        std::uint64_t timeoutMs = 10000;   // no traffic at all for this long: dead
    };

    ReliabilityLayer();
    explicit ReliabilityLayer(Options options);

    // Queues an application message.
    void send(std::vector<std::uint8_t> message, Reliability reliability, Priority priority = Priority::High,
              std::uint8_t channel = 0);

    // Feeds one received plaintext datagram; `parsed` gets what was in it
    // (for logging). Returns false on a malformed datagram.
    bool onDatagram(std::span<const std::uint8_t> plaintext, std::uint64_t nowMs, Datagram* parsed = nullptr);

    // Datagrams (plaintext) to put on the wire now: ACKs, NAKs, resends due,
    // then new data, as much as the window allows.
    std::vector<std::vector<std::uint8_t>> flush(std::uint64_t nowMs);

    // The next complete, in-order message, if any.
    std::optional<std::vector<std::uint8_t>> receive();

    bool idle() const { return outgoing_.empty() && resend_.empty() && acks_.empty() && naks_.empty(); }
    bool dead(std::uint64_t nowMs) const { return lastReceiveMs_ != 0 && nowMs - lastReceiveMs_ > options_.timeoutMs; }
    // 0 until the first sample.
    std::uint64_t rttMs() const { return srtt_ < 0 ? 0 : static_cast<std::uint64_t>(srtt_); }

    struct Stats {
        std::uint64_t datagramsSent = 0, datagramsReceived = 0, resends = 0, acksReceived = 0, naksReceived = 0;
        std::uint64_t messagesSent = 0, messagesReceived = 0, duplicates = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    struct Outgoing {
        InternalPacket packet;
        Priority priority = Priority::High;
    };
    struct InFlight {
        InternalPacket packet;
        std::uint64_t sentMs = 0;
        std::uint64_t nextResendMs = 0;
        int sends = 0;
    };
    struct SplitAssembly {
        std::uint32_t count = 0;
        std::uint32_t received = 0;
        std::vector<std::optional<InternalPacket>> pieces;
    };
    struct Channel {
        std::uint32_t orderedReadIndex = 0;
        std::uint64_t orderedReadCount = 0; // orderedReadIndex without the 24-bit wrap
        std::uint32_t highestSequencedReadIndex = 0;
        std::map<std::pair<std::uint64_t, std::uint32_t>, InternalPacket> heap; // (unwrapped index, sequencing)
    };

    void deliver(InternalPacket packet);
    void deliverOrdered(InternalPacket packet);
    void onAck(const Ranges& ranges, std::uint64_t nowMs);
    void onNak(const Ranges& ranges, std::uint64_t nowMs);
    std::uint64_t rto() const;
    std::size_t datagramBudget() const;
    void queue(InternalPacket packet, Priority priority);

    Options options_;
    // send side
    std::uint32_t nextReliable_ = 0;
    std::uint32_t nextDatagram_ = 0;
    std::uint16_t nextSplitId_ = 0;
    std::array<std::uint32_t, kOrderingChannels> orderedWrite_{};
    std::array<std::uint32_t, kOrderingChannels> sequencedWrite_{};
    std::deque<Outgoing> outgoing_;
    std::map<std::uint32_t, InFlight> resend_;                              // by reliable message number
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> history_; // datagram -> reliable numbers
    std::deque<std::uint32_t> historyOrder_;
    std::unordered_map<std::uint32_t, std::uint64_t> datagramSentMs_;
    // receive side
    std::vector<std::uint32_t> acks_, naks_;
    std::uint32_t expectedDatagram_ = 0;
    std::uint32_t reliableBase_ = 0;   // every reliable number below this was received
    std::vector<bool> reliableWindow_; // received flags for reliableBase_ + i
    std::unordered_map<std::uint16_t, SplitAssembly> splits_;
    std::array<Channel, kOrderingChannels> channels_{};
    std::deque<std::vector<std::uint8_t>> received_;
    // timing
    double srtt_ = -1.0, rttvar_ = 0.0;
    std::uint64_t lastReceiveMs_ = 0;
    Stats stats_;
};

} // namespace lunar::net

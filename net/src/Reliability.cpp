#include <lunar/net/Reliability.hpp>

#include <lunar/net/Crypto.hpp>
#include <lunar/net/Offline.hpp>

#include <algorithm>
#include <cmath>

namespace lunar::net {

namespace {

constexpr std::uint32_t kMask24 = 0xffffff;

// Distance from `from` forward to `to` in 24-bit sequence space.
std::uint32_t ahead24(std::uint32_t from, std::uint32_t to)
{
    return (to - from) & kMask24;
}

bool newer24(std::uint32_t a, std::uint32_t b) // a is after b
{
    const std::uint32_t d = ahead24(b, a);
    return d != 0 && d < 0x800000;
}

} // namespace

ReliabilityLayer::ReliabilityLayer() : ReliabilityLayer(Options{}) {}

ReliabilityLayer::ReliabilityLayer(Options options) : options_(options) {}

std::size_t ReliabilityLayer::datagramBudget() const
{
    // MTU minus IP/UDP (the client's 40), the 18 crypto bytes and room for
    // the datagram header.
    const std::size_t overhead = kMtuOverhead + kDatagramOverhead + 6;
    return options_.mtu > overhead + 64 ? options_.mtu - overhead : 64;
}

void ReliabilityLayer::send(std::vector<std::uint8_t> message, Reliability reliability, Priority priority,
                            std::uint8_t channel)
{
    if (message.empty())
        return;
    channel = static_cast<std::uint8_t>(channel % kOrderingChannels);
    // Ack receipts aren't tracked here; send the plain kind.
    if (reliability == Reliability::UnreliableWithAckReceipt)
        reliability = Reliability::Unreliable;
    else if (reliability == Reliability::ReliableWithAckReceipt)
        reliability = Reliability::Reliable;
    else if (reliability == Reliability::ReliableOrderedWithAckReceipt)
        reliability = Reliability::ReliableOrdered;

    InternalPacket base;
    base.reliability = reliability;
    base.orderingChannel = channel;
    if (isSequenced(reliability)) {
        base.orderingIndex = orderedWrite_[channel];
        base.sequencingIndex = sequencedWrite_[channel]++ & kMask24;
    } else if (isOrderedOrSequenced(reliability)) {
        base.orderingIndex = orderedWrite_[channel]++ & kMask24;
        orderedWrite_[channel] &= kMask24;
        sequencedWrite_[channel] = 0;
    }
    ++stats_.messagesSent;

    const std::size_t maxHeader = 1 + 2 + 3 + 3 + 4 + 10;
    const std::size_t room = datagramBudget() - maxHeader;
    if (message.size() <= room) {
        base.bitLength = message.size() * 8;
        base.payload = std::move(message);
        queue(std::move(base), priority);
        return;
    }
    // Split: every piece is reliable (RakNet upgrades unreliable splits).
    if (base.reliability == Reliability::Unreliable)
        base.reliability = Reliability::Reliable;
    else if (base.reliability == Reliability::UnreliableSequenced)
        base.reliability = Reliability::ReliableSequenced;
    const std::size_t count = (message.size() + room - 1) / room;
    const std::uint16_t id = nextSplitId_++;
    for (std::size_t i = 0; i < count; ++i) {
        InternalPacket piece = base;
        piece.split = true;
        piece.splitPacketId = id;
        piece.splitPacketCount = static_cast<std::uint32_t>(count);
        piece.splitPacketIndex = static_cast<std::uint32_t>(i);
        const std::size_t from = i * room, to = std::min(message.size(), from + room);
        piece.payload.assign(message.begin() + static_cast<std::ptrdiff_t>(from),
                             message.begin() + static_cast<std::ptrdiff_t>(to));
        piece.bitLength = piece.payload.size() * 8;
        queue(std::move(piece), priority);
    }
}

// Behind what it doesn't come before: the queue stays in priority order, first
// come first served within one, without a sort of all of it each flush. A big
// message is a piece a datagram [live: sUNC's 2 GB decoy remote was a million
// and a half; sorting them each frame took a second a frame, and the server
// timed us out].
void ReliabilityLayer::queue(InternalPacket packet, Priority priority)
{
    if (outgoing_.empty() || outgoing_.back().priority <= priority) {
        outgoing_.push_back({std::move(packet), priority});
        return;
    }
    const auto at = std::ranges::upper_bound(outgoing_, priority, {}, &Outgoing::priority);
    outgoing_.insert(at, {std::move(packet), priority});
}

std::uint64_t ReliabilityLayer::rto() const
{
    if (srtt_ < 0)
        return options_.initialRtoMs;
    const double r = 2.0 * srtt_ + 4.0 * rttvar_;
    return std::clamp(static_cast<std::uint64_t>(r), options_.minRtoMs, options_.maxRtoMs);
}

void ReliabilityLayer::onAck(const Ranges& ranges, std::uint64_t nowMs)
{
    for (auto [lo, hi] : ranges) {
        for (std::uint32_t d = lo;; d = (d + 1) & kMask24) {
            if (auto sent = datagramSentMs_.find(d); sent != datagramSentMs_.end()) {
                const auto sample = static_cast<double>(nowMs - sent->second);
                if (srtt_ < 0) {
                    srtt_ = sample;
                    rttvar_ = sample / 2;
                } else {
                    rttvar_ = 0.75 * rttvar_ + 0.25 * std::abs(srtt_ - sample);
                    srtt_ = 0.875 * srtt_ + 0.125 * sample;
                }
                datagramSentMs_.erase(sent);
            }
            if (auto it = history_.find(d); it != history_.end()) {
                for (std::uint32_t r : it->second)
                    resend_.erase(r);
                history_.erase(it);
            }
            if (d == hi)
                break;
        }
    }
}

void ReliabilityLayer::onNak(const Ranges& ranges, std::uint64_t nowMs)
{
    for (auto [lo, hi] : ranges) {
        for (std::uint32_t d = lo;; d = (d + 1) & kMask24) {
            if (auto it = history_.find(d); it != history_.end()) {
                for (std::uint32_t r : it->second)
                    if (auto in = resend_.find(r); in != resend_.end())
                        in->second.nextResendMs = nowMs; // resend at the next flush
                history_.erase(it);
            }
            if (d == hi)
                break;
        }
    }
}

bool ReliabilityLayer::onDatagram(std::span<const std::uint8_t> plaintext, std::uint64_t nowMs, Datagram* parsed)
{
    Datagram d = parseDatagram(plaintext, options_.header);
    lastReceiveMs_ = nowMs;
    ++stats_.datagramsReceived;
    bool ok = d.error.empty();
    if (d.header.isAck) {
        ++stats_.acksReceived;
        onAck(d.ranges, nowMs);
    } else if (d.header.isNak) {
        ++stats_.naksReceived;
        onNak(d.ranges, nowMs);
    } else if (ok || !d.packets.empty()) {
        const std::uint32_t n = d.header.datagramNumber;
        acks_.push_back(n);
        // Holes before this datagram get NAKed (RakNet 4 does it at once).
        if (newer24(n, expectedDatagram_)) {
            const std::uint32_t gap = ahead24(expectedDatagram_, n);
            if (gap < 1024)
                for (std::uint32_t m = expectedDatagram_; m != n; m = (m + 1) & kMask24)
                    naks_.push_back(m);
        }
        if (!newer24(expectedDatagram_, (n + 1) & kMask24))
            expectedDatagram_ = (n + 1) & kMask24;
        for (InternalPacket& p : d.packets) {
            if (isReliable(p.reliability)) {
                const std::uint32_t off = ahead24(reliableBase_, p.reliableMessageNumber);
                if (off >= 0x800000) { // older than the window: a duplicate
                    ++stats_.duplicates;
                    continue;
                }
                if (off > 1u << 20) {
                    ok = false;
                    continue;
                }
                if (reliableWindow_.size() <= off)
                    reliableWindow_.resize(off + 1, false);
                if (reliableWindow_[off]) {
                    ++stats_.duplicates;
                    continue;
                }
                reliableWindow_[off] = true;
                std::size_t advance = 0;
                while (advance < reliableWindow_.size() && reliableWindow_[advance])
                    ++advance;
                reliableWindow_.erase(reliableWindow_.begin(),
                                      reliableWindow_.begin() + static_cast<std::ptrdiff_t>(advance));
                reliableBase_ = (reliableBase_ + static_cast<std::uint32_t>(advance)) & kMask24;
            }
            if (p.split) {
                SplitAssembly& s = splits_[p.splitPacketId];
                if (s.pieces.empty()) {
                    s.count = p.splitPacketCount;
                    s.pieces.resize(s.count);
                }
                if (p.splitPacketCount != s.count || p.splitPacketIndex >= s.count || s.pieces[p.splitPacketIndex])
                    continue;
                const std::uint16_t id = p.splitPacketId;
                s.pieces[p.splitPacketIndex] = std::move(p);
                if (++s.received < s.count)
                    continue;
                InternalPacket whole;
                bool first = true;
                for (const auto& piece : s.pieces) {
                    if (!piece)
                        continue;
                    if (first) {
                        whole = *piece; // header fields of the whole message
                        whole.split = false;
                        whole.payload.clear();
                        whole.bitLength = 0;
                        first = false;
                    }
                    whole.payload.insert(whole.payload.end(), piece->payload.begin(), piece->payload.end());
                    whole.bitLength += piece->bitLength;
                }
                splits_.erase(id);
                deliver(std::move(whole));
                continue;
            }
            deliver(std::move(p));
        }
    }
    if (parsed)
        *parsed = std::move(d);
    return ok;
}

void ReliabilityLayer::deliver(InternalPacket packet)
{
    if (!isOrderedOrSequenced(packet.reliability)) {
        ++stats_.messagesReceived;
        received_.push_back(std::move(packet.payload));
        return;
    }
    deliverOrdered(std::move(packet));
}

void ReliabilityLayer::deliverOrdered(InternalPacket packet)
{
    Channel& ch = channels_[packet.orderingChannel % kOrderingChannels];
    const bool sequenced = isSequenced(packet.reliability);
    if (packet.orderingIndex == ch.orderedReadIndex) {
        if (sequenced) {
            if (ahead24(ch.highestSequencedReadIndex, packet.sequencingIndex) >= 0x800000)
                return; // older than what was already delivered
            ch.highestSequencedReadIndex = (packet.sequencingIndex + 1) & kMask24;
            ++stats_.messagesReceived;
            received_.push_back(std::move(packet.payload));
            return;
        }
        ++stats_.messagesReceived;
        received_.push_back(std::move(packet.payload));
        ch.orderedReadIndex = (ch.orderedReadIndex + 1) & kMask24;
        ++ch.orderedReadCount;
        ch.highestSequencedReadIndex = 0;
        // Everything held for the indices that are now current.
        while (!ch.heap.empty()) {
            auto it = ch.heap.begin();
            InternalPacket& next = it->second;
            if (next.orderingIndex != ch.orderedReadIndex) {
                if (newer24(next.orderingIndex, ch.orderedReadIndex))
                    break;
                ch.heap.erase(it); // stale
                continue;
            }
            InternalPacket held = std::move(next);
            ch.heap.erase(it);
            if (isSequenced(held.reliability)) {
                if (ahead24(ch.highestSequencedReadIndex, held.sequencingIndex) < 0x800000) {
                    ch.highestSequencedReadIndex = (held.sequencingIndex + 1) & kMask24;
                    ++stats_.messagesReceived;
                    received_.push_back(std::move(held.payload));
                }
            } else {
                ++stats_.messagesReceived;
                received_.push_back(std::move(held.payload));
                ch.orderedReadIndex = (ch.orderedReadIndex + 1) & kMask24;
                ++ch.orderedReadCount;
                ch.highestSequencedReadIndex = 0;
            }
        }
        return;
    }
    if (!newer24(packet.orderingIndex, ch.orderedReadIndex))
        return; // older ordering index: drop (RakNet does too)
    // Held until its turn: ordered ones sort after the sequenced ones of the same index.
    const std::pair key{ch.orderedReadCount + ahead24(ch.orderedReadIndex, packet.orderingIndex),
                        sequenced ? packet.sequencingIndex : 0xffffffffu};
    ch.heap.emplace(key, std::move(packet));
}

std::optional<std::vector<std::uint8_t>> ReliabilityLayer::receive()
{
    if (received_.empty())
        return std::nullopt;
    std::vector<std::uint8_t> m = std::move(received_.front());
    received_.pop_front();
    return m;
}

std::vector<std::vector<std::uint8_t>> ReliabilityLayer::flush(std::uint64_t nowMs)
{
    std::vector<std::vector<std::uint8_t>> out;
    const std::size_t budgetBits = datagramBudget() * 8;
    auto emitRanges = [&](std::vector<std::uint32_t>& numbers, bool ack) {
        if (numbers.empty())
            return;
        Ranges ranges = toRanges(std::move(numbers));
        numbers.clear();
        while (!ranges.empty()) {
            BitStream bs;
            DatagramHeader h;
            h.isAck = ack;
            h.isNak = !ack;
            h.write(bs, options_.header);
            writeRanges(bs, ranges, budgetBits - bs.bitsUsed());
            out.push_back(std::move(bs).take());
        }
    };
    emitRanges(acks_, true);
    emitRanges(naks_, false);

    // Packets for this round: due resends first, then new ones by priority.
    std::vector<InternalPacket*> resends;
    for (auto& [num, f] : resend_)
        if (f.nextResendMs <= nowMs)
            resends.push_back(&f.packet);

    // The window counts unacknowledged reliable messages (GUESS: the client's
    // sliding window counts bytes in flight).
    std::size_t ri = 0;
    while (ri < resends.size() || (!outgoing_.empty() && resend_.size() < options_.windowMessages)) {
        BitStream bs;
        DatagramHeader h;
        h.datagramNumber = nextDatagram_;
        h.isContinuousSend = false;
        // Header first with the final flags unknown: build packets, then the header.
        std::vector<InternalPacket> packs;
        std::size_t bytes = 4 + (options_.header.trailingU16 ? 2 : 0);
        auto fits = [&](const InternalPacket& p) {
            return bytes + p.headerBytes() + p.payload.size() <= datagramBudget() || packs.empty();
        };
        while (ri < resends.size() && fits(*resends[ri])) {
            bytes += resends[ri]->headerBytes() + resends[ri]->payload.size();
            packs.push_back(*resends[ri]);
            ++ri;
            ++stats_.resends;
        }
        while (!outgoing_.empty() && fits(outgoing_.front().packet)) {
            Outgoing o = std::move(outgoing_.front());
            outgoing_.pop_front();
            if (isReliable(o.packet.reliability))
                o.packet.reliableMessageNumber = nextReliable_++ & kMask24;
            if (o.priority >= Priority::Medium)
                h.flagB = true;
            bytes += o.packet.headerBytes() + o.packet.payload.size();
            packs.push_back(std::move(o.packet));
        }
        if (packs.empty())
            break;
        h.write(bs, options_.header);
        std::vector<std::uint32_t> reliables;
        for (InternalPacket& p : packs) {
            p.write(bs, options_.header.tracer);
            if (isReliable(p.reliability)) {
                reliables.push_back(p.reliableMessageNumber);
                InFlight& f = resend_[p.reliableMessageNumber];
                if (f.sends == 0)
                    f.packet = p;
                f.sentMs = nowMs;
                f.nextResendMs = nowMs + rto() * static_cast<std::uint64_t>(1 + std::min(f.sends, 4));
                ++f.sends;
            }
        }
        if (!reliables.empty()) {
            history_[nextDatagram_] = std::move(reliables);
            historyOrder_.push_back(nextDatagram_);
        }
        datagramSentMs_[nextDatagram_] = nowMs;
        nextDatagram_ = (nextDatagram_ + 1) & kMask24;
        ++stats_.datagramsSent;
        out.push_back(std::move(bs).take());
    }
    // Datagram records whose messages were all acknowledged some other way
    // (a resend got through) go; so do very old ones.
    while (!historyOrder_.empty()) {
        auto it = history_.find(historyOrder_.front());
        const bool live = it != history_.end()
                          && std::ranges::any_of(it->second, [&](std::uint32_t r) { return resend_.contains(r); });
        if (live && historyOrder_.size() <= 65536)
            break;
        if (it != history_.end())
            history_.erase(it);
        historyOrder_.pop_front();
    }
    if (datagramSentMs_.size() > 8192)
        datagramSentMs_.clear();
    return out;
}

} // namespace lunar::net

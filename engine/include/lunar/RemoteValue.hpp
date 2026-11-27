#pragma once

#include <lunar/Value.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lunar {

// What remotes carry over the network: a Value, or a table of them. Roblox
// sends a Luau table as an array (its members 1..n in order) or as a
// dictionary (string keys); metatables and table identity don't make the trip.
struct RemoteTable;
struct RemoteValue {
    Value value;                              // nil for a table or a Random
    std::shared_ptr<const RemoteTable> table; // set for a table
    // Set for a Random, which goes as its generator's 64-bit state: what the
    // receiver gets goes on from where the sender's was. The game hands its
    // clients one to flicker and shatter a room's lights the same way on
    // each (Module_Events), and called NextInteger on the number it was.
    std::optional<std::uint64_t> random;
};

struct RemoteTable {
    bool dictionary = false;
    std::vector<std::pair<std::string, RemoteValue>> members; // keys are empty in an array
};

inline RemoteValue remoteValue(Value value)
{
    return RemoteValue{std::move(value), nullptr};
}

} // namespace lunar

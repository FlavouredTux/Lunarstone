#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace lunar {
class ClientSession;
}

// --equip <slot> [--activate] [--unequip] [--tool-at <seconds>] (--net-join
// --scripts, --net-replay --run): a headless client's hands on its tools.
// Once the scripts have run `at` seconds and the slot (1-10, the hotbar's
// numbering) holds a tool, it's equipped as hotbar key `slot` would; three
// seconds later a left click (press, release 0.2 s on) activates it, and
// three seconds after that it's put away again.
struct ToolActions {
    int slot = 0; // 0: none
    bool activate = false;
    bool unequip = false;
    double at = 5.0;

    // True when args[i] was one of these (i past its value); error says what's wrong with a bad value.
    bool parse(std::span<const std::string_view> args, std::size_t& i, std::string& error);
    bool any() const { return slot > 0; }
    // Called every frame with the scripts' clock.
    void step(lunar::ClientSession& client, double scriptTime, const std::function<void(std::string)>& note);

private:
    double equippedAt_ = -1;
    int stage_ = 0;
};

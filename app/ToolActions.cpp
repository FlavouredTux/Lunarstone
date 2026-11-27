#include "ToolActions.hpp"

#include <lunar/Classes.hpp>
#include <lunar/script/PlaySession.hpp>

#include <format>

bool ToolActions::parse(std::span<const std::string_view> args, std::size_t& i, std::string& error)
{
    const std::string_view arg = args[i];
    if (arg == "--activate") {
        activate = true;
        return true;
    }
    if (arg == "--unequip") {
        unequip = true;
        return true;
    }
    if ((arg != "--equip" && arg != "--tool-at") || i + 1 >= args.size())
        return false;
    const std::string value(args[++i]);
    try {
        if (arg == "--equip") {
            slot = std::stoi(value);
            if (slot < 1 || slot > lunar::Hotbar::slotCount)
                error = std::format("--equip takes a hotbar slot, 1 to {}", lunar::Hotbar::slotCount);
        } else {
            at = std::stod(value);
        }
    } catch (const std::exception&) {
        error = std::format("{} {}: not a number", arg, value);
    }
    return true;
}

void ToolActions::step(lunar::ClientSession& client, double scriptTime, const std::function<void(std::string)>& note)
{
    if (slot <= 0 || stage_ == 4 || scriptTime < at)
        return;
    if (stage_ == 0) {
        if (!client.selectTool(slot - 1))
            return; // the slot fills once the Backpack has the tools
        const lunar::Tool* tool = client.hotbar().slot(slot - 1);
        note(std::format("tools: equipped slot {} ({}) at {:.1f} s", slot, tool ? tool->name() : "?", scriptTime));
        equippedAt_ = scriptTime;
        stage_ = activate ? 1 : unequip ? 3 : 4;
        return;
    }
    const double since = scriptTime - equippedAt_;
    if (stage_ == 1 && since >= 3.0) {
        client.toolButton(true);
        note(std::format("tools: left button down at {:.1f} s", scriptTime));
        stage_ = 2;
    } else if (stage_ == 2 && since >= 3.2) {
        client.toolButton(false);
        note(std::format("tools: left button up at {:.1f} s", scriptTime));
        stage_ = unequip ? 3 : 4;
    } else if (stage_ == 3 && since >= (activate ? 6.0 : 3.0)) {
        client.unequipTools();
        note(std::format("tools: put away at {:.1f} s", scriptTime));
        stage_ = 4;
    }
}

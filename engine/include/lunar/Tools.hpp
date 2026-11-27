#pragma once

// What the engine does with Tools, after the 0.742 client's RBX::Tool: a
// tool in a character with a Humanoid gets welded into the right hand and
// equipped, one lying in the Workspace is picked up by the character that
// touches its Handle, and Humanoid:EquipTool, UnequipTools, the hotbar and
// the drop key move tools between the character and the Backpack.

#include <lunar/Classes.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lunar {

// The states a Tool steps through, one at a time (Tool::setBackendToolState
// in the binary). Each step up does that state's work and each step down
// undoes it, so a tool always unwinds through Unequipped and the weld.
enum class ToolState : std::uint8_t {
    None,        // outside the Workspace, or RequiresHandle without a Handle
    Loose,       // passing through on the way to InWorkspace
    InWorkspace, // in the Workspace, not held by a Humanoid: its Handle picks it up on touch
    InCharacter, // in a model with a Humanoid, which has no torso (RequiresHandle)
    Gripped,     // RightGrip welds the Handle to the right hand
    Equipped,    // Equipped has fired; Activate works
};

// The hand a Humanoid holds tools in: RightHand for R15, Right Arm for R6.
BasePart* gripLimb(const Humanoid& humanoid);
// RightGrip's C0: the character's RightGripAttachment for R15 (0.2 below the
// hand's centre without one), one stud below the arm's centre for R6; both
// turned so the tool points forward.
CFrame gripOffset(const Humanoid& humanoid);
// The RightGrip weld in the hand, if the character holds something.
Weld* rightGrip(const Humanoid& humanoid);

// The player whose Character is `character`, if any.
Player* playerFromCharacter(const DataModel& game, const Instance& character);

// Humanoid:UnequipTools: every Tool in the character goes to its player's
// Backpack. False when there's no Backpack to put them in (the client warns
// "UnequipTools could not find Player backpack!").
bool unequipTools(const DataModel& game, Humanoid& humanoid);
// Humanoid:EquipTool: unequips the others, then parents the tool to the character.
void equipTool(const DataModel& game, Humanoid& humanoid, Tool& tool);
// The drop key (Backspace): every tool the player's character holds that
// CanBeDropped moves to the Workspace.
void dropTools(const DataModel& game, Player& player);

// The backpack hotbar's slots (CoreScripts BackpackScript, "Backpack Version
// 5.1"): the local player's tools in up to ten slots, kept by looking at the
// character and the Backpack (OnCharacterAdded / OnChildAdded /
// OnChildRemoved). A new character starts over, its tools first, then the
// Backpack's, each in the lowest empty slot; tools moving between the two
// keep their slot, and ones leaving both lose it. The windowed client's
// BackpackGui draws it; headless joins (--equip) use it on its own.
class Hotbar {
public:
    static constexpr int slotCount = 10; // HOTBAR_SLOTS_FULL

    void sync(const DataModel& game);
    void clear();
    // The tool in a slot (0-based), if any, as of the last sync.
    Tool* slot(int index) const;
    // What clicking slot `index` or pressing its key does (after a sync):
    // equip its tool (EquipNewTool: everything put away, then this one into
    // the character), or put it away when it's the one in hand.
    void select(const DataModel& game, int index);

private:
    void add(Tool& tool);

    std::weak_ptr<Instance> character_; // whose tools the slots hold
    std::array<std::weak_ptr<Tool>, slotCount> slots_{};
};

// Watches every Tool in a game and runs its state machine as it moves.
class ToolSystem {
public:
    // Who does the equipping (Tool's ancestry handler, vfn_60 at 0x1043c4ee4).
    // Backend: a server, or Play Solo, which equips every tool and welds it.
    // Client: a client of a live server, where only the local player's
    // character's tools step up, and the RightGrip weld is made here only for
    // a tool that came in from the local player's Backpack (the binary's
    // Tool+0x488 flag); the server makes none for those and waits for the
    // client's, which replicates [code + capture: other players' RightGrips
    // arrive in their client's id scope]. Any other tool in the character
    // (one the server moved there from the Workspace) is welded by the server.
    enum class Mode : std::uint8_t { Backend, Client };

    explicit ToolSystem(DataModel& game);
    ~ToolSystem();
    ToolSystem(const ToolSystem&) = delete;
    ToolSystem& operator=(const ToolSystem&) = delete;

    // Equipped's argument, the player's Mouse; without one Equipped passes nil.
    std::function<InstancePtr(Player&)> mouse;

    Mode mode() const { return mode_; }
    // Switches modes, and every tool goes where the new mode puts it.
    void setMode(Mode mode);
    // Every tool looks again where it should be (the local player changed).
    void refreshAll();

    ToolState state(const Tool& tool) const;
    // Whether this side made the tool's RightGrip (and takes it away again).
    bool weldsLocally(const Tool& tool) const;

    // Humanoid:EquipTool (0x103639088): the other tools are put away, then
    // the tool goes into the character. A client can't take a tool lying in
    // the Workspace itself and asks the server with the Humanoid's
    // ServerEquipTool event instead (a replicated event the client may send,
    // list at 0x10579f2aa) [code].
    void equip(Humanoid& humanoid, Tool& tool);

    // Tool:Activate / Deactivate (fromScript) and the left mouse button over
    // the 3D view. False when the tool isn't equipped, which scripts hear
    // about as a warning. ManualActivationOnly only stops the mouse; a
    // disabled tool fires nothing either way.
    bool activate(Tool& tool, bool fromScript) const;
    bool deactivate(Tool& tool, bool fromScript) const;

    // A part touched a loose tool's Handle: a living character that a player
    // owns and that can hold it puts its tools away and equips this one.
    void touched(Tool& tool, BasePart& hit);
    // Handles overlapping a player character's parts count as touches. The
    // physics side reports no Touched events, so the frame calls this after
    // stepping physics.
    void pickUpTouching();

private:
    struct Entry {
        std::weak_ptr<Tool> tool;
        ToolState state = ToolState::None;
        std::weak_ptr<Instance> holder; // the character it's in, once InCharacter
        std::weak_ptr<Instance> weld;   // RightGrip
        std::weak_ptr<Instance> parent; // where the last refresh saw it
        bool local = true;              // this side welds it (Client mode: it came from our Backpack)
        bool madeWeld = false;          // `weld` is ours, not one found in the hand
        std::uint64_t generation = 0;   // bumped by every refresh
        std::vector<Connection> connections;
    };

    void track(Tool& tool);
    void refresh(Tool& tool);
    ToolState target(const Tool& tool) const;
    void stepUp(Entry& entry, Tool& tool, ToolState to);
    void stepDown(Entry& entry, Tool& tool, ToolState to);
    void grip(Entry& entry, Tool& tool);
    void ungrip(Entry& entry, Tool& tool);
    InstancePtr mouseFor(const Instance* character);
    Player* localPlayer() const;

    DataModel* game_;
    Mode mode_ = Mode::Backend;
    std::unordered_map<const Tool*, Entry> entries_;
    std::vector<Connection> connections_;
};

} // namespace lunar

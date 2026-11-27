#pragma once

// Our own character in a live game (--net-join --scripts --walk, the
// windowed live client): simulated here, as the client that owns it, and
// told to the server, which shows it to everyone else.
//
// The server makes our client the network owner of our character's root
// part, so nobody moves it but us: Lunarstone's physics (physics::World over
// the replica's Workspace, physics::HumanoidController for our Humanoid,
// what --play runs for its character) walks and jumps it from a move vector
// and the jump key, and net::CharacterSync sends its motion, its Humanoid's
// state and its Animator's tracks (the ones its Animate script plays, see
// LiveAnimation) the way the 0.742 client does. The parts the server hands
// us besides (a game side door, given to the player who opens it) are
// simulated here too, with what they're jointed or constrained to.
// Everything else in the Workspace is someone else's to move: the world
// holds it where the network puts it.
//
// Each frame, with a ClientSession running the scripts:
//
//     character.setMoveVector(wasd, /*relativeToCamera=*/true); // before the frame
//     character.setJump(spaceDown);
//     client.step(dt);                                        // runs step(dt) as its physics
//     for (auto& out : character.take(Peer::nowMs()))
//         peer.send(connection, std::move(out.message), out.reliability);
//
// Without a ClientSession, call step(dt) yourself between the two.

#include <lunar/Classes.hpp>
#include <lunar/avatar/AnimationSystem.hpp>
#include <lunar/avatar/ReplicatedAnimation.hpp>
#include <lunar/net/CharacterSync.hpp>
#include <lunar/physics/HumanoidController.hpp>
#include <lunar/physics/World.hpp>

#include "UnionShapes.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lunar {
class ClientSession;
}
namespace lunar::net {
class Replica;
}

class LiveCharacter {
public:

    // `animation` is where our Animator's tracks play (LiveAnimation::system()).
    LiveCharacter(std::shared_ptr<lunar::net::Replica> replica, lunar::avatar::AnimationSystem& animation);
    ~LiveCharacter();
    LiveCharacter(const LiveCharacter&) = delete;
    LiveCharacter& operator=(const LiveCharacter&) = delete;

    // Makes step(dt) the client frame's physics (between Stepped and
    // Heartbeat, where the 0.742 client simulates), keeping its other hooks.
    void attach(lunar::ClientSession& client);

    // The controls, held until changed: the ControlModule's move vector (a
    // unit-ish vector, x right and z back; camera-relative like WASD, or in
    // world space) and whether jump is held. The windowed client sets them
    // each frame from the keyboard.
    void setMoveVector(lunar::Vector3 direction, bool relativeToCamera = true);
    void setJump(bool held);
    // The way the character turns to face, the camera's look while its
    // rotation is camera-relative (Shift Lock, first person); none lets it
    // turn to where it walks (AutoRotate), as the CameraModule leaves it.
    void setFacing(std::optional<lunar::Vector3> direction);

    // The frame's physics: picks up our character (a new one after a
    // respawn, once its root part has streamed in), walks it and steps the
    // world by dt. A new character stands still until the ground under it
    // has streamed in (or kFloorWait passes): with StreamingEnabled the
    // character can come before the parts around its spawn [live: one fell
    // through the map that way]. GUESS: the client's own wait (the gameplay
    // pause of StreamingIntegrityMode) wasn't traced.
    void step(double dt);
    static constexpr double kFloorWait = 5.0;
    // Seconds without the server's motion for our root after which it's
    // ours to move again (LiveCharacter::step). GUESS.
    static constexpr double kServerLetGo = 1.0;

    // What to send the server since the last call: the root's motion at the
    // physics rate, the Humanoid's changes and the Animator's. `nowMs` is the
    // connection's clock (Peer::nowMs), which the physics timestamps use.
    std::vector<lunar::net::CharacterSync::Outgoing> take(std::uint64_t nowMs);

    lunar::Model* character() const { return character_.lock().get(); }
    // Our world of the Workspace (there from attach), for raycasts.
    const lunar::physics::World* world() const { return world_.get(); }
    lunar::Humanoid* humanoid() const { return controller_ ? &controller_->humanoid() : nullptr; }
    // Where the character was when we took it over (where the server spawned it).
    std::optional<lunar::Vector3> spawnPosition() const { return spawn_; }
    const lunar::net::CharacterSync& sync() const { return sync_; }
    // One line: where the root is, the state, what went out.
    std::string describe() const;

private:
    void adopt(lunar::Model& character);
    void ensureWorld();
    void simulateOnly(lunar::Model& character);
    bool simulates(const lunar::BasePart& part) const;
    void refreshOwned(double dt);
    std::vector<const lunar::BasePart*> ownedRoots();
    bool groundLoaded() const;
    void noteTouches(lunar::Model& character);
    std::optional<std::vector<std::uint8_t>> takeTouches();

    std::shared_ptr<lunar::net::Replica> replica_;
    lunar::avatar::AnimationSystem& animation_;
    std::unique_ptr<lunar::physics::World> world_;
    std::unique_ptr<UnionShapes> unionShapes_; // what the world's hash-only unions collide as
    std::unique_ptr<lunar::physics::HumanoidController> controller_;
    lunar::InstancePtr humanoidHold_; // the Humanoid the controller drives, alive while it does
    std::weak_ptr<lunar::Model> character_;
    // The character the world simulates: ours while we own its root, none
    // while the server moves it.
    std::weak_ptr<lunar::Model> simulatedCharacter_;
    // The other parts the server gave us (Replica::ownedHere) and the
    // unanchored parts constraints and joints hold to them, which move as one
    // mechanism (a game side door and its knob): the world simulates them and
    // the server's motion for them isn't played. Looked at again when the
    // ownership changes and every kOwnedRefresh seconds (a part handed over
    // anchored and let go later).
    std::unordered_map<const lunar::BasePart*, std::weak_ptr<lunar::Instance>> owned_;
    std::vector<std::weak_ptr<lunar::Instance>> ownedOrder_; // the same, in the order found
    std::uint64_t ownershipSeen_ = 0;
    double ownedAge_ = 0.0;
    static constexpr double kOwnedRefresh = 0.5;
    bool controlling_ = true; // whether the world's steps walk our Humanoid
    std::optional<lunar::Vector3> spawn_;
    std::optional<double> waitingForFloor_; // seconds waited, while it does
    std::optional<lunar::net::InstanceId> rootId_;
    // While the server moves our root: its samples so far, and how long since
    // the last one came (takeRootBack).
    std::size_t serverSamples_ = 0;
    double serverQuiet_ = 0.0;
    lunar::net::CharacterSync sync_;
    lunar::avatar::TrackSender tracks_;
    lunar::Vector3 move_;
    bool moveRelative_ = true;
    bool jump_ = false;
    std::optional<lunar::Vector3> facing_;
    std::optional<std::uint64_t> lastTakeMs_;
    // Our character's touches of parts a script listens to, as of the last
    // step (held weakly too: either part can be gone by the next), and what
    // happened to each pair the server hears of since the last ID_TOUCHES.
    struct Touching {
        lunar::physics::World::Touch pair;
        std::weak_ptr<lunar::Instance> ours, other;
    };
    std::vector<Touching> touching_;
    struct TouchChange {
        std::weak_ptr<const lunar::Instance> ours, other;
        std::uint8_t state = 0; // the client's 0..5: see takeTouches
    };
    std::vector<TouchChange> touchChanges_;
};

// A walk without a keyboard (--walk): directions held for so long, in
// world space. "square" walks a square of 3 s sides (48 studs at the
// default WalkSpeed) with a jump at the first corner, "line" goes north
// and back; otherwise comma-separated steps: n, e, s, w (towards -Z, +X,
// +Z, -X) or i (stand) followed by seconds, and j, a jump (Space held a
// quarter second) while keeping the direction before it: "i1,n3,j,e3".
class WalkPlan {
public:
    static std::optional<WalkPlan> parse(std::string_view text);

    struct Input {
        lunar::Vector3 move; // world space
        bool jump = false;
        bool done = false; // past the last step: stand
    };
    // The controls `seconds` into the walk.
    Input at(double seconds) const;
    double duration() const;
    const std::string& text() const { return text_; }

private:
    struct Step {
        lunar::Vector3 move;
        bool jump = false;
        double seconds = 0;
    };
    std::vector<Step> steps_;
    std::string text_;
};

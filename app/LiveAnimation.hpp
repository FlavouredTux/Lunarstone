#pragma once

// Animation on our client of a live game (--net-join --scripts, --net-replay
// --run): our character's tracks, which its Animate script loads, and the
// other players', which their clients replicate through the server as
// Animator.OnCombinedUpdate. One system poses every rig each client frame,
// so the replica renders mid-animation.

#include <lunar/assets/AssetCache.hpp>
#include <lunar/avatar/AnimationSystem.hpp>
#include <lunar/avatar/ReplicatedAnimation.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lunar {
class ClientSession;
}
namespace lunar::net {
class Replica;
}

class LiveAnimation {
public:
    explicit LiveAnimation(std::shared_ptr<lunar::net::Replica> replica);
    LiveAnimation(const LiveAnimation&) = delete;
    LiveAnimation& operator=(const LiveAnimation&) = delete;

    // Call before ClientSession::start: the scripts' LoadAnimation plays
    // here, and the client's frames step it (after PreAnimation).
    void attach(lunar::ClientSession& client);
    // --net-replay: each frame waits for the clips asked for so far, as if
    // downloads were instant, so a replay poses the same every time.
    void setWaitForClips(bool wait) { waitForClips_ = wait; }
    // The clock the replica's physics playback runs on (seconds; the one
    // its messages are stamped with): the steady clock unless set
    // (--net-replay plays on from the capture's end).
    void setClock(std::function<double()> clock) { clock_ = std::move(clock); }

    // The animation phase of a frame: the assemblies the server moves go to
    // where they are now (Replica::stepMotion), the animation updates the
    // server relayed since the last frame come in, then every rig moves on
    // by dt and is posed from its root.
    void step(double dt);

    // One line per animated character: who, then each track blended in
    // with its weight.
    std::vector<std::string> describe();

    // Where every rig plays, our character's included (LiveCharacter sends
    // its Animator's tracks from here).
    lunar::avatar::AnimationSystem& system() { return system_; }

private:
    void receive();

    std::shared_ptr<lunar::net::Replica> replica_;
    std::shared_ptr<lunar::assets::AssetCache> assets_;
    lunar::avatar::AnimationSystem system_;
    lunar::avatar::ReplicatedAnimation replicated_{system_};
    std::size_t received_ = 0; // replica events looked at so far
    bool waitForClips_ = false;
    std::function<double()> clock_;
};

#pragma once

#include "LiveAnimation.hpp"
#include "LiveCharacter.hpp"

#include <lunar/net/Remotes.hpp>
#include <lunar/net/ToolReplication.hpp>
#include <lunar/script/PlaySession.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lunar::net {
class Replica;
}

class ChatBubbles;

// Our client in a live game (--net-join --scripts, --net-play): the game's
// LocalScripts over the replica (ClientSession), the animations they and
// the other players' clients play (LiveAnimation), and the scripts'
// remotes both ways (RemoteBridge, which takes the replica's remote events
// from when this is made), our tools' moves (ToolReplicator) and the chat's
// bubbles, and our character, which it simulates in the frame's physics
// (LiveCharacter). Nothing here touches the network: the owner fills the
// replica and sends what character()->take(), tools()->take() and
// remotes().take() hand over, in that order.
class LiveClient {
public:
    struct Options {
        std::int64_t userId = 0;
        std::string platform;                  // an Enum.Platform name for UserInputService:GetPlatform()
        std::function<void(std::string)> note; // the scripts' output and the remotes, a line each
        // Player:RequestStreamAroundAsync: the owner asks the server and calls
        // session()->client().streamAroundDone(request) once it has answered.
        // Unset, every request is done at once (nothing streams).
        std::function<void(std::uint64_t request, lunar::Vector3 at, double timeout)> streamAround;
        // Our character is ours to move (LiveCharacter): simulated in the
        // frame's physics and sent to the server. Off, it stands where the
        // server put it.
        bool simulateCharacter = true;
        // The server teleports us: Player.OnTeleportInternal's table on our
        // Player (lunar::net::teleportTicket reads it). The owner leaves and
        // joins where it says.
        std::function<void(const lunar::RemoteValue& ticket)> teleport;
        std::function<void(lunar::ClientSession&)> configureClient;
    };

    LiveClient(std::shared_ptr<lunar::net::Replica> replica, Options options);
    ~LiveClient();
    LiveClient(const LiveClient&) = delete;
    LiveClient& operator=(const LiveClient&) = delete;

    // Makes the client once the join is in (Tag 13) and the server has
    // filled StarterPlayerScripts, or `anyway` (the join is late), and starts
    // its scripts once our Player has arrived. True once they run.
    bool start(bool anyway);
    bool started() const { return client_ && client_->started(); }
    void setConfigureClient(std::function<void(lunar::ClientSession&)> configure)
    {
        options_.configureClient = std::move(configure);
        if (client_ && options_.configureClient)
            options_.configureClient(*client_);
    }
    // One client frame: the scripts in RunService's order, animation in its
    // phase, then the bubbles past their time go.
    void step(double dt);
    // The client's clock: the seconds stepped so far.
    double time() const { return time_; }

    // Null until the join is in.
    lunar::ClientSession* session() { return client_.get(); }
    LiveAnimation* animation() { return animation_.get(); }
    lunar::net::RemoteBridge& remotes() { return *remotes_; }
    // Our tools' moves, grip welds and activations for the server; null until the join is in.
    lunar::net::ToolReplicator* tools() { return tools_.get(); }
    // Our character: the controls go in before step (setMoveVector,
    // setJump), what it owes the server comes out of take(Peer::nowMs()).
    // Null until the join is in, or with simulateCharacter off.
    LiveCharacter* character() { return character_.get(); }
    // The chat over the heads. The chat sink notes each line and adds it
    // here, unless the owner takes the sink over (TextChatSource, whose
    // observe should add it then). Null until the join is in.
    ChatBubbles* bubbles() { return bubbles_.get(); }

    // Where the scripts' output also goes (the window's console), besides
    // the note; null for nowhere else.
    void setOutputSink(lunar::OutputSink sink) { outputSink_ = std::move(sink); }

    // research: a click on a button of our PlayerGui by its path there
    // ("MainUI.LobbyFrame.Play"), firing what a click in the window fires.
    // What happened, as a line for the log.
    std::string click(std::string_view path);

private:
    std::shared_ptr<lunar::net::Replica> replica_;
    Options options_;
    lunar::OutputSink outputSink_;
    std::unique_ptr<lunar::net::RemoteBridge> remotes_;
    std::unique_ptr<LiveAnimation> animation_; // what the client's frames pose; outlives it
    std::unique_ptr<lunar::ClientSession> client_;
    std::unique_ptr<lunar::net::ToolReplicator> tools_;
    std::unique_ptr<LiveCharacter> character_;
    std::unique_ptr<ChatBubbles> bubbles_;
    double time_ = 0;
    std::optional<std::chrono::steady_clock::time_point> joinDataAt_; // when Tag 13 was first seen
};

// Our Player in a joined game: the newest one with our UserId. A server
// can still list one from an earlier session of the account, which comes in
// the join data after the one made for this connection, with a lower id.
// GUESS: that the server's instance ids only grow. Null before it arrives.
lunar::Player* ourPlayer(const lunar::net::Replica& replica, std::int64_t userId);

// research: LUNAR_NET_PROMPT="seconds:name;seconds:name", the prompts a
// session holds, by the scripts' time: the shown ProximityPrompt of that
// name is held, and let go once its HoldDuration has passed.
struct TimedPrompts {
    std::vector<std::pair<double, std::string>> prompts;
    std::size_t next = 0;
    struct Held {
        std::weak_ptr<lunar::ProximityPrompt> prompt;
        double until = 0.0;
    };
    std::vector<Held> held;
    static TimedPrompts fromEnvironment();
    void step(lunar::ClientSession* session, double seconds, const std::function<void(std::string)>& note);
};

// research: LUNAR_NET_CLICK="seconds:path;seconds:path", the clicks a
// session makes (LiveClient::click), by the scripts' time.
struct TimedClicks {
    std::vector<std::pair<double, std::string>> clicks;
    std::size_t next = 0;
    static TimedClicks fromEnvironment();
    // Clicks those due by `seconds` in the player's GUI, each line to note.
    void step(lunar::Player* player, double seconds, const std::function<void(std::string)>& note);
};
// The click itself (LiveClient::click), on a player's PlayerGui.
std::string clickGuiButton(lunar::Player* player, std::string_view path);

// Where a windowed client's game comes from (--net-play): a live server,
// or a recording of a session with one, played back as it happened.
// What a window's frame spends at most taking in what the server sent, once
// the game is going; the rest waits for the next frame. A room that streams
// in (the game) arrives as a burst that took 10 ms and more of the frame it came
// in, which at 120 frames a second is more than the frame.
inline constexpr std::chrono::microseconds frameMessageBudget{3000};

class GameFeed {
public:
    virtual ~GameFeed() = default;
    std::function<void(lunar::ClientSession&)> configureClient;

    // Takes in what has arrived, without waiting, and sends what the client
    // owes the server (acknowledgements, pings, markers, the scripts' remotes).
    virtual void pump() = 0;
    // Keeps the connection up from another thread while the window's thread
    // waits (acknowledgements, resends, RakNet's own pings) without handing
    // anything on: what arrives waits for the next pump. Only called while
    // no other call on the feed is running.
    virtual void service() {}
    // One client frame of dt seconds, once the join is in: the scripts and
    // the animations.
    virtual void frame(double dt) = 0;
    virtual const std::shared_ptr<lunar::net::Replica>& replica() const = 0;
    // Our client; null until the join is in and our Player has arrived.
    virtual LiveClient* client() = 0;
    virtual std::int64_t userId() const = 0;
    // Why the game is over (kicked, disconnected); nothing while it goes on.
    virtual std::optional<std::string> ended() const = 0;
    // Leaves (disconnects) and says what the session was like.
    virtual void leave() = 0;
    // Bumps when the feed moved to another server (a teleport): replica()
    // and client() are a new game's from then on.
    virtual std::uint64_t generation() const { return 0; }
    // The server has sent us elsewhere: the next pump() leaves and joins
    // there, holding the caller until that's done.
    virtual bool teleporting() const { return false; }
    // While the join is still finding a server (its web side, before the
    // session with the server starts): what it's waiting on, for the
    // loading screen. replica() is there meanwhile, empty.
    virtual std::optional<std::string> joining() const { return std::nullopt; }
    // The window has let go of the game before the last teleport (its
    // scene, chat and GUI): the feed can drop that game's session, which
    // otherwise holds the whole of it until the next teleport.
    virtual void releasePrevious() {}
};

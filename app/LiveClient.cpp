#include "LiveClient.hpp"

#include "LiveChat.hpp"

#include <lunar/PlaceFile.hpp>
#include <lunar/Prompts.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/Streaming.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

lunar::Player* ourPlayer(const lunar::net::Replica& replica, std::int64_t userId)
{
    lunar::Player* ours = nullptr;
    std::uint32_t newest = 0;
    if (const lunar::Instance* players = replica.game()->findFirstChildOfClass("Players"))
        for (const lunar::InstancePtr& child : players->children())
            if (auto* player = dynamic_cast<lunar::Player*>(child.get()); player && player->userId() == userId)
                if (const auto id = replica.idOf(player); !ours || (id && id->index > newest)) {
                    ours = player;
                    newest = id ? id->index : 0;
                }
    return ours;
}

LiveClient::LiveClient(std::shared_ptr<lunar::net::Replica> replica, Options options)
    : replica_(std::move(replica)), options_(std::move(options)),
      remotes_(std::make_unique<lunar::net::RemoteBridge>(replica_))
{
    remotes_->log = [this](const std::string& line) { options_.note(line); };
    // The teleport's events in full: what a hop needs is in their tables.
    remotes_->clientEvent
        = [this](lunar::Instance& target, std::string_view event, std::span<const lunar::RemoteValue> arguments) {
              if (&target != ourPlayer(*replica_, options_.userId))
                  return; // everyone's teleports reach every client; only ours matter
              options_.note(std::format("teleport: {}.{}{}", target.fullName(), event,
                                        lunar::net::describeRemoteArguments(arguments, 8000)));
              if (event == "OnTeleportInternal" && arguments.size() >= 2 && options_.teleport)
                  options_.teleport(arguments[1]);
          };
}

LiveClient::~LiveClient() = default;

namespace {

// Whether the server has filled StarterPlayerScripts yet. It sends its
// contents shortly after the join data (Tag 13), and the client copies them into PlayerScripts,
// so it waits for them; every server puts at least the default
// PlayerModule, PlayerScriptsLoader and RbxCharacterSounds there.
bool starterScriptsArrived(const lunar::DataModel& game)
{
    const lunar::Instance* starterPlayer = game.findService("StarterPlayer");
    const lunar::Instance* scripts
        = starterPlayer ? starterPlayer->findFirstChildOfClass("StarterPlayerScripts") : nullptr;
    return scripts && !scripts->children().empty();
}

} // namespace

bool LiveClient::start(bool anyway)
{
    // GUESS: a game whose StarterPlayerScripts stays empty starts a second after Tag 13.
    if (!client_ && replica_->joinDataDone && !joinDataAt_)
        joinDataAt_ = std::chrono::steady_clock::now();
    const bool waited = joinDataAt_ && std::chrono::steady_clock::now() - *joinDataAt_ > std::chrono::seconds(1);
    const bool ready = replica_->joinDataDone && (starterScriptsArrived(*replica_->game()) || waited);
    if (!client_ && (ready || anyway)) {
        client_ = std::make_unique<lunar::ClientSession>(replica_->game());
        // Instance references must bind as the packet is decoded, but scripts
        // must see the complete update. A WaitForChild("Humanoid") resuming
        // halfway through a packet otherwise misses its following description.
        client_->client().setDataUpdateInProgress([weak = std::weak_ptr<lunar::net::Replica>(replica_)] {
            auto replica = weak.lock();
            return replica && replica->applying();
        });
        if (options_.configureClient)
            options_.configureClient(*client_);
        // The window draws what the render step left, as the genuine client
        // does: the game places its camera and first-person rig on the character
        // there, and the physics moving the character after that trailed them.
        client_->setDrawBeforeSimulation(true);
        animation_ = std::make_unique<LiveAnimation>(replica_);
        animation_->attach(*client_);
        if (options_.simulateCharacter) {
            character_ = std::make_unique<LiveCharacter>(replica_, animation_->system());
            character_->attach(*client_);
        }
        if (!client_->client().setPlatform(options_.platform))
            options_.note(std::format("Enum.Platform has no {}; the scripts see OSX", options_.platform));
        client_->client().setNetworkBytecode([](std::string_view source) -> std::optional<std::string> {
            auto code = lunar::net::decodeProtectedBytecode(source);
            return code ? std::optional(std::move(*code)) : std::nullopt;
        });
        client_->client().setOutput([this](lunar::MessageType type, std::string_view text) {
            if (outputSink_)
                outputSink_(type, text);
            options_.note(std::format("[client] {}{}",
                                      type == lunar::MessageType::Error     ? "error: "
                                      : type == lunar::MessageType::Warning ? "warn: "
                                                                            : "",
                                      text));
        });
        client_->client().setRemoteSink(
            [this](lunar::Instance& remote, std::string_view event, std::vector<lunar::RemoteValue> args) {
                remotes_->send(remote, event, std::move(args));
            });
        client_->client().setEventShape(
            [this](const lunar::Instance& owner, std::string_view event) -> std::optional<std::vector<std::string>> {
                auto types = replica_->eventParameters(owner, event);
                if (!types)
                    return std::nullopt;
                std::vector<std::string> shape;
                for (const std::uint8_t type : *types) {
                    const char* name = type == lunar::net::nettype::Tuple ? "..." : lunar::net::luauTypeName(type);
                    shape.emplace_back(name ? name : "");
                }
                return shape;
            });
        remotes_->setReceiver(
            [this](lunar::Instance& remote, std::string_view event, std::span<const lunar::RemoteValue> args) {
                client_->client().receiveRemote(remote, event, args);
            });
        bubbles_ = std::make_unique<ChatBubbles>(replica_->game());
        client_->client().setChatSink([this](const lunar::ChatLine& line) {
            options_.note(describeChatLine(line));
            bubbles_->add(line, time_);
        });
        tools_ = std::make_unique<lunar::net::ToolReplicator>(replica_);
        tools_->log = [this](const std::string& line) { options_.note(line); };
        client_->client().setStreamAroundHook([this](std::uint64_t request, lunar::Vector3 at, double timeout) {
            if (options_.streamAround)
                options_.streamAround(request, at, timeout);
            else
                client_->client().streamAroundDone(request); // nothing streams: everything is here
        });
    }
    if (!client_)
        return false;
    if (client_->started())
        return true;
    lunar::Player* ours = ourPlayer(*replica_, options_.userId);
    if (!ours || !client_->start(*ours))
        return false;
    options_.note("started the client's scripts");
    // research: LUNAR_NET_CLIENT_LUA=<file> runs that Luau in the client's context too (a remote probe).
    if (const char* path = std::getenv("LUNAR_NET_CLIENT_LUA"); path && *path) {
        if (auto source = lunar::readFile(path); !source)
            options_.note("probe: " + source.error());
        else if (auto ran = client_->client().run(*source, "Probe"); !ran)
            options_.note("probe: " + ran.error().message);
    }
    return true;
}

void LiveClient::step(double dt)
{
    if (!started())
        return;
    client_->step(dt);
    time_ += dt;
    bubbles_->update(time_);
}

std::string LiveClient::click(std::string_view path)
{
    return clickGuiButton(ourPlayer(*replica_, options_.userId), path);
}

std::string clickGuiButton(lunar::Player* player, std::string_view path)
{
    lunar::Instance* at = player ? player->findFirstChildOfClass("PlayerGui") : nullptr;
    for (std::string_view rest = path; at && !rest.empty();) {
        const std::size_t dot = rest.find('.');
        at = at->findFirstChild(rest.substr(0, dot));
        rest = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
    }
    auto* button = dynamic_cast<lunar::GuiButton*>(at);
    if (!button)
        return std::format("click: no button {} in our PlayerGui", path);
    // In the middle of where the GUI last laid it out.
    const lunar::Vector2 middle = button->absolutePosition() + button->absoluteSize() * 0.5f;
    const int x = static_cast<int>(middle.x);
    const int y = static_cast<int>(middle.y);
    button->firePress(x, y);
    button->fireRelease(x, y, true);
    return std::format("click: {} (visible {})", path, button->visible());
}

TimedClicks TimedClicks::fromEnvironment()
{
    TimedClicks out;
    const char* env = std::getenv("LUNAR_NET_CLICK");
    for (std::string_view rest = env ? env : ""; !rest.empty();) {
        const std::size_t semi = rest.find(';');
        const std::string_view item = rest.substr(0, semi);
        rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
        const std::size_t colon = item.find(':');
        if (colon == std::string_view::npos)
            continue;
        out.clicks.emplace_back(std::stod(std::string(item.substr(0, colon))), std::string(item.substr(colon + 1)));
    }
    std::ranges::stable_sort(out.clicks, {}, &std::pair<double, std::string>::first);
    return out;
}

TimedPrompts TimedPrompts::fromEnvironment()
{
    TimedPrompts out;
    const char* env = std::getenv("LUNAR_NET_PROMPT");
    for (std::string_view rest = env ? env : ""; !rest.empty();) {
        const std::size_t semi = rest.find(';');
        const std::string_view item = rest.substr(0, semi);
        rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
        const std::size_t colon = item.find(':');
        if (colon == std::string_view::npos)
            continue;
        out.prompts.emplace_back(std::stod(std::string(item.substr(0, colon))), std::string(item.substr(colon + 1)));
    }
    std::ranges::stable_sort(out.prompts, {}, &std::pair<double, std::string>::first);
    return out;
}

void TimedPrompts::step(lunar::ClientSession* session, double seconds, const std::function<void(std::string)>& note)
{
    lunar::PromptSystem* system = session ? session->client().prompts() : nullptr;
    if (!system)
        return;
    while (next < prompts.size() && prompts[next].first <= seconds) {
        const std::string& name = prompts[next++].second;
        std::string shown;
        bool found = false;
        for (const lunar::PromptSystem::Shown& s : system->shown()) {
            shown += std::format("{}{}", shown.empty() ? "" : ", ", s.prompt->name());
            if (!found && s.prompt->name() == name) {
                found = true;
                system->holdBegin(*s.prompt);
                held.push_back({s.prompt, seconds + static_cast<double>(s.prompt->holdDuration()) + 0.2});
                note(std::format("prompt: holding {} ({})", s.prompt->fullName(), s.prompt->actionText()));
            }
        }
        if (!found)
            note(std::format("prompt: {} isn't shown (shown: {})", name, shown.empty() ? "none" : shown));
    }
    for (std::size_t i = 0; i < held.size();) {
        if (held[i].until > seconds) {
            ++i;
            continue;
        }
        if (auto prompt = held[i].prompt.lock()) {
            system->holdEnd(*prompt);
            note(std::format("prompt: let go of {}", prompt->name()));
        }
        held.erase(held.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void TimedClicks::step(lunar::Player* player, double seconds, const std::function<void(std::string)>& note)
{
    while (next < clicks.size() && clicks[next].first <= seconds)
        note(clickGuiButton(player, clicks[next++].second));
}

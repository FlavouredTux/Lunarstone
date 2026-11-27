#include "InstanceApi.hpp"
#include "Services.hpp"
#include "Userdata.hpp"

#include <lunar/assets/Social.hpp>
#include <lunar/assets/Web.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <stop_token>
#include <thread>

namespace lunar::lua {

template <class T>
struct SocialJob {
    std::atomic<bool> done = false;
    std::expected<T, std::string> result = std::unexpected(std::string());
    std::stop_source stop;
};

struct SocialState {
    struct Online {
        double expires = 0;
        std::vector<GameInviteFriend> friends;
    };
    std::map<std::int64_t, Online> online;
    std::map<std::int64_t, std::weak_ptr<SocialJob<std::vector<GameInviteFriend>>>> pendingOnline;
    std::weak_ptr<GameInviteSession> active;
};

namespace {

SocialState& state(Shared& shared)
{
    if (!shared.social)
        shared.social = std::make_shared<SocialState>();
    return *shared.social;
}

template <class T, class Work>
std::shared_ptr<SocialJob<T>> startJob(Work work)
{
    auto job = std::make_shared<SocialJob<T>>();
    try {
        std::thread([job, work = std::move(work)] {
            const assets::HttpStopScope scope(job->stop.get_token());
            try {
                job->result = work(job->stop.get_token());
            } catch (const std::exception& error) {
                job->result = std::unexpected(error.what());
            }
            job->done.store(true, std::memory_order_release);
        }).detach();
    } catch (const std::system_error& error) {
        job->result = std::unexpected(std::string("request could not start: ") + error.what());
        job->done.store(true, std::memory_order_release);
    }
    return job;
}

template <class T>
void awaitJob(Runtime& rt, std::shared_ptr<SocialJob<T>> job,
              std::move_only_function<void(const std::expected<T, std::string>&)> reply)
{
    rt.after(0, [&rt, job = std::move(job), reply = std::move(reply)] mutable {
        if (!job->done.load(std::memory_order_acquire))
            awaitJob(rt, std::move(job), std::move(reply));
        else
            reply(job->result);
    });
}

Player& localPlayer(lua_State* L, int index)
{
    auto* player = instanceCast<Player>(toInstance(L, index));
    if (!player)
        raise(L, "The player argument must be a Player instance");
    Runtime& rt = Runtime::of(L);
    const auto* players = instanceCast<Players>(rt.game().findService("Players"));
    if (!players || players->localPlayer() != player)
        raise(L, "This method must be invoked for the LocalPlayer");
    return *player;
}

std::int64_t recipientAt(lua_State* L, int index)
{
    if (lua_isnoneornil(L, index))
        return 0;
    const double value = luaL_checknumber(L, index);
    if (!std::isfinite(value) || value < 0 || value >= 0x1p63 || std::floor(value) != value)
        raise(L, "InviteUser must be a non-negative 64-bit user id");
    return static_cast<std::int64_t>(value);
}

Packed packOnline(const std::vector<GameInviteFriend>& friends, std::size_t limit)
{
    auto result = std::make_shared<PackedTable>();
    for (std::size_t i = 0; i < std::min(limit, friends.size()); ++i) {
        const auto& person = friends[i];
        auto row = std::make_shared<PackedTable>();
        const auto add = [&](std::string name, Value value) {
            row->emplace_back(packed(Value(std::move(name))), packed(std::move(value)));
        };
        add("VisitorId", static_cast<double>(person.userId));
        add("UserName", person.username);
        add("DisplayName", person.displayName);
        add("IsOnline", person.online);
        add("LocationType", static_cast<double>(person.locationType));
        add("PlaceId", static_cast<double>(person.placeId));
        add("UniverseId", static_cast<double>(person.universeId));
        add("GameId", person.gameId);
        add("LastLocation", person.lastLocation);
        add("LastOnline", person.lastOnline);
        result->emplace_back(packed(Value(static_cast<double>(i + 1))), Packed{Value(), std::move(row)});
    }
    return Packed{Value(), std::move(result)};
}

int getOnline(lua_State* L)
{
    self(L);
    if (Runtime::of(L).role() == ScriptRole::Server)
        raise(L, "GetFriendsOnline must be invoked on the LocalPlayer");
    auto& player = localPlayer(L, 1);
    const double requested = luaL_optnumber(L, 2, 200);
    if (!std::isfinite(requested) || requested < 0 || std::floor(requested) != requested)
        raise(L, "maxFriends must be a non-negative integer");
    const auto limit = static_cast<std::size_t>(std::min(requested, 200.0));
    Runtime& rt = Runtime::of(L);
    if (!limit || player.userId() <= 0) {
        lua_newtable(L);
        return 1;
    }
    auto& cache = state(rt.shared()).online;
    if (const auto found = cache.find(player.userId()); found != cache.end() && found->second.expires > rt.now()) {
        unpack(L, packOnline(found->second.friends, limit));
        return 1;
    }
    checkYieldable(L);
    robloxWeb(L);
    auto& pending = state(rt.shared()).pendingOnline[player.userId()];
    auto job = pending.lock();
    if (!job) {
        job = startJob<std::vector<GameInviteFriend>>([web = rt.shared().web, id = player.userId()](std::stop_token) {
            return assets::getFriendsOnline(*web, id);
        });
        pending = job;
    }
    const auto token = rt.suspend(L);
    awaitJob<std::vector<GameInviteFriend>>(
        rt, std::move(job), [&rt, L, token, id = player.userId(), limit](const auto& result) {
            state(rt.shared()).pendingOnline.erase(id);
            if (!rt.isSuspended(L, token))
                return;
            const auto* players = instanceCast<Players>(rt.game().findService("Players"));
            const auto* local = players ? players->localPlayer() : nullptr;
            if (!local || local->userId() != id) {
                rt.fail(L, token, "The local player changed before the friend request completed");
                return;
            }
            if (!result) {
                rt.fail(L, token, "GetFriendsOnline failed: " + result.error());
                return;
            }
            state(rt.shared()).online.insert_or_assign(id, SocialState::Online{rt.now() + 30, *result});
            const Packed rows = packOnline(*result, limit);
            rt.wake(
                L, token,
                [&](lua_State* thread) {
                    unpack(thread, rows);
                    return 1;
                },
                "GetFriendsOnline");
        });
    return lua_yield(L, 0);
}

int canInvite(lua_State* L)
{
    self(L);
    Runtime& rt = Runtime::of(L);
    if (rt.role() == ScriptRole::Server)
        raise(L, "CanSendGameInviteAsync must be called from a local script");
    localPlayer(L, 2);
    const auto recipient = recipientAt(L, 3);
    const auto placeId = static_cast<std::int64_t>(rt.game().placeId());
    if (placeId == 0) {
        // The native method allows the Studio prompt; its UI refuses to send
        // from an unpublished place. The native frontend has the same limit.
        lua_pushboolean(L, true);
        return 1;
    }
    checkYieldable(L);
    robloxWeb(L);
    auto job = startJob<bool>([web = rt.shared().web, placeId, recipient](std::stop_token) {
        return assets::canPromptGameInvite(*web, placeId, recipient);
    });
    const auto token = rt.suspend(L);
    awaitJob<bool>(rt, std::move(job), [&rt, L, token, placeId](const auto& result) {
        if (!rt.isSuspended(L, token))
            return;
        if (!result)
            rt.fail(L, token, "CanSendGameInviteAsync failed: " + result.error());
        else if (static_cast<std::int64_t>(rt.game().placeId()) != placeId)
            rt.fail(L, token, "The experience changed before the permission check completed");
        else
            rt.wake(L, token, [&](lua_State* thread) {
                lua_pushboolean(thread, *result);
                return 1;
            });
    });
    return lua_yield(L, 0);
}

void fireClosed(Shared& shared, Instance& service, Player& player, const std::vector<std::int64_t>& ids)
{
    auto recipients = std::make_shared<PackedTable>();
    for (std::size_t i = 0; i < ids.size(); ++i)
        recipients->emplace_back(packed(Value(static_cast<double>(i + 1))), packed(Value(static_cast<double>(ids[i]))));
    shared.events.fire(service, "GameInvitePromptClosed",
                       {packed(Value(player.shared_from_this())), Packed{Value(), std::move(recipients)}});
}

std::string newKey()
{
    const std::uint64_t high = scriptEntropy(), low = scriptEntropy();
    return std::format("{:08x}-{:04x}-4{:03x}-{:04x}-{:012x}", high >> 32, (high >> 16) & 0xffff, high & 0xfff,
                       ((low >> 48) & 0x3fff) | 0x8000, low & 0xffffffffffff);
}

class InviteSession final : public GameInviteSession {
public:
    InviteSession(Runtime& rt, Instance& service, Player& player, GameInviteOptions options)
        : shared_(rt.sharedPtr()), player_(player.shared_from_this()), service_(service.shared_from_this()),
          web_(rt.shared().web), role_(rt.role()), place_(static_cast<std::int64_t>(rt.game().placeId())),
          user_(player.userId()), options_(std::move(options))
    {
        retry();
    }
    ~InviteSession() override { stop(); }
    const GameInviteOptions& options() const override { return options_; }
    bool loading() const override { return loading_; }
    std::string_view error() const override { return error_; }
    const std::vector<GameInviteFriend>& friends() const override { return friends_; }
    bool sending() const override { return send_ != nullptr; }
    bool closed() const override { return closed_; }
    GameInviteStatus status(std::int64_t id) const override
    {
        const auto found = statuses_.find(id);
        return found == statuses_.end() ? GameInviteStatus::Available : found->second;
    }
    void poll() override
    {
        if (closed_)
            return;
        if (!alive()) {
            closed_ = true;
            stop();
            return;
        }
        if (load_ && load_->done.load(std::memory_order_acquire)) {
            loading_ = false;
            if (load_->result)
                friends_ = std::move(*load_->result);
            else
                error_ = "Friends could not be loaded: " + load_->result.error();
            load_.reset();
        }
        if (send_ && send_->done.load(std::memory_order_acquire)) {
            if (send_->result) {
                statuses_[sendingId_] = GameInviteStatus::Sent;
                sent_.push_back(sendingId_);
            } else {
                statuses_[sendingId_] = GameInviteStatus::Failed;
                error_ = "The invitation could not be sent: " + send_->result.error();
            }
            send_.reset();
        }
    }
    void retry() override
    {
        if (closed_ || load_ || send_)
            return;
        friends_.clear();
        error_.clear();
        loading_ = true;
        if (!alive() || place_ <= 0 || user_ <= 0) {
            loading_ = false;
            error_ = "Game invitations are unavailable in local play.";
            return;
        }
        load_ = startJob<std::vector<GameInviteFriend>>(
            [web = web_, place = place_, user = user_, target = options_.inviteUser](
                std::stop_token stop) -> std::expected<std::vector<GameInviteFriend>, std::string> {
                auto allowed = assets::canPromptGameInvite(*web, place, target);
                if (!allowed)
                    return std::unexpected(allowed.error());
                if (!*allowed)
                    return std::unexpected("Invitations are not available for this account or recipient.");
                if (stop.stop_requested())
                    return std::unexpected("cancelled");
                if (target) {
                    auto person = assets::getInviteUser(*web, target);
                    if (!person)
                        return std::unexpected(person.error());
                    return std::vector<GameInviteFriend>{std::move(*person)};
                }
                auto friends = assets::getFriends(*web, user);
                if (!friends)
                    return friends;
                std::erase_if(*friends, [user](const auto& person) { return person.userId == user; });
                if (!friends->empty() && !stop.stop_requested()) {
                    if (auto online = assets::getFriendsOnline(*web, user))
                        for (auto& person : *friends)
                            if (const auto found = std::ranges::find(*online, person.userId, &GameInviteFriend::userId);
                                found != online->end())
                                person = *found;
                }
                std::ranges::stable_sort(*friends, [](const auto& a, const auto& b) {
                    if (a.online != b.online)
                        return a.online > b.online;
                    return a.displayName < b.displayName;
                });
                return friends;
            });
    }
    void invite(std::int64_t id) override
    {
        poll();
        if (closed_ || loading_ || send_ || status(id) == GameInviteStatus::Sent || id == user_
            || std::ranges::find(friends_, id, &GameInviteFriend::userId) == friends_.end())
            return;
        error_.clear();
        statuses_[id] = GameInviteStatus::Sending;
        sendingId_ = id;
        auto& key = keys_[id];
        if (key.empty())
            key = newKey();
        send_ = startJob<void>([web = web_, place = place_, id, options = options_, key,
                                weak = shared_](std::stop_token stop) -> std::expected<void, std::string> {
            return assets::sendGameInvite(*web, place, id, options, key,
                                          [stop, weak] { return stop.stop_requested() || weak.expired(); });
        });
    }
    void close() override
    {
        if (closed_)
            return;
        poll();
        if (closed_)
            return;
        closed_ = true;
        stop();
        auto shared = shared_.lock();
        auto player = std::dynamic_pointer_cast<Player>(player_.lock());
        auto service = service_.lock();
        if (shared && player && service)
            fireClosed(*shared, *service, *player, sent_);
    }

private:
    bool alive() const
    {
        auto shared = shared_.lock();
        auto player = std::dynamic_pointer_cast<Player>(player_.lock());
        auto service = service_.lock();
        Runtime* rt = shared ? shared->runtimeFor(role_) : nullptr;
        const auto* players = rt ? instanceCast<Players>(rt->game().findService("Players")) : nullptr;
        return rt && service && !service->destroyed() && player && !player->destroyed() && player->userId() == user_
               && players && players->localPlayer() == player.get()
               && static_cast<std::int64_t>(rt->game().placeId()) == place_;
    }
    void stop()
    {
        if (load_)
            load_->stop.request_stop();
        if (send_)
            send_->stop.request_stop();
    }
    std::weak_ptr<Shared> shared_;
    std::weak_ptr<Instance> player_, service_;
    std::shared_ptr<const assets::RobloxWeb> web_;
    ScriptRole role_;
    std::int64_t place_, user_, sendingId_ = 0;
    GameInviteOptions options_;
    bool loading_ = false, closed_ = false;
    std::string error_;
    std::vector<GameInviteFriend> friends_;
    std::map<std::int64_t, GameInviteStatus> statuses_;
    std::map<std::int64_t, std::string> keys_;
    std::vector<std::int64_t> sent_;
    std::shared_ptr<SocialJob<std::vector<GameInviteFriend>>> load_;
    std::shared_ptr<SocialJob<void>> send_;
};

int promptInvite(lua_State* L)
{
    Instance& service = self(L);
    auto& player = localPlayer(L, 2);
    Runtime& rt = Runtime::of(L);
    GameInviteOptions options;
    if (!lua_isnoneornil(L, 3)) {
        Instance* supplied = toInstance(L, 3);
        if (!supplied || !supplied->isA("ExperienceInviteOptions"))
            raise(L, "Second argument must be an ExperienceInviteOptions instance");
        if (rt.role() == ScriptRole::Server)
            raise(L, "ExperienceInviteOptions must be used from a local script");
        lua_getfield(L, 3, "InviteUser");
        options.inviteUser = recipientAt(L, -1);
        lua_pop(L, 1);
        const auto read = [&](const char* name) {
            lua_getfield(L, 3, name);
            const std::string value(checkString(L, -1));
            lua_pop(L, 1);
            return value;
        };
        options.promptMessage = read("PromptMessage");
        options.launchData = read("LaunchData");
        options.inviteMessageId = read("InviteMessageId");
    }
    if (options.inviteUser == player.userId() && options.inviteUser != 0)
        raise(L, "The LocalPlayer cannot invite themselves");
    if (rt.shared().events.hasListeners(service, "PromptInviteRequested")) {
        // A running Roblox core frontend already owns the native prompt.
        rt.shared().events.fire(
            service, "PromptInviteRequested",
            {packed(Value(player.shared_from_this())), lua_isnoneornil(L, 3) ? Packed{} : pack(L, 3)});
        return 0;
    }
    auto& social = state(rt.shared());
    if (auto existing = social.active.lock(); existing && !existing->closed())
        return 0;
    if (!rt.shared().gameInvitePrompt)
        raise(L, "The game invitation prompt is unavailable in this host");
    robloxWeb(L);
    auto session = std::make_shared<InviteSession>(rt, service, player, std::move(options));
    social.active = session;
    if (!rt.shared().gameInvitePrompt(session)) {
        session.reset();
        raise(L, "The game invitation prompt is unavailable in this host");
    }
    return 0;
}

int promptClosed(lua_State* L)
{
    Instance& service = self(L);
    auto& player = localPlayer(L, 2);
    Runtime& rt = Runtime::of(L);
    if (auto active = state(rt.shared()).active.lock(); active && !active->closed()) {
        active->close();
        return 0;
    }
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<std::int64_t> ids;
    std::set<std::int64_t> seen;
    const int count = lua_objlen(L, 3);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, 3, i);
        const auto id = recipientAt(L, -1);
        if (id > 0 && seen.insert(id).second)
            ids.push_back(id);
        lua_pop(L, 1);
    }
    fireClosed(rt.shared(), service, player, ids);
    return 0;
}

constexpr auto methods = std::to_array<Method>({
    {"GetFriendsOnline", "Player", getOnline},
    {"GetFriendsOnlineAsync", "Player", getOnline},
    {"CanSendGameInviteAsync", "SocialService", canInvite},
    {"PromptGameInvite", "SocialService", promptInvite},
    {"InvokeGameInvitePromptClosed", "SocialService", promptClosed},
});

} // namespace

std::span<const Method> socialMethods()
{
    return methods;
}

} // namespace lunar::lua

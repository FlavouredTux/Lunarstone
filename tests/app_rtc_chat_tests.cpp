#include "framework.hpp"

#include "RtcChat.hpp"

#include <lunar/assets/Json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

const lunar::assets::json::Value& field(const lunar::assets::json::Value& object, std::string_view name)
{
    if (const auto* value = object[name])
        return *value;
    throw lt::Failure{std::format("missing JSON field {}", name)};
}

std::string stringOf(const lunar::assets::json::Value& value)
{
    if (const auto* text = value.string())
        return *text;
    throw lt::Failure{"expected a JSON string"};
}

struct Wire {
    std::mutex mutex;
    std::vector<std::string> sent;
    std::deque<std::string> received;
    std::atomic<int> connections = 0;
    std::atomic<bool> closed = false;
    std::atomic<bool> failSend = false;
};

class Socket final : public lunar::assets::WebSocket {
public:
    explicit Socket(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}
    std::expected<void, std::string> send(std::string_view text, std::stop_token) override
    {
        if (wire_->failSend)
            return std::unexpected("test send failed");
        const std::scoped_lock lock(wire_->mutex);
        wire_->sent.emplace_back(text);
        return {};
    }
    std::expected<std::optional<std::string>, std::string> receive() override
    {
        const std::scoped_lock lock(wire_->mutex);
        if (!wire_->received.empty()) {
            std::string text = std::move(wire_->received.front());
            wire_->received.pop_front();
            return text;
        }
        if (wire_->closed)
            return std::unexpected("test connection closed");
        return std::nullopt;
    }

private:
    std::shared_ptr<Wire> wire_;
};

RtcChatSource::Connector connector(const std::shared_ptr<Wire>& wire)
{
    return [wire](std::string_view,
                  std::stop_token) -> std::expected<std::unique_ptr<lunar::assets::WebSocket>, std::string> {
        ++wire->connections;
        return std::make_unique<Socket>(wire);
    };
}

template <class F>
void until(F ready)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(ready());
}

} // namespace

TEST(rtc_protocol_quotes_messages_and_marks_names_unverified)
{
    const auto init = lunar::assets::json::parse(rtcInit(".Roblox Friend\""));
    CHECK(init.has_value());
    CHECK_EQ(stringOf(field(*init, "username")), ".RobloxFriend");
    CHECK_EQ(stringOf(field(*init, "event")), "init");
    CHECK((*init)["token"] == nullptr);
    const auto message = lunar::assets::json::parse(rtcMessage("  hello  \"friend\" \\ test\n"));
    CHECK(message.has_value());
    CHECK_EQ(stringOf(field(*message, "event")), "rtc");
    const auto* args = field(*message, "args").array();
    CHECK(args != nullptr);
    CHECK_EQ(args->size(), 5u);
    CHECK_EQ(stringOf((*args)[0]), "hello");
    CHECK_EQ(stringOf((*args)[1]), "");
    CHECK_EQ(stringOf((*args)[2]), "\"friend\"");
    CHECK_EQ(stringOf((*args)[3]), "\\");
    CHECK_EQ(stringOf((*args)[4]), "test");
    CHECK(rtcMessage(" \t\n ").empty());
}

TEST(rtc_plain_text_removes_styles_and_preserves_unicode)
{
    CHECK_EQ(rtcPlainText("§8[§cAsteroide§8] §#ff0022Friend§r: héllo 世界"), "[Asteroide] Friend: héllo 世界");
    CHECK_EQ(rtcPlainText("§x§f§f§0§0§a§ahello"), "hello");
    CHECK_EQ(rtcPlainText("<b>plain</b>\ntext\tkept"), "<b>plain</b> text kept");
    CHECK_EQ(rtcPlainText("§#nope"), "§#nope");
    CHECK_EQ(rtcPlainText("\xee\x90\xa9 hello"), "[Asteroide] hello");
}

TEST(rtc_ping_packets_match_the_meteor_client)
{
    for (const bool available : {false, true}) {
        const auto heartbeat = lunar::assets::json::parse(rtcPing(available));
        CHECK(heartbeat.has_value());
        CHECK_EQ(stringOf(field(*heartbeat, "event")), "ping");
        CHECK_EQ(field(*heartbeat, "available").boolean(), std::optional<bool>(available));
        CHECK((*heartbeat)["args"] == nullptr);
        const auto reply = lunar::assets::json::parse(rtcPing(available, 42));
        CHECK(reply.has_value());
        CHECK_EQ(field(*reply, "available").boolean(), std::optional<bool>(available));
        const auto* args = field(*reply, "args").array();
        CHECK(args != nullptr);
        CHECK_EQ(args->size(), 1u);
        CHECK_EQ(stringOf(args->front()), "42");
    }
}

TEST(rtc_connects_on_activation_and_delivers_only_on_the_ui_thread)
{
    auto wire = std::make_shared<Wire>();
    RtcChatSource source({.username = "Test"}, connector(wire));
    std::vector<std::string> messages;
    const auto main = std::this_thread::get_id();
    source.deliver = [&](const lunar::ui::ChatMessage& message) {
        CHECK(std::this_thread::get_id() == main);
        messages.push_back(message.text);
    };
    source.poll();
    CHECK_EQ(wire->connections.load(), 0);
    source.setAvailable(true);
    source.activate();
    source.activate();
    until([&] { return source.connected(); });
    CHECK_EQ(wire->connections.load(), 1);
    CHECK(messages.empty());
    {
        const std::scoped_lock lock(wire->mutex);
        wire->received.push_back(R"({"event":"message","message":"§aFriend: hello"})");
        wire->received.push_back(R"({"event":"users","further":["Friend"]})");
        wire->received.push_back("not JSON");
    }
    source.send("hello friend");
    until([&] {
        source.poll();
        const std::scoped_lock lock(wire->mutex);
        return wire->sent.size() >= 3
               && std::ranges::any_of(messages, [](const std::string& text) { return text == "Friend: hello"; });
    });
    const std::scoped_lock lock(wire->mutex);
    const auto init = lunar::assets::json::parse(wire->sent.front());
    CHECK(init.has_value());
    CHECK_EQ(stringOf(field(*init, "username")), ".Test");
    bool sentChat = false;
    bool sentPing = false;
    for (const std::string& packet : wire->sent) {
        const auto data = lunar::assets::json::parse(packet);
        CHECK(data.has_value());
        const std::string event = stringOf(field(*data, "event"));
        sentChat |= event == "rtc";
        sentPing |= event == "ping";
        if (event == "ping") {
            CHECK_EQ(field(*data, "available").boolean(), std::optional<bool>(true));
            const auto* args = field(*data, "args").array();
            CHECK(args != nullptr);
            CHECK_EQ(args->size(), 1u);
            const std::string nonce = stringOf(args->front());
            std::uint32_t number = 0;
            const auto parsed = std::from_chars(nonce.data(), nonce.data() + nonce.size(), number);
            CHECK(parsed.ec == std::errc{} && parsed.ptr == nonce.data() + nonce.size());
            CHECK(number < 99'999'999u);
        }
    }
    CHECK(sentChat && sentPing);
}

TEST(rtc_users_replies_follow_window_availability)
{
    auto wire = std::make_shared<Wire>();
    RtcChatSource source({.username = "Test"}, connector(wire));
    source.activate();
    until([&] { return source.connected(); });
    for (const bool available : {false, true, false}) {
        std::size_t previous = 0;
        source.setAvailable(available);
        {
            const std::scoped_lock lock(wire->mutex);
            previous = wire->sent.size();
            wire->received.push_back(R"({"event":"users","further":[]})");
        }
        until([&] {
            const std::scoped_lock lock(wire->mutex);
            return wire->sent.size() > previous;
        });
        const std::scoped_lock lock(wire->mutex);
        const auto reply = lunar::assets::json::parse(wire->sent.back());
        CHECK(reply.has_value());
        CHECK_EQ(stringOf(field(*reply, "event")), "ping");
        CHECK_EQ(field(*reply, "available").boolean(), std::optional<bool>(available));
    }
}

TEST(rtc_disconnected_messages_are_not_sent_later)
{
    auto wire = std::make_shared<Wire>();
    RtcChatSource source({.username = "Test"}, connector(wire));
    source.send("do not send this later");
    source.activate();
    until([&] { return source.connected(); });
    const std::scoped_lock lock(wire->mutex);
    CHECK_EQ(wire->sent.size(), 1u);
    CHECK(wire->sent.front().contains("init"));
}

TEST(rtc_connection_loss_and_reconnect_wait_do_not_block_shutdown)
{
    auto wire = std::make_shared<Wire>();
    auto source = std::make_unique<RtcChatSource>(RtcChatSource::Options{.username = "Test"}, connector(wire));
    source->activate();
    until([&] { return source->connected(); });
    wire->closed = true;
    until([&] { return !source->connected(); });
    const auto started = std::chrono::steady_clock::now();
    source.reset();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
}

TEST(rtc_defaults_to_the_online_roblox_username)
{
    auto wire = std::make_shared<Wire>();
    RtcChatSource source(RtcChatSource::Options{}, connector(wire));
    source.setRobloxUsername("ActualAccountName");
    CHECK_EQ(source.username(), "ActualAccountName");
    CHECK_EQ(wire->connections.load(), 0); // identity alone never connects
    source.activate();
    until([&] { return source.connected(); });
    const std::scoped_lock lock(wire->mutex);
    const auto init = lunar::assets::json::parse(wire->sent.front());
    CHECK(init.has_value());
    CHECK_EQ(stringOf(field(*init, "username")), ".ActualAccountName");
}

TEST(rtc_late_join_and_opt_in_name_changes_reconnect_with_the_selected_identity)
{
    auto wire = std::make_shared<Wire>();
    RtcChatSource source(RtcChatSource::Options{}, connector(wire));
    const std::string guest(source.username());
    CHECK(guest.starts_with("Lunarstone_"));
    source.activate();
    until([&] { return source.connected(); });
    source.setRobloxUsername("JoinedAccount");
    until([&] { return source.connected(); });
    CHECK_EQ(source.username(), "JoinedAccount");
    CHECK_EQ(wire->connections.load(), 2);

    source.send("/name MyChosenName");
    until([&] { return source.connected(); });
    CHECK_EQ(source.username(), "MyChosenName");
    CHECK_EQ(wire->connections.load(), 3);
    source.setRobloxUsername("LatestAccount");
    CHECK_EQ(source.username(), "MyChosenName");
    CHECK_EQ(wire->connections.load(), 3); // an explicit choice survives replication and teleports

    source.send("/name reset");
    until([&] { return source.connected(); });
    CHECK_EQ(source.username(), "LatestAccount");
    CHECK_EQ(wire->connections.load(), 4);
    const std::scoped_lock lock(wire->mutex);
    CHECK_EQ(wire->sent.size(), 4u);
    std::vector<std::string> names;
    for (const std::string& packet : wire->sent) {
        const auto data = lunar::assets::json::parse(packet);
        CHECK(data.has_value());
        CHECK_EQ(stringOf(field(*data, "event")), "init"); // /name is never sent as a public message
        names.push_back(stringOf(field(*data, "username")));
    }
    CHECK_EQ(names, (std::vector<std::string>{"." + guest, ".JoinedAccount", ".MyChosenName", ".LatestAccount"}));
}

TEST(rtc_explicit_launch_name_can_be_reset_and_invalid_names_stay_local)
{
    auto wire = std::make_shared<Wire>();
    RtcChatSource source({.username = "LaunchChoice"}, connector(wire));
    source.setRobloxUsername("RobloxAccount");
    CHECK_EQ(source.username(), "LaunchChoice");
    source.send("/name invalid name");
    CHECK_EQ(source.username(), "LaunchChoice");
    source.send("/name reset");
    CHECK_EQ(source.username(), "RobloxAccount");
    source.send("/name");
    CHECK_EQ(wire->connections.load(), 0);
    CHECK(wire->sent.empty());
}

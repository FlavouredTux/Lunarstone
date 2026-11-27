#include "RtcChat.hpp"

#include <lunar/assets/Json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <format>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

namespace {

std::string quote(std::string_view text)
{
    std::string result = "\"";
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '\\' || character == '"') {
            result += '\\';
            result += character;
        } else if (byte < 0x20) {
            result += std::format("\\u{:04x}", byte);
        } else {
            result += character;
        }
    }
    result += '"';
    return result;
}

std::string usernameFromEnvironment()
{
    if (const char* configured = std::getenv("LUNAR_RTC_NAME"))
        return configured;
    return {};
}

std::string guestName(std::string_view name)
{
    std::string clean = ".";
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
            clean += c;
        if (clean.size() >= 33)
            break;
    }
    return clean.size() > 1 ? clean : ".Lunarstone";
}

} // namespace

std::string rtcPing(bool available, std::optional<std::uint32_t> nonce)
{
    if (nonce)
        return std::format(R"({{"event":"ping","args":["{}"],"available":{}}})", *nonce, available);
    return std::format(R"({{"event":"ping","available":{}}})", available);
}

std::string rtcInit(std::string_view username)
{
    return std::format(R"({{"event":"init","username":{},"online":true}})", quote(guestName(username)));
}

std::string rtcMessage(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    std::string args;
    for (std::size_t start = 0;;) {
        const std::size_t end = text.find(' ', start);
        if (!args.empty())
            args += ',';
        args += quote(text.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return std::format(R"({{"event":"rtc","args":[{}],"format":["white","none"]}})", args);
}

std::string rtcPlainText(std::string_view text)
{
    std::string plain;
    for (std::size_t i = 0; i < text.size();) {
        // Minecraft section-sign colours/styles. Keep the surrounding UTF-8
        // text as plain text; the server already includes the sender's name.
        if (text.substr(i).starts_with("\xc2\xa7") && i + 2 < text.size()) {
            const char code = text[i + 2];
            const auto hex
                = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
            if (code == '#' && i + 9 <= text.size() && std::ranges::all_of(text.substr(i + 3, 6), hex)) {
                i += 9;
                continue;
            }
            if (hex(code) || std::string_view("klmnorxKLMNORX").contains(code)) {
                i += 3;
                continue;
            }
        }
        if (text.substr(i).starts_with("\xee\x90\xa9")) {
            plain += "[Asteroide]";
            i += 3;
            continue;
        }
        // Do not let control characters alter a displayed message's layout.
        const auto byte = static_cast<unsigned char>(text[i++]);
        if (byte >= 0x20 && byte != 0x7f)
            plain += static_cast<char>(byte);
        else if (byte == '\n' || byte == '\r' || byte == '\t')
            plain += ' ';
    }
    return plain;
}

struct RtcChatSource::Impl {
    Options options;
    std::string customName;
    std::string robloxName;
    std::string fallbackName;
    Connector connect;
    std::atomic<bool> connected = false;
    std::atomic<bool> available = false;
    std::mt19937 random{std::random_device{}()};
    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<lunar::ui::ChatMessage> incoming;
    std::deque<std::string> outgoing;
    std::jthread worker; // destroyed and joined before its state

    std::string chosenName() const
    {
        return !customName.empty() ? customName : !robloxName.empty() ? robloxName : fallbackName;
    }

    void emit(std::string text, bool error = false)
    {
        lunar::ui::ChatMessage message;
        message.kind = error ? lunar::ui::ChatMessage::Kind::Error : lunar::ui::ChatMessage::Kind::System;
        message.text = std::move(text);
        const std::scoped_lock lock(mutex);
        if (incoming.size() >= 128)
            incoming.pop_front();
        incoming.push_back(std::move(message));
    }

    void wait(std::stop_token stop, std::chrono::milliseconds delay)
    {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, stop, delay, [this] { return !outgoing.empty(); });
    }

    std::expected<void, std::string> run(lunar::assets::WebSocket& socket, std::stop_token stop)
    {
        if (auto sent = socket.send(rtcInit(options.username), stop); !sent)
            return sent;
        connected = true;
        emit(std::format("Connected to Asteroide RTC as {} (public chat).", guestName(options.username).substr(1)));
        auto nextPing = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!stop.stop_requested()) {
            std::deque<std::string> pending;
            {
                const std::scoped_lock lock(mutex);
                pending.swap(outgoing);
            }
            for (const std::string& message : pending)
                if (auto sent = socket.send(message, stop); !sent)
                    return std::unexpected("connection lost while sending; the message was not retried");
            for (int count = 0; count < 64 && !stop.stop_requested(); ++count) {
                auto received = socket.receive();
                if (!received)
                    return std::unexpected(received.error());
                if (!*received)
                    break;
                const auto data = lunar::assets::json::parse(**received);
                if (!data)
                    continue;
                if (const auto* message = (*data)["message"]; message && message->string()) {
                    std::string plain = rtcPlainText(*message->string());
                    if (!plain.empty())
                        emit(std::move(plain));
                }
                if (const auto* event = (*data)["event"]; event && event->string() && *event->string() == "users") {
                    const auto* users = (*data)["further"];
                    if (users && users->array()) {
                        // ws.java calls ping with Random.nextInt(99999999)
                        // after users updates, independently of the heartbeat.
                        const auto nonce = std::uniform_int_distribution<std::uint32_t>(0, 99'999'998)(random);
                        if (auto sent = socket.send(rtcPing(available, nonce), stop); !sent)
                            return sent;
                    }
                }
            }
            if (std::chrono::steady_clock::now() >= nextPing) {
                if (auto sent = socket.send(rtcPing(available), stop); !sent)
                    return sent;
                nextPing = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            }
            wait(stop, std::chrono::milliseconds(50));
        }
        return {};
    }

    void work(std::stop_token stop)
    {
        auto delay = std::chrono::seconds(2);
        while (!stop.stop_requested()) {
            auto connection = connect(options.url, stop);
            std::string failure;
            if (connection) {
                delay = std::chrono::seconds(2);
                if (auto result = run(**connection, stop); !result)
                    failure = std::move(result.error());
                connection->reset();
            } else {
                failure = std::move(connection.error());
            }
            connected = false;
            {
                const std::scoped_lock lock(mutex);
                outgoing.clear(); // never resend old chat after reconnecting
            }
            if (stop.stop_requested())
                return;
            emit(std::format("RTC disconnected: {}. Reconnecting in {} seconds.", failure, delay.count()), true);
            wait(stop, delay);
            delay = std::min(delay * 2, std::chrono::seconds(30));
        }
    }
};

RtcChatSource::RtcChatSource() : RtcChatSource(Options{.username = usernameFromEnvironment()}) {}

RtcChatSource::RtcChatSource(Options options, Connector connect) : impl_(std::make_unique<Impl>())
{
    impl_->customName = options.username;
    impl_->fallbackName = std::format("Lunarstone_{:06x}", std::random_device{}() & 0xffffffu);
    options.username = impl_->chosenName();
    impl_->options = std::move(options);
    impl_->connect = std::move(connect);
}

RtcChatSource::~RtcChatSource() = default;

bool RtcChatSource::connected() const
{
    return impl_->connected;
}

void RtcChatSource::setAvailable(bool available)
{
    impl_->available = available;
}

std::string_view RtcChatSource::username() const
{
    return impl_->options.username;
}

void RtcChatSource::restart()
{
    const std::string chosen = impl_->chosenName();
    if (chosen == impl_->options.username)
        return;
    const bool active = impl_->worker.joinable();
    if (active) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    impl_->connected = false;
    {
        const std::scoped_lock lock(impl_->mutex);
        impl_->outgoing.clear();
    }
    // The worker is stopped before changing the identity it reads.
    impl_->options.username = chosen;
    if (active)
        activate();
}

void RtcChatSource::setRobloxUsername(std::string_view username)
{
    if (impl_->robloxName == username)
        return;
    impl_->robloxName = username;
    restart();
}

void RtcChatSource::activate()
{
    if (impl_->worker.joinable())
        return;
    impl_->emit("Connecting to Asteroide RTC...");
    impl_->worker = std::jthread([state = impl_.get()](std::stop_token stop) { state->work(stop); });
}

void RtcChatSource::poll()
{
    if (!deliver)
        return;
    std::deque<lunar::ui::ChatMessage> messages;
    {
        const std::scoped_lock lock(impl_->mutex);
        messages.swap(impl_->incoming);
    }
    for (const auto& message : messages)
        deliver(message);
}

void RtcChatSource::send(std::string_view text)
{
    if (text.size() > 1024) {
        impl_->emit("RTC message is too long.", true);
        return;
    }
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return;
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    if (text == "/name") {
        impl_->emit(std::format("RTC name: {}. Use /name <name> to choose one, or /name reset for your Roblox name.",
                                username()));
        return;
    }
    if (text.starts_with("/name ")) {
        std::string_view chosen = text.substr(6);
        const auto start = chosen.find_first_not_of(' ');
        chosen = start == std::string_view::npos ? std::string_view{} : chosen.substr(start);
        if (chosen == "reset") {
            impl_->customName.clear();
        } else {
            const auto allowed = [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            };
            if (chosen.empty() || chosen.size() > 32 || !std::ranges::all_of(chosen, allowed)) {
                impl_->emit("RTC names must use 1-32 letters, numbers, or underscores.", true);
                return;
            }
            impl_->customName = chosen;
        }
        restart();
        impl_->emit(std::format("RTC name set to {}.", username()));
        return;
    }
    std::string message = rtcMessage(text);
    if (message.empty())
        return;
    bool queued = false;
    {
        const std::scoped_lock lock(impl_->mutex);
        if (impl_->connected && impl_->outgoing.size() < 16) {
            impl_->outgoing.push_back(std::move(message));
            queued = true;
        }
    }
    if (queued)
        impl_->wake.notify_all();
    else
        impl_->emit("RTC is disconnected or busy; your message was not sent.", true);
}

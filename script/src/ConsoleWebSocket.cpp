// The console's WebSocket library (Console.cpp opens it): WebSocket.connect(url)
// and the object it answers with -- OnMessage(message) and OnClose() to
// connect to, Send(message) and Close() -- as the sUNC docs give it, ws://
// and wss:// both.
//
// The socket is the assets library's (curl's), which is used from one thread:
// a worker of its own connects, sends what the script queued and polls for
// messages. connect yields the calling thread until the worker has connected
// (or failed to, which raises in it), as a web request yields. The messages
// come back to the script's side once a frame, where they fire OnMessage; the
// worker ending (Close, the server closing, the line dropping) fires OnClose
// once. The two events are a hidden Folder's, so they are RBXScriptSignals
// like any other: Connect, Once and Wait all work on them.

#include "Console.hpp"
#include "Runtime.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/assets/WebSocket.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace lunar::lua {

namespace {

// Past the tags Runtime.hpp names (and the Drawing's).
constexpr int WebSocketTag = 65;

// What the worker and the script's side share.
struct Socket {
    std::mutex mutex;
    std::deque<std::string> outgoing; // for the worker to send
    std::deque<std::string> incoming; // for OnMessage
    std::string error;                // why it never connected
    std::atomic<bool> connected{false};
    std::atomic<bool> finished{false}; // the worker is done: never connected, or closed
    std::atomic<bool> closing{false};  // Close() asked it to stop
};

void work(const std::shared_ptr<Socket>& socket, const std::string& url)
{
    std::stop_source stop;
    auto connected = assets::connectWebSocket(url, stop.get_token());
    if (!connected) {
        {
            const std::scoped_lock lock(socket->mutex);
            socket->error = connected.error();
        }
        socket->finished.store(true, std::memory_order_release);
        return;
    }
    assets::WebSocket& ws = **connected;
    socket->connected.store(true, std::memory_order_release);
    bool open = true;
    while (open && !socket->closing.load(std::memory_order_acquire)) {
        std::deque<std::string> out;
        {
            const std::scoped_lock lock(socket->mutex);
            out.swap(socket->outgoing);
        }
        bool busy = !out.empty();
        for (const std::string& text : out)
            open = open && ws.send(text, stop.get_token()).has_value();
        auto received = ws.receive();
        if (!received) {
            open = false; // a close is an error to it
        } else if (*received) {
            const std::scoped_lock lock(socket->mutex);
            socket->incoming.push_back(std::move(**received));
            busy = true;
        }
        if (!busy)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    socket->finished.store(true, std::memory_order_release);
}

struct Handle {
    std::shared_ptr<Socket> socket;
    InstancePtr events; // the hidden Folder whose OnMessage and OnClose these are
};

Handle& checkHandle(lua_State* L, int idx)
{
    if (auto* handle = static_cast<Handle*>(lua_touserdatatagged(L, idx, WebSocketTag)))
        return *handle;
    luaL_typeerror(L, idx, "WebSocket");
}

// Once a frame until the worker is done: the messages that came in fire
// OnMessage in order, and the end fires OnClose.
void pump(Runtime& rt, std::shared_ptr<Socket> socket, InstancePtr events)
{
    rt.after(0.0, [&rt, socket = std::move(socket), events = std::move(events)] mutable {
        const bool finished = socket->finished.load(std::memory_order_acquire);
        std::deque<std::string> in;
        {
            const std::scoped_lock lock(socket->mutex);
            in.swap(socket->incoming);
        }
        for (std::string& message : in) {
            const std::array arguments{Value(std::move(message))};
            events->fireEvent("OnMessage", arguments);
        }
        if (finished) {
            events->fireEvent("OnClose");
            return;
        }
        pump(rt, std::move(socket), std::move(events));
    });
}

// Send(message): queued for the worker, which sends it as a text frame.
int send(lua_State* L)
{
    Handle& handle = checkHandle(L, 1);
    const std::string_view message = checkString(L, 2);
    if (handle.socket->finished.load(std::memory_order_acquire) || handle.socket->closing.load())
        raise(L, "the WebSocket is closed");
    const std::scoped_lock lock(handle.socket->mutex);
    handle.socket->outgoing.emplace_back(message);
    return 0;
}

// Close(): the worker stops and drops the connection; OnClose fires on the
// next frame.
int close(lua_State* L)
{
    checkHandle(L, 1).socket->closing.store(true, std::memory_order_release);
    return 0;
}

int socketIndex(lua_State* L)
{
    const Handle& handle = checkHandle(L, 1);
    const std::string_view key = checkString(L, 2);
    if (key == "OnMessage" || key == "OnClose") {
        pushHubSignal(L, *handle.events, std::string(key));
        return 1;
    }
    if (key == "Send") {
        lua_pushcfunction(L, send, "Send");
        return 1;
    }
    if (key == "Close") {
        lua_pushcfunction(L, close, "Close");
        return 1;
    }
    raise(L, std::format("{} is not a valid member of WebSocket", key));
}

// WebSocket.connect(url): yields until connected, then the WebSocket.
int connect(lua_State* L)
{
    std::string url{checkString(L, 1)};
    if (!url.starts_with("ws://") && !url.starts_with("wss://"))
        luaL_argerror(L, 1, "is not a ws:// or wss:// URL");
    checkYieldable(L);
    auto socket = std::make_shared<Socket>();
    std::thread([socket, url] { work(socket, url); }).detach();
    Runtime& rt = Runtime::of(L);
    const std::uint64_t token = rt.suspend(L);
    // Polled from the next frame on, so the thread has yielded before it can
    // be woken.
    struct Wait {
        static void poll(Runtime& rt, lua_State* L, std::uint64_t token, std::shared_ptr<Socket> socket,
                         std::string url)
        {
            rt.after(0.0, [&rt, L, token, socket = std::move(socket), url = std::move(url)] mutable {
                if (!rt.isSuspended(L, token)) {
                    socket->closing.store(true);
                    return;
                }
                if (socket->connected.load(std::memory_order_acquire)) {
                    InstancePtr events = ClassRegistry::create("Folder");
                    rt.wake(L, token, [&](lua_State* thread) {
                        void* memory = lua_newuserdatataggedwithmetatable(thread, sizeof(Handle), WebSocketTag);
                        new (memory) Handle{socket, events};
                        return 1;
                    }, "WebSocket.connect");
                    pump(rt, socket, events);
                    return;
                }
                if (socket->finished.load(std::memory_order_acquire)) {
                    std::string error;
                    {
                        const std::scoped_lock lock(socket->mutex);
                        error = socket->error;
                    }
                    rt.fail(L, token, std::format("WebSocket.connect({}) failed: {}", url, error), "WebSocket.connect");
                    return;
                }
                poll(rt, L, token, std::move(socket), std::move(url));
            });
        }
    };
    Wait::poll(rt, L, token, std::move(socket), std::move(url));
    return lua_yield(L, 0);
}

} // namespace

void openConsoleWebSocket(lua_State* thread)
{
    // The metatable is the VM's, made the first time a console's globals are.
    lua_getuserdatametatable(thread, WebSocketTag);
    const bool made = !lua_isnil(thread, -1);
    lua_pop(thread, 1);
    if (!made) {
        lua_newtable(thread);
        lua_pushcfunction(thread, socketIndex, "__index");
        lua_setfield(thread, -2, "__index");
        // Dropped by every script, it closes: the worker stops, and the pump
        // (which holds the socket) fires OnClose to no one.
        registerType(thread, WebSocketTag, "WebSocket", [](lua_State*, void* memory) {
            auto* handle = static_cast<Handle*>(memory);
            handle->socket->closing.store(true, std::memory_order_release);
            handle->~Handle();
        });
    }
    lua_newtable(thread);
    lua_pushcfunction(thread, connect, "connect");
    lua_setfield(thread, -2, "connect");
    lua_setglobal(thread, "WebSocket");
}

} // namespace lunar::lua

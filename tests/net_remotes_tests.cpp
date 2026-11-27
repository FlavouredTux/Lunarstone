// Remotes on a live server: the argument values remotes carry (tables and
// enums inside variants), the RemoteBridge between a Replica and the
// client's scripts, and a ClientSession's remotes through its sink.

#include "framework.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/net/Remotes.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/Values.hpp>
#include <lunar/script/PlaySession.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <vector>

using namespace lunar;
using namespace lunar::net;

namespace {

std::vector<std::uint8_t> unhex(std::string_view hex)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    return out;
}

template <class T>
std::shared_ptr<T> make(const std::string& name, Instance* parent)
{
    auto instance = std::make_shared<T>();
    instance->setName(name);
    instance->setParent(parent);
    return instance;
}

RemoteValue table(bool dictionary, std::vector<std::pair<std::string, RemoteValue>> members)
{
    return RemoteValue{Value{}, std::make_shared<RemoteTable>(RemoteTable{dictionary, std::move(members)})};
}

std::string text(const RemoteValue& v)
{
    return describeRemoteArguments(std::span(&v, 1), 1000);
}

} // namespace

TEST(net_remote_dictionary_matches_a_capture)
{
    // The argument of ReplicatedStorage.meow.OnClientEvent as a live server
    // sent it ("Flex Your FPS And Ping!", place 18667984660): a Dictionary of
    // four, string keys as varint-length strings, numbers as doubles.
    const auto bytes = unhex("1f04"
                             "06736563726574"
                             "0c0000003c412a9741"
                             "05746f6b656e"
                             "0c000070f542530042"
                             "0174"
                             "0206646576696365"
                             "027672"
                             "0c0000000000000840");
    NetworkStream in(bytes);
    std::string rendered;
    auto v = readRemoteVariant(in, {}, &rendered);
    CHECK(v.has_value());
    CHECK(!in.bad() && in.left() == 0);
    CHECK_EQ(text(*v), std::string("({secret = 97161295, token = 8764546734, t = \"device\", vr = 3})"));
    CHECK(rendered.starts_with("Dictionary: Dictionary[4](secret = double: 97161295"));
    // Written back member for member, it's the same bytes.
    NetworkStream out;
    CHECK(writeVariant(out, *v));
    CHECK(std::ranges::equal(out.bytes(), bytes));
    // The plain reader still gives nil for a table.
    NetworkStream again(bytes);
    auto plain = readVariant(again);
    CHECK(plain.has_value() && std::holds_alternative<std::monostate>(*plain));
}

TEST(net_remote_tables_and_enums_round_trip)
{
    const EnumInfo* material = enums::find("Material");
    CHECK(material != nullptr);
    ValueContext context;
    context.enumIdOf = [material](const EnumInfo& e) -> std::optional<std::uint16_t> {
        return &e == material ? std::optional<std::uint16_t>(300) : std::nullopt;
    };
    context.enumById = [material](std::uint16_t id) { return id == 300 ? material : nullptr; };
    const RemoteValue v = table(false, {{"", remoteValue(Value(1.0))},
                                        {"", remoteValue(Value(std::string("a")))},
                                        {"", table(true, {{"pos", remoteValue(Value(Vector3{1, 2, 3}))},
                                                          {"empty", table(false, {})}})},
                                        {"", remoteValue(Value(*material->find("Plastic")))}});
    NetworkStream out;
    CHECK(writeVariant(out, v, context));
    const auto bytes = out.bytes();
    CHECK_EQ(bytes[0], nettype::Array);
    CHECK_EQ(bytes[1], 4);
    // The EnumItem: type 7, the schema's enum id (u16 300), the value (Plastic = 256 as a varint).
    const std::vector<std::uint8_t> tail{nettype::Enum, 0x2c, 0x01, 0x80, 0x02};
    CHECK(std::ranges::equal(bytes.last(tail.size()), tail));
    NetworkStream in(bytes);
    auto back = readRemoteVariant(in, context);
    CHECK(back.has_value() && in.left() == 0);
    CHECK_EQ(text(*back), std::string("({1, \"a\", {pos = 1, 2, 3, empty = {}}, Enum.Material.Plastic})"));
}

TEST(net_remote_event_items_carry_tables)
{
    // A Tuple of a number, a dictionary and an array through an Event
    // Invocation item, both ways.
    auto game = DataModel::create();
    auto storage = game->getService("ReplicatedStorage");
    auto ping = make<RemoteEvent>("Ping", storage);
    ReplicationServer server(game, {"ReplicatedStorage"});
    const Schema& schema = server.schema();
    const auto eid = schema.eventId(*schema.classId("RemoteEvent"), "OnRemoteServerEvent");
    CHECK(eid.has_value());
    const std::vector<RemoteValue> args{remoteValue(Value(1.0)), table(true, {{"x", remoteValue(Value(2.0))}}),
                                        table(false, {{"", remoteValue(Value(std::string("a")))}})};
    const std::array<Item, 1> items{eventInvocation(server.idOf(ping.get()), *eid, args)};
    auto message = encodeData(items, schema, server.context(), {}, ItemContext{false});
    auto decoded = decodeData(message, schema, server.context(), nullptr, {}, ItemContext{false});
    CHECK(decoded.has_value() && decoded->size() == 1);
    const auto& e = std::get<EventInvocationItem>(decoded->front());
    CHECK_EQ(e.arguments.size(), std::size_t{3});
    CHECK_EQ(e.tables.size(), std::size_t{2});
    CHECK_EQ(describeRemoteArguments(remoteArguments(e)), std::string("(1, {x = 2}, {\"a\"})"));
}

namespace {

// A client of a ReplicationServer whose scripts' remotes go through a RemoteBridge.
struct RemoteRig {
    std::shared_ptr<DataModel> game = DataModel::create();
    Instance* storage = game->getService("ReplicatedStorage");
    std::shared_ptr<RemoteEvent> ping = make<RemoteEvent>("Ping", storage);
    std::shared_ptr<UnreliableRemoteEvent> fast = make<UnreliableRemoteEvent>("Fast", storage);
    std::shared_ptr<RemoteFunction> add = make<RemoteFunction>("Add", storage);
    std::shared_ptr<RemoteFunction> ask = make<RemoteFunction>("Ask", storage);
    std::shared_ptr<Sound> music = make<Sound>("Music", storage);
    std::unique_ptr<ReplicationServer> server;
    std::shared_ptr<Replica> replica = std::make_shared<Replica>();
    std::unique_ptr<RemoteBridge> bridge;
    std::unique_ptr<ScriptContext> client;
    std::vector<std::string> lines;

    RemoteRig()
    {
        server = std::make_unique<ReplicationServer>(game, std::vector<std::string>{"ReplicatedStorage"});
        for (const auto& m : server->joinMessages())
            replica->handle(m);
        bridge = std::make_unique<RemoteBridge>(replica);
        bridge->log = [this](const std::string& line) { lines.push_back(line); };
        client = std::make_unique<ScriptContext>(replica->game(), ScriptRole::Client);
        client->setOutput([this](MessageType type, std::string_view message) {
            if (type != MessageType::Info)
                lines.push_back(std::format("{}{}",
                                            type == MessageType::Error     ? "error: "
                                            : type == MessageType::Warning ? "warning: "
                                                                           : "",
                                            message));
        });
        client->setRemoteSink([this](Instance& remote, std::string_view event, std::vector<RemoteValue> args) {
            bridge->send(remote, event, std::move(args));
        });
        bridge->setReceiver([this](Instance& remote, std::string_view event, std::span<const RemoteValue> args) {
            client->receiveRemote(remote, event, args);
        });
    }

    // What the client sent, as the server reads it: "Ping.OnRemoteServerEvent(1, ...)".
    std::vector<std::string> serverReads(bool* unreliable = nullptr)
    {
        std::vector<std::string> out;
        for (auto& m : bridge->take()) {
            if (!m.reliable && unreliable)
                *unreliable = true;
            auto items = decodeData(m.message, server->schema(), server->context(), nullptr, {}, ItemContext{false});
            CHECK(items.has_value());
            for (const Item& item : *items)
                if (const auto* e = std::get_if<EventInvocationItem>(&item)) {
                    auto target = server->find(e->id);
                    out.push_back(std::format("{}.{}{}", target ? target->name() : "?",
                                              server->schema().events[e->eventId].name,
                                              describeRemoteArguments(remoteArguments(*e))));
                }
        }
        return out;
    }

    void fromServer(const Instance& remote, std::string_view event, std::vector<RemoteValue> args)
    {
        const Schema& schema = server->schema();
        const auto eid = schema.eventId(*schema.classId(remote.className()), event);
        CHECK(eid.has_value());
        const std::array<Item, 1> items{eventInvocation(server->idOf(&remote), *eid, args)};
        CHECK(replica->handle(encodeData(items, schema, server->context())));
    }

    void steps(int n)
    {
        for (int i = 0; i < n; ++i)
            client->step(1.0 / 60.0);
    }

    bool has(std::string_view line) const { return std::ranges::find(lines, line) != lines.end(); }
};

} // namespace

TEST(net_remote_bridge_fire_server_and_on_client_event)
{
    RemoteRig rig;
    // The server's event arrives before anyone listens: it waits for the listener.
    rig.fromServer(*rig.ping, "OnClientEvent",
                   {remoteValue(Value(std::string("hi"))), table(true, {{"n", remoteValue(Value(3.0))}})});
    CHECK(rig.client->run(R"(
        local storage = game:GetService("ReplicatedStorage")
        storage.Ping.OnClientEvent:Connect(function(word, t) print("got", word, t.n) end)
        storage.Ping:FireServer(1, {x = 2, [5] = "five"}, {"a", "b"}, nil, workspace)
        storage.Fast:FireServer(Vector3.new(1, 2, 3))
    )",
                           "Test")
              .has_value());
    rig.steps(2);
    CHECK(rig.has("got hi 3"));
    bool unreliable = false;
    const auto reads = rig.serverReads(&unreliable);
    CHECK_EQ(reads.size(), std::size_t{2});
    // Keys that aren't strings become strings; workspace isn't replicated here, so it goes as nil.
    CHECK(reads[0] == "Ping.OnRemoteServerEvent(1, {x = 2, 5 = \"five\"}, {\"a\", \"b\"}, nil, nil)"
          || reads[0] == "Ping.OnRemoteServerEvent(1, {5 = \"five\", x = 2}, {\"a\", \"b\"}, nil, nil)");
    CHECK_EQ(reads[1], std::string("Fast.OnRemoteServerEvent(1, 2, 3)"));
    CHECK(unreliable);
    CHECK_EQ(rig.bridge->sent, std::size_t{2});
    CHECK_EQ(rig.bridge->received, std::size_t{1});
}

// A Random goes as its generator's state and arrives a Random that goes on
// from there, and back the same way: the game hands its clients one to flicker
// and shatter a room's lights with, and called NextInteger on a number.
TEST(net_remote_bridge_carries_a_random)
{
    RemoteRig rig;
    const RemoteValue random{.random = 0x123456789abcdef0ULL};
    rig.fromServer(*rig.ping, "OnClientEvent", {remoteValue(Value(std::string("shatter"))), random, random});
    CHECK(rig.client
              ->run(R"(
        local storage = game:GetService("ReplicatedStorage")
        storage.Ping.OnClientEvent:Connect(function(name, a, b)
            storage.Ping:FireServer(a)
            print(name, typeof(a), a:NextInteger(1, 1000000000) == b:NextInteger(1, 1000000000))
        end)
    )",
                    "Test")
              .has_value());
    rig.steps(2);
    CHECK(rig.has("shatter Random true"));
    const auto reads = rig.serverReads();
    CHECK_EQ(reads.size(), std::size_t{1});
    CHECK_EQ(reads[0], std::string("Ping.OnRemoteServerEvent(Random(0x123456789abcdef0))"));
}

// The server's Play and Stop come as events on the Sound, not as its
// Playing [live: the game]; a Play starts it over, and the server can move it.
TEST(net_server_plays_and_stops_a_sound_by_its_events)
{
    RemoteRig rig;
    auto* sound = dynamic_cast<Sound*>(rig.replica->game()->getService("ReplicatedStorage")->findFirstChild("Music"));
    CHECK(sound != nullptr);
    CHECK(!sound->playing());
    sound->setTimePosition(4.0);
    rig.fromServer(*rig.music, "playbackActionSync", {remoteValue(Value(0.0))});
    rig.fromServer(*rig.music, "PlayingUpdatedFromServer", {remoteValue(Value(true))});
    CHECK(sound->playing());
    CHECK_EQ(sound->timePosition(), 0.0);
    rig.fromServer(*rig.music, "TimePositionUpdatedFromServer", {remoteValue(Value(92.45)), remoteValue(Value(92.45))});
    CHECK_EQ(sound->timePosition(), 92.45);
    rig.fromServer(*rig.music, "playbackActionSync", {remoteValue(Value(3.0))});
    rig.fromServer(*rig.music, "PlayingUpdatedFromServer", {remoteValue(Value(false))});
    CHECK(!sound->playing());
}

TEST(net_remote_bridge_invoke_server)
{
    RemoteRig rig;
    CHECK(rig.client->run(R"(
        local storage = game:GetService("ReplicatedStorage")
        task.spawn(function() print("sum", storage.Add:InvokeServer(2, 3)) end)
        task.spawn(function()
            local ok, message = pcall(function() return storage.Add:InvokeServer("boom") end)
            print("second", ok, message)
        end)
    )",
                           "Test")
              .has_value());
    rig.steps(1);
    // Ids count per RemoteFunction from 1; there is no LocalPlayer here, so the player goes as nil.
    const auto reads = rig.serverReads();
    CHECK_EQ(reads.size(), std::size_t{2});
    CHECK_EQ(reads[0], std::string("Add.RemoteOnInvokeServer(1, nil, 2, 3)"));
    CHECK_EQ(reads[1], std::string("Add.RemoteOnInvokeServer(2, nil, \"boom\")"));
    // Answered out of order: the error first.
    rig.fromServer(*rig.add, "RemoteOnInvokeError", {remoteValue(Value(2.0)), remoteValue(Value(std::string("no")))});
    rig.fromServer(*rig.add, "RemoteOnInvokeSuccess", {remoteValue(Value(1.0)), remoteValue(Value(5.0))});
    rig.steps(2);
    CHECK(rig.has("sum 5"));
    // The server's message, raised where the script called InvokeServer, as it came.
    CHECK(rig.has("second false no"));
}

TEST(net_remote_bridge_answers_invoke_client)
{
    RemoteRig rig;
    // The invocation comes before OnClientInvoke is set: it waits for it.
    rig.fromServer(*rig.ask, "RemoteOnInvokeClient",
                   {remoteValue(Value(7.0)), remoteValue(Value(4.0)), remoteValue(Value(5.0))});
    rig.steps(1);
    CHECK(rig.serverReads().empty());
    CHECK(rig.client->run(R"(
        local storage = game:GetService("ReplicatedStorage")
        storage.Ask.OnClientInvoke = function(a, b)
            if a == 0 then error("zero") end
            task.wait()
            return a + b, {a, b}
        end
    )",
                           "Test")
              .has_value());
    rig.steps(3);
    rig.fromServer(*rig.ask, "RemoteOnInvokeClient", {remoteValue(Value(8.0)), remoteValue(Value(0.0))});
    rig.steps(2);
    const auto reads = rig.serverReads();
    CHECK_EQ(reads.size(), std::size_t{2});
    CHECK_EQ(reads[0], std::string("Ask.RemoteOnInvokeSuccess(7, 9, {4, 5})"));
    CHECK(reads[1].starts_with("Ask.RemoteOnInvokeError(8, \"") && reads[1].contains("zero"));
}

TEST(script_client_session_remotes_go_through_the_sink)
{
    // A ClientSession's LocalScript: FireServer and InvokeServer reach the
    // sink as the network's events, with our Player in InvokeServer's.
    auto game = DataModel::create();
    auto players = game->getService("Players");
    auto player = make<Player>("Me", players);
    player->setUserId(42);
    auto storage = game->getService("ReplicatedStorage");
    auto ping = make<RemoteEvent>("Ping", storage);
    auto add = make<RemoteFunction>("Add", storage);
    auto script = std::make_shared<LocalScript>();
    script->setSource(R"(
        local storage = game:GetService("ReplicatedStorage")
        storage.Ping.OnClientEvent:Connect(function(x) print("client got", x) end)
        storage.Ping:FireServer(21)
        print("sum", storage.Add:InvokeServer(2, 3))
    )");
    // In our PlayerGui, where a live server puts the StarterGui copy.
    auto playerGui = make<PlayerGui>("PlayerGui", player.get());
    script->setParent(playerGui);
    lunar::ClientSession session(game);
    std::vector<std::string> lines, sent;
    session.client().setOutput([&](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    session.client().setRemoteSink([&](Instance& remote, std::string_view event, std::vector<RemoteValue> args) {
        sent.push_back(std::format("{}.{}{}", remote.name(), event, describeRemoteArguments(args)));
    });
    CHECK(session.start(42));
    session.step(1.0 / 60.0);
    CHECK_EQ(sent.size(), std::size_t{2});
    CHECK_EQ(sent[0], std::string("Ping.OnRemoteServerEvent(21)"));
    CHECK_EQ(sent[1], std::string("Add.RemoteOnInvokeServer(1, Players.Me, 2, 3)"));
    CHECK(session.client().receiveRemote(*ping, "OnClientEvent", std::vector{remoteValue(Value(42.0))}));
    CHECK(session.client().receiveRemote(*add, "RemoteOnInvokeSuccess",
                                         std::vector{remoteValue(Value(1.0)), remoteValue(Value(5.0))}));
    CHECK(!session.client().receiveRemote(*ping, "Changed", {}));
    session.step(1.0 / 60.0);
    session.step(1.0 / 60.0);
    CHECK(std::ranges::find(lines, std::string("client got 42")) != lines.end());
    CHECK(std::ranges::find(lines, std::string("sum 5")) != lines.end());
}

// A live server's default RbxCharacterSounds and PlayerScriptsLoader aren't
// Archivable, but our PlayerScripts gets them, AtomicBinding module and all,
// and they run; the shipped PlayerModule beside them stays out (the native
// controls do its work). Our new character's Humanoid becomes the camera's
// subject before its scripts run [live: The Mimic].
TEST(script_client_session_copies_the_default_player_scripts_and_aims_the_camera)
{
    auto game = DataModel::create();
    auto players = game->getService("Players");
    auto player = make<Player>("Me", players);
    player->setUserId(42);
    auto* starterPlayer = game->getService("StarterPlayer");
    auto starterScripts = make<StarterPlayerScripts>("StarterPlayerScripts", starterPlayer);
    auto sounds = std::make_shared<LocalScript>();
    sounds->setName("RbxCharacterSounds");
    sounds->setSource(
        R"(print("character sounds", script.Parent.Name, script:FindFirstChild("AtomicBinding") ~= nil))");
    auto binding = make<ModuleScript>("AtomicBinding", sounds.get());
    binding->setArchivable(false);
    sounds->setArchivable(false);
    sounds->setParent(starterScripts.get());
    auto loader = std::make_shared<LocalScript>();
    loader->setName("PlayerScriptsLoader");
    loader->setSource(R"(print("loader ran"))");
    loader->setArchivable(false);
    loader->setParent(starterScripts.get());
    auto module = std::make_shared<ModuleScript>();
    module->setName("PlayerModule");
    module->setSource("return {}");
    module->setArchivable(false);
    module->setParent(starterScripts.get());

    lunar::ClientSession session(game);
    std::vector<std::string> lines;
    session.client().setOutput([&](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    CHECK(session.start(42));
    session.step(1.0 / 60.0);
    CHECK(std::ranges::find(lines, std::string("character sounds PlayerScripts true")) != lines.end());
    CHECK(std::ranges::find(lines, std::string("loader ran")) != lines.end());
    CHECK(!sounds->archivable() && !binding->archivable());
    const Instance* playerScripts = player->findFirstChildOfClass("PlayerScripts");
    CHECK(playerScripts && !playerScripts->findFirstChild("PlayerModule"));

    auto character = make<Model>("Me", game->workspace());
    auto humanoid = make<Humanoid>("Humanoid", character.get());
    player->setCharacter(character.get());
    const Camera* camera = game->workspace()->currentCamera();
    CHECK(camera != nullptr);
    CHECK(camera && camera->cameraSubject() == humanoid.get());
}

// A slow server fills StarterPlayerScripts in two goes, the defaults first
// and the game's own scripts later [live: the game]: those reach
// PlayerScripts too, a step after they came, and run once.
TEST(script_client_session_copies_late_starter_player_scripts)
{
    auto game = DataModel::create();
    auto players = game->getService("Players");
    auto player = make<Player>("Me", players);
    player->setUserId(42);
    auto* starterPlayer = game->getService("StarterPlayer");
    auto starterScripts = make<StarterPlayerScripts>("StarterPlayerScripts", starterPlayer);
    auto early = make<LocalScript>("Early", starterScripts.get());
    early->setSource(R"(print("early", script.Parent.Name))");

    lunar::ClientSession session(game);
    std::vector<std::string> lines;
    session.client().setOutput([&](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    CHECK(session.start(42));
    session.step(1.0 / 60.0);

    auto late = std::make_shared<LocalScript>();
    late->setName("Client");
    late->setSource(R"(print("late", script.Parent.Name, script:FindFirstChild("Helper") ~= nil))");
    late->setParent(starterScripts.get());
    make<ModuleScript>("Helper", late.get()); // the rest of its packet
    session.step(1.0 / 60.0);
    session.step(1.0 / 60.0);
    CHECK_EQ(std::ranges::count(lines, std::string("early PlayerScripts")), 1);
    CHECK_EQ(std::ranges::count(lines, std::string("late PlayerScripts true")), 1);
    CHECK(std::ranges::find(lines, std::string("late StarterPlayerScripts true")) == lines.end());
}

// The legacy Mouse knows the view's size from the moment a script asks for
// it, and every frame after: Midnight Racing's map menu turns its camera by
// mouse.X / mouse.ViewSizeX, and 0 / 0 drew the map all black.
TEST(script_client_session_mouse_knows_the_view_size)
{
    auto game = DataModel::create();
    auto players = game->getService("Players");
    auto player = make<Player>("Me", players);
    player->setUserId(42);
    auto script = std::make_shared<LocalScript>();
    script->setSource(R"(
        local mouse = game:GetService("Players").LocalPlayer:GetMouse()
        local turn = mouse.X / mouse.ViewSizeX
        print("asked", mouse.ViewSizeX, mouse.ViewSizeY, turn == turn)
        game:GetService("RunService").RenderStepped:Wait()
        print("frame", mouse.ViewSizeX, mouse.ViewSizeY)
    )");
    script->setParent(make<PlayerGui>("PlayerGui", player.get()).get());
    lunar::ClientSession session(game);
    session.setViewportSize({1280.0f, 720.0f});
    std::vector<std::string> lines;
    session.client().setOutput([&](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    CHECK(session.start(42));
    session.step(1.0 / 60.0);
    session.step(1.0 / 60.0);
    CHECK(std::ranges::find(lines, std::string("asked 1280 720 true")) != lines.end());
    CHECK(std::ranges::find(lines, std::string("frame 1280 720")) != lines.end());
}

// Mouse.Hit and its family: the ray from the camera through the pointer
// (the middle of the view while it's locked there) [live: the game sends
// MousePos = mouse.Hit with every move; nil aimed them nowhere].
TEST(script_client_session_mouse_hit_follows_the_camera_ray)
{
    auto game = DataModel::create();
    auto players = game->getService("Players");
    auto player = make<Player>("Me", players);
    player->setUserId(42);
    auto script = std::make_shared<LocalScript>();
    script->setSource(R"(
        local camera = workspace.CurrentCamera
        camera.CameraType = Enum.CameraType.Scriptable
        camera.CFrame = CFrame.new(0, 5, 20)
        local wall = Instance.new("Part")
        wall.Name = "Wall"
        wall.Anchored = true
        wall.Size = Vector3.new(10, 10, 1)
        wall.Position = Vector3.new(0, 5, 0)
        wall.Parent = workspace
        game:GetService("UserInputService").MouseBehavior = Enum.MouseBehavior.LockCenter
        game:GetService("RunService").RenderStepped:Wait()
        local mouse = game:GetService("Players").LocalPlayer:GetMouse()
        local function r(x) return math.floor(x * 100 + 0.5) / 100 end
        print("hit", mouse.Target == wall, r(mouse.Hit.Position.Z), mouse.TargetSurface.Name,
            r(mouse.UnitRay.Direction.Z), r(mouse.Origin.Position.Z), r(mouse.Hit.LookVector.Z))
        mouse.TargetFilter = wall
        print("past", mouse.Target, r((mouse.Hit.Position - mouse.Origin.Position).Magnitude), mouse.TargetSurface.Name)
    )");
    script->setParent(make<PlayerGui>("PlayerGui", player.get()).get());
    lunar::ClientSession session(game);
    session.setViewportSize({1280.0f, 720.0f});
    // The wall's near face, for a 1000-stud ray straight down -Z that doesn't pass it by.
    session.client().setRaycastHook([](const DataModel& model, const RaycastQuery& query) -> std::optional<RaycastHit> {
        Instance* wall = model.findFirstChild("Workspace")->findFirstChild("Wall");
        if (!wall || std::abs(query.direction.z + 1000.0f) > 0.01f
            || std::ranges::find(query.filter, wall, &InstancePtr::get) != query.filter.end())
            return std::nullopt;
        return RaycastHit{wall->shared_from_this(), {0.0f, 5.0f, 0.5f}, {0.0f, 0.0f, 1.0f}};
    });
    std::vector<std::string> lines;
    session.client().setOutput([&](MessageType type, std::string_view text) {
        if (type != MessageType::Info)
            lines.emplace_back(text);
    });
    CHECK(session.start(42));
    for (int i = 0; i < 3; ++i)
        session.step(1.0 / 60.0);
    CHECK(std::ranges::find(lines, std::string("hit true 0.5 Back -1 20 -1")) != lines.end());
    CHECK(std::ranges::find(lines, std::string("past nil 1000 Right")) != lines.end());
}

TEST(net_remote_bridge_unheard_events_queue_up_to_a_limit)
{
    RemoteRig rig;
    for (int i = 1; i <= 300; ++i)
        rig.fromServer(*rig.ping, "OnClientEvent", {remoteValue(Value(static_cast<double>(i)))});
    rig.steps(1);
    CHECK(rig.has("warning: Remote event invocation queue exhausted for ReplicatedStorage.Ping; did you forget to "
                  "implement OnClientEvent? (1 events dropped)"));
    // A listener gets what was kept, oldest first.
    CHECK(rig.client
              ->run(R"(
        local count, first, last = 0, nil, nil
        game:GetService("ReplicatedStorage").Ping.OnClientEvent:Connect(function(i)
            count += 1
            first = first or i
            last = i
        end)
        task.delay(0.1, function() print("heard", count, first, last) end)
    )",
                    "Test")
              .has_value());
    rig.steps(10);
    CHECK(rig.has("heard 256 1 256"));
}

// A call over the client's send limits isn't sent: the script hears nothing of
// it, an invocation never returns, and what comes after goes as usual [live:
// sUNC's last call, 2 GB of strings in one table 2109 times, got the connection
// dropped when sent].
TEST(net_remote_bridge_drops_calls_over_the_send_limits)
{
    RemoteRig rig;
    CHECK(rig.client->run(R"(
        local storage = game:GetService("ReplicatedStorage")
        local megabyte = string.rep("a", 1000000)
        local strings = table.create(101, table.create(1, megabyte))
        storage.Ping:FireServer(strings)
        local values = table.create(5000, table.create(1000, true))
        storage.Ping:FireServer(values)
        task.spawn(function()
            local answer = storage.Add:InvokeServer(strings)
            print("returned", answer)
        end)
        task.spawn(function() storage.Add:InvokeServer("next") end)
        storage.Ping:FireServer("after")
    )",
                           "Test")
              .has_value());
    rig.steps(2);
    CHECK(std::ranges::none_of(rig.lines, [](const std::string& line) { return line.starts_with("error:"); }));
    const auto reads = rig.serverReads();
    CHECK_EQ(reads.size(), std::size_t{2});
    // The unsent invocation had its id.
    CHECK_EQ(reads[0], std::string("Add.RemoteOnInvokeServer(2, nil, \"next\")"));
    CHECK_EQ(reads[1], std::string("Ping.OnRemoteServerEvent(\"after\")"));
    rig.steps(10);
    CHECK(std::ranges::none_of(rig.lines, [](const std::string& line) { return line.starts_with("returned"); }));
}

// An event can name an instance a New item earlier in the same message
// made: it arrives as that instance, not nil [capture: a grab effect and the RootAnchor accessory made just before it].
TEST(net_remote_event_sees_an_instance_made_earlier_in_its_message)
{
    RemoteRig rig;
    CHECK(rig.client->run(R"(
        game:GetService("ReplicatedStorage").Ping.OnClientEvent:Connect(function(t)
            print("anchor", t.Anchor and t.Anchor.Name)
        end)
    )",
                           "Test")
              .has_value());
    auto anchor = make<Part>("RootAnchor", rig.storage);
    const Schema& schema = rig.server->schema();
    const auto eid = schema.eventId(*schema.classId("RemoteEvent"), "OnClientEvent");
    // The New item the server would send for it, then the event, in one message.
    auto made = decodeData(rig.server->addInstance(anchor), schema, rig.server->context(), nullptr, {},
                           ItemContext{true});
    CHECK(made.has_value() && made->size() == 1);
    std::vector<Item> items = std::move(*made);
    items.emplace_back(eventInvocation(rig.server->idOf(rig.ping.get()), *eid,
                                       std::vector{table(true, {{"Anchor", remoteValue(Value(InstancePtr(anchor)))}})}));
    CHECK(rig.replica->handle(encodeData(items, schema, rig.server->context())));
    rig.steps(1);
    CHECK(rig.has("anchor RootAnchor"));
}

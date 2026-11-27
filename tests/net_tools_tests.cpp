// A client's tools on a live server: what net::ToolReplicator sends as a
// ClientSession equips, uses and puts away a tool from its Backpack, read
// back the way the server reads it, and what it leaves alone (the server's
// own moves, other players' tools).

#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/Tools.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/net/ToolReplication.hpp>
#include <lunar/script/PlaySession.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <string>
#include <vector>

using namespace lunar;
using namespace lunar::net;

namespace {

template <class T>
std::shared_ptr<T> make(const std::string& name, Instance* parent)
{
    auto instance = std::make_shared<T>();
    instance->setName(name);
    instance->setParent(parent);
    return instance;
}

std::shared_ptr<Part> makePart(const std::string& name, const CFrame& cframe, Instance* parent)
{
    auto part = make<Part>(name, parent);
    part->setSize({1, 1, 1});
    part->setCFrame(cframe);
    return part;
}

// An R15-ish character with what holding a tool needs.
std::shared_ptr<Model> makeCharacter(const std::string& name, Vector3 at, Instance* parent)
{
    auto character = make<Model>(name, parent);
    make<Humanoid>("Humanoid", character.get())->setRigType(Enum::HumanoidRigType::R15);
    makePart("HumanoidRootPart", CFrame(at), character.get());
    makePart("UpperTorso", CFrame(at), character.get());
    makePart("RightHand", CFrame(at + Vector3{1.5f, -1, 0}), character.get());
    return character;
}

std::shared_ptr<Tool> makeTool(const std::string& name, Instance* parent)
{
    auto tool = std::make_shared<Tool>();
    tool->setName(name);
    tool->setGrip(CFrame(0, -1, 0));
    makePart("Handle", CFrame(20, 1, 0), tool.get());
    tool->setParent(parent);
    return tool;
}

// A server game (two players, ours with a sign in its Backpack), its
// replica on the client with a ClientSession over it, and the ToolReplicator.
struct ToolRig {
    std::shared_ptr<DataModel> game = DataModel::create();
    std::shared_ptr<Player> player = make<Player>("Me", game->getService("Players"));
    std::shared_ptr<Backpack> backpack = make<Backpack>("Backpack", player.get());
    std::shared_ptr<Tool> sign = makeTool("Sign", backpack.get());
    std::shared_ptr<Model> character = makeCharacter("Me", {0, 3, 0}, game->workspace());
    std::shared_ptr<Player> otherPlayer = make<Player>("Other", game->getService("Players"));
    std::shared_ptr<Model> other = makeCharacter("Other", {10, 3, 0}, game->workspace());
    std::shared_ptr<Tool> theirs = makeTool("Theirs", make<Backpack>("Backpack", otherPlayer.get()).get());
    std::unique_ptr<ReplicationServer> server;
    std::shared_ptr<Replica> replica = std::make_shared<Replica>();
    std::unique_ptr<lunar::ClientSession> client;
    std::unique_ptr<ToolReplicator> tools;
    std::vector<std::string> log;

    ToolRig()
    {
        player->setUserId(5);
        player->setCharacter(character.get());
        otherPlayer->setUserId(6);
        otherPlayer->setCharacter(other.get());
        server = std::make_unique<ReplicationServer>(game, std::vector<std::string>{"Workspace", "Players"});
        for (const auto& m : server->joinMessages())
            replica->handle(m);
        client = std::make_unique<lunar::ClientSession>(replica->game());
        CHECK(client->start(5));
        tools = std::make_unique<ToolReplicator>(replica);
        tools->log = [this](const std::string& line) { log.push_back(line); };
    }

    // The replica's copy of a server instance.
    template <class T = Instance>
    T* mine(const Instance& original)
    {
        return dynamic_cast<T*>(replica->find(server->idOf(&original)).get());
    }

    // What the client sent, one line per item, as the server reads it.
    std::vector<std::string> sent()
    {
        std::vector<std::string> out;
        auto name = [this](InstanceId id) {
            if (id.scope == replica->globals->peerId)
                return std::format("ours:{}", id.index);
            auto inst = server->find(id);
            return inst ? inst->fullName() : std::format("{}:{}", id.scope, id.index);
        };
        const Schema& schema = server->schema();
        for (const auto& message : tools->take()) {
            auto items = decodeData(message, schema, server->context(), nullptr, {}, ItemContext{false});
            CHECK(items.has_value());
            for (const Item& item : *items) {
                if (const auto* c = std::get_if<ChangePropertyItem>(&item)) {
                    CHECK(c->parent.has_value() && !c->flag);
                    out.push_back(std::format("{}.Parent = {}", name(c->id), name(c->parent.value_or(InstanceId{}))));
                } else if (const auto* n = std::get_if<NewInstanceItem>(&item)) {
                    std::string line = std::format("new {} {} in {}:", schema.classes[n->classId].name, name(n->id),
                                                   name(n->parent));
                    for (const auto* pass : {&n->properties, &n->refProperties})
                        for (const auto& [id, value] : *pass) {
                            const auto* ref = std::get_if<InstancePtr>(&value);
                            line += std::format(" {}={}", schema.properties[id].name,
                                                ref ? (*ref ? (*ref)->fullName() : "nil") : toString(value));
                        }
                    out.push_back(line);
                } else if (const auto* d = std::get_if<DeleteItem>(&item)) {
                    out.push_back(std::format("delete {}", name(d->id)));
                } else if (const auto* e = std::get_if<EventInvocationItem>(&item)) {
                    out.push_back(std::format("{}.{}", name(e->id), schema.events[e->eventId].name));
                }
            }
        }
        return out;
    }

    // The server moves one of its instances; the client hears it.
    void serverMoves(Instance& instance, Instance* to)
    {
        instance.setParent(to);
        ChangePropertyItem change;
        change.id = server->idOf(&instance);
        change.parent = to ? server->idOf(to) : InstanceId{};
        const std::array<Item, 1> items{change};
        CHECK(replica->handle(encodeData(items, server->schema(), server->context())));
    }
};

} // namespace

TEST(net_tools_equip_use_and_put_away)
{
    ToolRig rig;
    CHECK(rig.sent().empty());
    CHECK(rig.client->selectTool(0));
    // The tool's Parent first, then the weld the server waits for, in our own
    // id scope: the hand to the handle, C0 the grip offset, C1 the tool's Grip.
    auto equipped = rig.sent();
    CHECK_EQ(equipped.size(), std::size_t{2});
    CHECK_EQ(equipped[0], std::string("Players.Me.Backpack.Sign.Parent = Workspace.Me"));
    CHECK(equipped[1].starts_with("new Weld ours:1 in Workspace.Me.RightHand: Part0=Workspace.Me.RightHand "
                                  "Part1=Players.Me.Backpack.Sign.Handle "));
    CHECK(equipped[1].contains(" Name=RightGrip"));
    CHECK(equipped[1].contains(" C0=0, -0.2, 0, "));
    CHECK(equipped[1].contains(" C1=0, -1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1"));
    CHECK(rig.replica->find({rig.replica->globals->peerId, 1}) != nullptr);

    rig.client->toolButton(true);
    rig.client->toolButton(false);
    CHECK(rig.sent() == (std::vector<std::string>{"Players.Me.Backpack.Sign.Activated",
                                                  "Players.Me.Backpack.Sign.Deactivated"}));

    // Put away: the Parent, then the weld goes.
    rig.client->unequipTools();
    CHECK(rig.sent()
          == (std::vector<std::string>{"Players.Me.Backpack.Sign.Parent = Players.Me.Backpack", "delete ours:1"}));
    CHECK(rig.replica->find({rig.replica->globals->peerId, 1}) == nullptr);
    CHECK_EQ(rig.tools->sent, std::size_t{6});
}

TEST(net_tools_leave_the_servers_moves_alone)
{
    ToolRig rig;
    // The server equips our sign (a server script's EquipTool): it waits for
    // our weld, which we send; the move itself is the server's.
    rig.serverMoves(*rig.sign, rig.character.get());
    auto* sign = rig.mine<Tool>(*rig.sign);
    CHECK(sign != nullptr && sign->parent() == rig.mine(*rig.character));
    const auto welded = rig.sent();
    CHECK_EQ(welded.size(), std::size_t{1});
    CHECK(welded[0].starts_with("new Weld ours:1 in Workspace.Me.RightHand"));
    // And puts it back: our weld goes with our Delete, the move isn't echoed.
    rig.serverMoves(*rig.sign, rig.backpack.get());
    CHECK(rig.sent() == (std::vector<std::string>{"delete ours:1"}));

    // Someone else's tool: nothing of ours, no weld, no events.
    rig.serverMoves(*rig.theirs, rig.other.get());
    CHECK(rig.mine(*rig.theirs)->parent() == rig.mine(*rig.other));
    CHECK(rig.client->tools().state(*rig.mine<Tool>(*rig.theirs)) == ToolState::None);
    rig.mine<Tool>(*rig.theirs)->fireEvent("Activated");
    CHECK(rig.sent().empty());

    // A tool lying in the Workspace: EquipTool asks the server for it.
    auto loose = makeTool("Loose", rig.game->workspace());
    CHECK(rig.replica->handle(rig.server->addInstance(loose)));
    auto* humanoid = rig.mine<Humanoid>(*rig.character->findFirstChildOfClass("Humanoid"));
    rig.client->tools().equip(*humanoid, *rig.mine<Tool>(*loose));
    CHECK(rig.sent() == (std::vector<std::string>{"Workspace.Me.Humanoid.ServerEquipTool"}));
}

TEST(net_tools_character_gone_sends_nothing)
{
    ToolRig rig;
    CHECK(rig.client->selectTool(0));
    CHECK_EQ(rig.sent().size(), std::size_t{2});
    // The server removes our character (a respawn): the weld went with it.
    const std::array<Item, 1> items{DeleteItem{rig.server->idOf(rig.character.get())}};
    CHECK(rig.replica->handle(encodeData(items, rig.server->schema(), rig.server->context())));
    CHECK(rig.sent().empty());
}

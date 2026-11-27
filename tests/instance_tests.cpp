#include "framework.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>

#include <format>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace lunar;

namespace {

template <class T>
std::shared_ptr<T> make(std::string name, Instance* parent = nullptr)
{
    auto instance = std::make_shared<T>();
    instance->setName(std::move(name));
    instance->setParent(parent);
    return instance;
}

std::vector<std::string> names(const std::vector<InstancePtr>& instances)
{
    std::vector<std::string> result;
    result.reserve(instances.size());
    for (const auto& i : instances)
        result.push_back(i->name());
    return result;
}

} // namespace

// --- Classes -----------------------------------------------------------------

TEST(class_names_come_from_reflection)
{
    CHECK_EQ(Part().className(), "Part");
    CHECK_EQ(Folder().className(), "Folder");
    CHECK_EQ(Workspace::staticClassInfo().name, "Workspace");
    CHECK_EQ(DataModel::staticClassInfo().name, "DataModel");
}

TEST(new_instance_is_named_after_its_class)
{
    auto part = std::make_shared<Part>();
    CHECK_EQ(part->name(), "Part");
    CHECK(part->parent() == nullptr);
    CHECK(part->children().empty());
}

TEST(is_a_walks_the_class_chain)
{
    auto part = std::make_shared<Part>();
    CHECK(part->isA("Part"));
    CHECK(part->isA("BasePart"));
    CHECK(part->isA("PVInstance"));
    CHECK(part->isA("Instance"));
    CHECK(!part->isA("Model"));
    CHECK(!part->isA("Folder"));

    auto game = DataModel::create();
    CHECK(game->workspace()->isA("Model"));
    CHECK(game->isA("ServiceProvider"));
}

// instanceCast answers as dynamic_cast does, for every registered class
// against a few bases and leaves, const or not, and for placeholders.
TEST(instance_cast_agrees_with_dynamic_cast)
{
    std::vector<InstancePtr> instances;
    for (const ClassInfo* info : ClassRegistry::all())
        if (info->factory)
            instances.push_back(info->factory());
    instances.push_back(std::make_shared<UnknownInstance>(ClassRegistry::placeholder("NotARealClass")));
    CHECK(instances.size() > 50);
    auto agree = [&]<class T>() {
        for (const InstancePtr& instance : instances) {
            Instance* plain = instance.get();
            const Instance* constant = plain;
            if (instanceCast<T>(plain) != dynamic_cast<T*>(plain)
                || instanceCast<const T>(constant) != dynamic_cast<const T*>(constant))
                return false;
        }
        return instanceCast<T>(static_cast<Instance*>(nullptr)) == nullptr;
    };
    CHECK(agree.operator()<GuiBase2d>());
    CHECK(agree.operator()<GuiObject>());
    CHECK(agree.operator()<GuiButton>());
    CHECK(agree.operator()<TextLabel>());
    CHECK(agree.operator()<TextBox>());
    CHECK(agree.operator()<LayerCollector>());
    CHECK(agree.operator()<UIGridStyleLayout>());
    CHECK(agree.operator()<BasePart>());
    CHECK(agree.operator()<Part>());
    CHECK(agree.operator()<Model>());
    // From a pointer to a base other than Instance, too.
    auto label = std::make_shared<TextLabel>();
    const GuiObject* object = label.get();
    CHECK_EQ(instanceCast<TextLabel>(object), static_cast<const TextLabel*>(label.get()));
    CHECK(instanceCast<TextButton>(object) == nullptr);
}

TEST(registry_creates_creatable_classes)
{
    InstancePtr part = ClassRegistry::create("Part");
    CHECK_EQ(part->className(), "Part");
    CHECK(dynamic_cast<Part*>(part.get()) != nullptr);
    CHECK_EQ(ClassRegistry::create("Model")->className(), "Model");
    CHECK_EQ(ClassRegistry::create("Folder")->className(), "Folder");
}

TEST(registry_refuses_everything_else)
{
    CHECK_THROWS(ClassRegistry::create("Nope"), Error);
    CHECK_THROWS(ClassRegistry::create("Instance"), Error);
    CHECK_THROWS(ClassRegistry::create("BasePart"), Error);
    CHECK_THROWS(ClassRegistry::create("Workspace"), Error);
    CHECK_THROWS(ClassRegistry::create("DataModel"), Error);
    CHECK(ClassRegistry::find("BasePart") != nullptr);
    CHECK(ClassRegistry::find("Nope") == nullptr);
}

TEST(part_has_roblox_defaults)
{
    Part part;
    CHECK(part.size() == (Vector3{4.0f, 1.0f, 2.0f}));
    CHECK(part.position() == Vector3{});
    CHECK(part.color() == Color3::fromRGB(163, 162, 165));
    CHECK_EQ(part.transparency(), 0.0f);
    CHECK(!part.anchored());
    CHECK(part.canCollide());
}

// --- Hierarchy ---------------------------------------------------------------

TEST(set_parent_links_both_ways)
{
    auto folder = make<Folder>("Folder");
    auto part = make<Part>("Part", folder.get());
    CHECK(part->parent() == folder.get());
    CHECK_EQ(folder->children().size(), 1u);
    CHECK(folder->children()[0] == part);
}

TEST(reparent_moves_child)
{
    auto a = make<Folder>("A");
    auto b = make<Folder>("B");
    auto part = make<Part>("Part", a.get());
    part->setParent(b);
    CHECK(a->children().empty());
    CHECK_EQ(b->children().size(), 1u);
    CHECK(part->parent() == b.get());
    part->setParent(nullptr);
    CHECK(b->children().empty());
    CHECK(part->parent() == nullptr);
}

TEST(children_keep_insertion_order)
{
    auto folder = make<Folder>("Folder");
    make<Part>("One", folder.get());
    make<Part>("Two", folder.get());
    make<Part>("Three", folder.get());
    std::vector<InstancePtr> kids(folder->children().begin(), folder->children().end());
    CHECK_EQ(names(kids), (std::vector<std::string>{"One", "Two", "Three"}));
}

TEST(parent_keeps_children_alive)
{
    auto folder = make<Folder>("Folder");
    std::weak_ptr<Part> weak;
    {
        auto part = make<Part>("Part", folder.get());
        weak = part;
    }
    CHECK(!weak.expired());
    folder->clearAllChildren();
    CHECK(weak.expired());
}

TEST(dropped_parent_orphans_referenced_child)
{
    auto part = std::make_shared<Part>();
    {
        auto folder = make<Folder>("Folder");
        part->setParent(folder);
    }
    CHECK(part->parent() == nullptr);
}

TEST(cannot_parent_to_self)
{
    auto folder = make<Folder>("Folder");
    CHECK_THROWS(folder->setParent(folder), Error);
}

TEST(cannot_create_cycles)
{
    auto a = make<Folder>("A");
    auto b = make<Folder>("B", a.get());
    auto c = make<Folder>("C", b.get());
    CHECK_THROWS(a->setParent(c), Error);
    CHECK(a->parent() == nullptr);
    CHECK(c->parent() == b.get());
}

TEST(descendants_are_preorder)
{
    auto root = make<Folder>("Root");
    auto a = make<Folder>("A", root.get());
    make<Part>("A1", a.get());
    make<Part>("A2", a.get());
    auto b = make<Folder>("B", root.get());
    make<Part>("B1", b.get());
    CHECK_EQ(names(root->descendants()), (std::vector<std::string>{"A", "A1", "A2", "B", "B1"}));
}

TEST(find_first_child)
{
    auto root = make<Folder>("Root");
    auto a = make<Folder>("A", root.get());
    auto deep = make<Part>("Deep", a.get());
    auto target = make<Part>("Target", root.get());

    CHECK(root->findFirstChild("Target") == target.get());
    CHECK(root->findFirstChild("Deep") == nullptr);
    CHECK(root->findFirstChild("Deep", true) == deep.get());
    CHECK(root->findFirstChild("Missing", true) == nullptr);
}

TEST(find_first_child_by_class)
{
    auto root = make<Folder>("Root");
    auto model = make<Model>("M", root.get());
    auto part = make<Part>("P", model.get());

    CHECK(root->findFirstChildOfClass("Model") == model.get());
    CHECK(root->findFirstChildOfClass("PVInstance") == nullptr); // exact class only
    CHECK(root->findFirstChildWhichIsA("PVInstance") == model.get());
    CHECK(root->findFirstChildWhichIsA("BasePart") == nullptr);
    CHECK(root->findFirstChildWhichIsA("BasePart", true) == part.get());
}

TEST(find_first_ancestor)
{
    auto game = DataModel::create();
    auto model = make<Model>("Car", game->workspace());
    auto folder = make<Folder>("Wheels", model.get());
    auto part = make<Part>("Wheel", folder.get());

    CHECK(part->findFirstAncestor("Car") == model.get());
    CHECK(part->findFirstAncestorOfClass("Workspace") == game->workspace());
    CHECK(part->findFirstAncestorWhichIsA("Model") == model.get());
    CHECK(part->findFirstAncestor("Nope") == nullptr);
    CHECK(part->findFirstAncestor("Wheel") == nullptr); // never itself
}

TEST(ancestor_and_descendant_checks)
{
    auto a = make<Folder>("A");
    auto b = make<Folder>("B", a.get());
    auto c = make<Folder>("C", b.get());
    CHECK(c->isDescendantOf(a.get()));
    CHECK(a->isAncestorOf(c.get()));
    CHECK(!a->isDescendantOf(c.get()));
    CHECK(!c->isAncestorOf(a.get()));
    CHECK(!a->isDescendantOf(a.get()));
    CHECK(!a->isAncestorOf(nullptr));
}

TEST(full_name_skips_data_model)
{
    auto game = DataModel::create();
    auto model = make<Model>("Tower", game->workspace());
    auto part = make<Part>("Top", model.get());
    CHECK_EQ(part->fullName(), "Workspace.Tower.Top");
    CHECK_EQ(game->workspace()->fullName(), "Workspace");

    auto loose = make<Folder>("Loose");
    auto child = make<Part>("Child", loose.get());
    CHECK_EQ(child->fullName(), "Loose.Child");
}

// --- Destroy -----------------------------------------------------------------

TEST(destroy_unparents_and_locks)
{
    auto folder = make<Folder>("Folder");
    auto part = make<Part>("Part", folder.get());
    part->destroy();
    CHECK(part->destroyed());
    CHECK(part->parent() == nullptr);
    CHECK(folder->children().empty());
    CHECK_THROWS(part->setParent(folder), Error);
    part->destroy(); // second call is a no-op
}

TEST(destroy_takes_descendants_with_it)
{
    auto root = make<Folder>("Root");
    auto mid = make<Folder>("Mid", root.get());
    auto leaf = make<Part>("Leaf", mid.get());
    root->destroy();
    CHECK(mid->destroyed());
    CHECK(leaf->destroyed());
    CHECK(mid->parent() == nullptr);
    CHECK(leaf->parent() == nullptr);
    CHECK_THROWS(leaf->setParent(root), Error);
}

TEST(destroy_fires_destroying_then_disconnects)
{
    auto folder = make<Folder>("Folder");
    int destroyingCalls = 0;
    folder->destroying.connect([&] { ++destroyingCalls; });
    Connection added = folder->childAdded.connect([](Instance*) {});
    folder->destroy();
    CHECK_EQ(destroyingCalls, 1);
    CHECK(!added.connected());
}

TEST(clear_all_children)
{
    auto folder = make<Folder>("Folder");
    auto a = make<Part>("A", folder.get());
    auto b = make<Part>("B", folder.get());
    folder->clearAllChildren();
    CHECK(folder->children().empty());
    CHECK(a->destroyed());
    CHECK(b->destroyed());
    CHECK(!folder->destroyed());
}

// --- Clone -------------------------------------------------------------------

TEST(clone_copies_properties_and_children)
{
    auto model = make<Model>("Tower");
    auto part = make<Part>("Floor", model.get());
    part->setSize({8.0f, 2.0f, 8.0f});
    part->setAnchored(true);
    make<Folder>("Extras", model.get());

    auto parent = make<Folder>("Holder");
    model->setParent(parent);

    InstancePtr copy = model->clone();
    CHECK(copy != nullptr);
    CHECK(copy != model);
    CHECK_EQ(copy->className(), "Model");
    CHECK_EQ(copy->name(), "Tower");
    CHECK(copy->parent() == nullptr);
    CHECK_EQ(names(copy->descendants()), (std::vector<std::string>{"Floor", "Extras"}));

    auto* floor = dynamic_cast<Part*>(copy->findFirstChild("Floor"));
    CHECK(floor != nullptr);
    CHECK(floor != part.get());
    CHECK(floor->parent() == copy.get());
    CHECK(floor->size() == (Vector3{8.0f, 2.0f, 8.0f}));
    CHECK(floor->anchored());
}

TEST(clone_skips_non_archivable)
{
    auto folder = make<Folder>("Folder");
    auto keep = make<Part>("Keep", folder.get());
    auto skip = make<Part>("Skip", folder.get());
    skip->setArchivable(false);

    InstancePtr copy = folder->clone();
    CHECK(copy->findFirstChild("Keep") != nullptr);
    CHECK(copy->findFirstChild("Skip") == nullptr);
    CHECK(skip->clone() == nullptr);
}

TEST(clone_does_not_copy_connections)
{
    auto part = make<Part>("Part");
    int calls = 0;
    part->changed.connect([&](std::string_view) { ++calls; });
    InstancePtr copy = part->clone();
    copy->setName("Other");
    CHECK_EQ(calls, 0);
}

TEST(services_and_data_model_cannot_be_cloned)
{
    auto game = DataModel::create();
    CHECK(game->clone() == nullptr);
    CHECK(game->workspace()->clone() == nullptr);
}

// --- Services ----------------------------------------------------------------

TEST(get_service_creates_once)
{
    auto game = DataModel::create();
    Instance* ws = game->getService("Workspace");
    CHECK(ws != nullptr);
    CHECK(ws == game->workspace());
    CHECK(ws == game->findService("Workspace"));
    CHECK(ws->parent() == game.get());
    CHECK_EQ(game->children().size(), 1u);
}

TEST(get_service_rejects_non_services)
{
    auto game = DataModel::create();
    CHECK_THROWS(game->getService("Part"), Error);
    CHECK_THROWS(game->getService("Nope"), Error);
    CHECK(game->findService("Nope") == nullptr);
}

TEST(services_are_parent_locked)
{
    auto game = DataModel::create();
    Workspace* ws = game->workspace();
    CHECK_THROWS(ws->setParent(nullptr), Error);
    CHECK_THROWS(ws->destroy(), Error);
    CHECK(ws->parent() == game.get());
}

// --- Events ------------------------------------------------------------------

TEST(parenting_fires_events_in_order)
{
    auto game = DataModel::create();
    Workspace* ws = game->workspace();
    auto folder = make<Folder>("Folder", ws);
    auto part = make<Part>("Part");

    std::vector<std::string> log;
    folder->childAdded.connect([&](Instance* c) { log.push_back("Folder.ChildAdded " + c->name()); });
    folder->childRemoved.connect([&](Instance* c) { log.push_back("Folder.ChildRemoved " + c->name()); });
    ws->descendantAdded.connect([&](Instance* d) { log.push_back("Workspace.DescendantAdded " + d->name()); });
    ws->descendantRemoving.connect([&](Instance* d) { log.push_back("Workspace.DescendantRemoving " + d->name()); });
    part->ancestryChanged.connect([&](Instance* c, Instance* p) {
        log.push_back(std::format("Part.AncestryChanged {} {}", c->name(), p ? p->name() : "nil"));
    });
    part->changed.connect([&](std::string_view property) { log.push_back(std::format("Part.Changed {}", property)); });

    part->setParent(folder);
    CHECK_EQ(log, (std::vector<std::string>{
                      "Folder.ChildAdded Part",
                      "Workspace.DescendantAdded Part",
                      "Part.AncestryChanged Part Folder",
                      "Part.Changed Parent",
                  }));

    log.clear();
    part->setParent(nullptr);
    CHECK_EQ(log, (std::vector<std::string>{
                      "Workspace.DescendantRemoving Part",
                      "Folder.ChildRemoved Part",
                      "Part.AncestryChanged Part nil",
                      "Part.Changed Parent",
                  }));
}

TEST(moving_a_subtree_notifies_every_descendant)
{
    auto game = DataModel::create();
    auto model = make<Model>("Model");
    auto part = make<Part>("Part", model.get());

    std::vector<std::string> added;
    game->workspace()->descendantAdded.connect([&](Instance* d) { added.push_back(d->name()); });
    Instance* movedChild = nullptr;
    Instance* movedTo = nullptr;
    part->ancestryChanged.connect([&](Instance* c, Instance* p) {
        movedChild = c;
        movedTo = p;
    });

    model->setParent(game->workspace());
    CHECK_EQ(added, (std::vector<std::string>{"Model", "Part"}));
    CHECK(movedChild == model.get());
    CHECK(movedTo == game->workspace());
}

TEST(changed_fires_only_on_real_changes)
{
    auto part = make<Part>("Part");
    std::vector<std::string> log;
    part->changed.connect([&](std::string_view property) { log.emplace_back(property); });

    part->setName("Part"); // same value
    part->setAnchored(false);
    part->setName("Brick");
    part->setAnchored(true);
    part->setTransparency(0.5f);
    CHECK_EQ(log, (std::vector<std::string>{"Name", "Anchored", "Transparency"}));
}

TEST(destroy_fires_removal_events)
{
    auto folder = make<Folder>("Folder");
    auto part = make<Part>("Part", folder.get());
    std::vector<std::string> log;
    folder->childRemoved.connect([&](Instance* c) { log.push_back("ChildRemoved " + c->name()); });
    folder->descendantRemoving.connect([&](Instance* d) { log.push_back("DescendantRemoving " + d->name()); });
    part->destroy();
    CHECK_EQ(log, (std::vector<std::string>{"DescendantRemoving Part", "ChildRemoved Part"}));
}

// A move nobody listens to takes a shortcut (no list of what moves, no
// events): the tree comes out the same, Changed still fires, and one
// listener anywhere the move reaches (deep in the subtree, up the new
// ancestors) hears it as before.
TEST(instance_reparent_without_listeners_matches)
{
    auto build = [] {
        auto root = ClassRegistry::create("Folder");
        auto a = ClassRegistry::create("Folder");
        auto b = ClassRegistry::create("Folder");
        auto leaf = ClassRegistry::create("Part");
        leaf->setParent(b);
        b->setParent(a);
        a->setParent(root);
        return std::tuple{root, a, b, leaf};
    };
    auto [root, a, b, leaf] = build();
    auto other = ClassRegistry::create("Folder");
    other->setParent(root);
    int parentChanges = 0;
    auto changed = b->changed.connect([&](std::string_view name) { parentChanges += name == "Parent" ? 1 : 0; });
    b->setParent(other);
    CHECK_EQ(parentChanges, 1);
    CHECK(b->parent() == other.get());
    CHECK(a->children().empty());
    CHECK_EQ(other->children().size(), std::size_t{1});
    CHECK(leaf->isDescendantOf(other.get()));

    // Someone listening deep down: AncestryChanged reaches it.
    int ancestry = 0;
    Instance* heardParent = nullptr;
    auto deep = leaf->ancestryChanged.connect([&](Instance* child, Instance* parent) {
        ancestry += child == b.get() ? 1 : 100;
        heardParent = parent;
    });
    b->setParent(a);
    CHECK_EQ(ancestry, 1);
    CHECK(heardParent == a.get());
    // Up the new parent's ancestors: DescendantAdded for each moving instance.
    int added = 0;
    auto up = root->descendantAdded.connect([&](Instance*) { ++added; });
    b->setParent(other);
    CHECK_EQ(added, 2); // b and leaf
    CHECK_EQ(ancestry, 2);
    CHECK(heardParent == other.get());
}

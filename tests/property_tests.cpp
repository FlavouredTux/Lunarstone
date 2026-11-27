#include "framework.hpp"

#include <lunar/Classes.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace lunar;

namespace {

std::vector<std::string> propertyNames(const Instance& instance)
{
    std::vector<std::string> names;
    for (const Property* property : instance.properties())
        names.emplace_back(property->name);
    return names;
}

template <class F>
std::string errorOf(F&& f)
{
    try {
        std::forward<F>(f)();
    } catch (const Error& e) {
        return e.what();
    }
    return "<no error>";
}

} // namespace

TEST(properties_are_listed_base_first)
{
    Part part;
    // clang-format off
    CHECK_EQ(propertyNames(part), (std::vector<std::string>{
        "ClassName", "Name", "Archivable", "Parent",
        "Size", "CFrame", "Position", "Orientation", "Color", "BrickColor", "Transparency", "Anchored", "CanCollide",
        "Material", "MaterialVariant", "Reflectance", "CanTouch", "CanQuery", "CastShadow", "Locked", "Massless",
        "EnableFluidForces", "AudioCanCollide", "RootPriority", "CollisionGroup", "CustomPhysicalProperties",
        "AssemblyLinearVelocity", "AssemblyAngularVelocity", "Velocity", "RotVelocity", "PivotOffset",
        "LocalTransparencyModifier",
        "TopSurface", "BottomSurface", "FrontSurface", "BackSurface", "LeftSurface", "RightSurface",
        "FormFactor", "Shape"}));
    // clang-format on
    Folder folder;
    CHECK_EQ(propertyNames(folder), (std::vector<std::string>{"ClassName", "Name", "Archivable", "Parent"}));
}

TEST(property_types_and_read_only)
{
    Part part;
    const Property* className = part.classInfo().findProperty("ClassName");
    const Property* size = part.classInfo().findProperty("Size");
    const Property* parent = part.classInfo().findProperty("Parent");
    CHECK(className && size && parent);
    CHECK(className->readOnly());
    CHECK(!size->readOnly());
    CHECK(!parent->readOnly());
    CHECK_EQ(size->type, "Vector3");
    CHECK_EQ(parent->type, "Instance");
    CHECK_EQ(part.classInfo().findProperty("Transparency")->type, "number");
    CHECK(part.classInfo().findProperty("Nope") == nullptr);
}

TEST(get_returns_typed_values)
{
    auto part = std::make_shared<Part>();
    CHECK(part->get("Name") == Value("Part"));
    CHECK(part->get("ClassName") == Value("Part"));
    CHECK(part->get("Size") == Value(Vector3{4.0f, 1.0f, 2.0f}));
    CHECK(part->get("Anchored") == Value(false));
    CHECK(part->get("Transparency") == Value(0.0));
    CHECK(part->get("Parent") == Value());
}

TEST(set_goes_through_the_setter)
{
    auto part = std::make_shared<Part>();
    std::vector<std::string> log;
    part->changed.connect([&](std::string_view property) { log.emplace_back(property); });

    part->set("Name", "Brick");
    part->set("Size", Vector3{2.0f, 2.0f, 2.0f});
    part->set("Transparency", 0.25);
    part->set("Anchored", true);
    part->set("Anchored", true); // no change, no event

    CHECK_EQ(part->name(), "Brick");
    CHECK(part->size() == (Vector3{2.0f, 2.0f, 2.0f}));
    CHECK_EQ(part->transparency(), 0.25f);
    CHECK(part->anchored());
    CHECK_EQ(log, (std::vector<std::string>{"Name", "Size", "Transparency", "Anchored"}));
}

TEST(set_parent_by_property)
{
    auto folder = std::make_shared<Folder>();
    auto part = std::make_shared<Part>();
    part->set("Parent", InstancePtr(folder));
    CHECK(part->parent() == folder.get());
    CHECK(part->get("Parent") == Value(InstancePtr(folder)));
    part->set("Parent", Value());
    CHECK(part->parent() == nullptr);
}

TEST(set_rejects_wrong_types)
{
    auto part = std::make_shared<Part>();
    CHECK_EQ(errorOf([&] { part->set("Size", "big"); }),
             "Unable to assign property Size. Vector3 expected, got string");
    CHECK_EQ(errorOf([&] { part->set("Anchored", 1.0); }),
             "Unable to assign property Anchored. boolean expected, got number");
    CHECK_EQ(errorOf([&] { part->set("Parent", 5.0); }),
             "Unable to assign property Parent. Instance expected, got number");
    CHECK(part->size() == (Vector3{4.0f, 1.0f, 2.0f}));
}

TEST(set_rejects_read_only_and_unknown)
{
    auto game = DataModel::create();
    auto part = std::make_shared<Part>();
    part->setParent(game->workspace());
    CHECK_EQ(errorOf([&] { part->set("ClassName", "Model"); }),
             "Unable to assign property ClassName. Property is read only");
    CHECK_EQ(errorOf([&] { part->set("Wobble", 1.0); }), "Wobble is not a valid member of Part \"Workspace.Part\"");
    CHECK_EQ(errorOf([&] { (void)part->get("Wobble"); }), "Wobble is not a valid member of Part \"Workspace.Part\"");
}

TEST(position_moves_cframe_and_keeps_rotation)
{
    auto part = std::make_shared<Part>();
    CFrame rotated = CFrame(1.0f, 2.0f, 3.0f) * CFrame::angles(0.0f, 1.0f, 0.0f);
    part->setCFrame(rotated);

    std::vector<std::string> log;
    part->changed.connect([&](std::string_view property) { log.emplace_back(property); });
    part->set("Position", Vector3{10.0f, 0.0f, 0.0f});

    CHECK(part->position() == (Vector3{10.0f, 0.0f, 0.0f}));
    CHECK(part->cframe().lookVector() == rotated.lookVector());
    CHECK_EQ(log, (std::vector<std::string>{"CFrame", "Position"}));
}

TEST(value_type_names_and_strings)
{
    CHECK_EQ(typeName(Value()), "nil");
    CHECK_EQ(typeName(Value(true)), "boolean");
    CHECK_EQ(typeName(Value(1.5)), "number");
    CHECK_EQ(typeName(Value("x")), "string");
    CHECK_EQ(typeName(Value(CFrame())), "CFrame");
    CHECK_EQ(typeName(Value(std::make_shared<Part>())), "Instance");

    CHECK_EQ(toString(Value()), "nil");
    CHECK_EQ(toString(Value(true)), "true");
    CHECK_EQ(toString(Value(0.5)), "0.5");
    CHECK_EQ(toString(Value(3.0)), "3");
    CHECK_EQ(toString(Value(Vector3{4.0f, 1.0f, 2.0f})), "4, 1, 2");
    CHECK_EQ(toString(Value(CFrame(1.0f, 2.0f, 3.0f))), "1, 2, 3, 1, 0, 0, 0, 1, 0, 0, 0, 1");
    CHECK_EQ(toString(Value(std::make_shared<Part>())), "Part");
}

TEST(clone_copies_cframe)
{
    auto part = std::make_shared<Part>();
    part->setCFrame(CFrame(5.0f, 6.0f, 7.0f) * CFrame::angles(0.5f, 0.0f, 0.0f));
    auto copy = std::dynamic_pointer_cast<Part>(part->clone());
    CHECK(copy != nullptr);
    CHECK(copy->cframe() == part->cframe());
}

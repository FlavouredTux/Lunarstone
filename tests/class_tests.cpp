#include "framework.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/PlaceFile.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace lunar;
using namespace std::string_literals;

namespace {

bool near(float a, float b)
{
    return std::abs(a - b) < 1e-4f;
}

bool near(Vector3 a, Vector3 b)
{
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

// Unit columns at right angles, to within a hundredth of a percent.
bool isRotation(const CFrame& cf)
{
    const Vector3 x = cf.rightVector(), y = cf.upVector(), z{cf.r[0][2], cf.r[1][2], cf.r[2][2]};
    for (float e : {x.dot(x) - 1.0f, y.dot(y) - 1.0f, z.dot(z) - 1.0f, x.dot(y), x.dot(z), y.dot(z)})
        if (!(std::abs(e) < 1e-4f))
            return false;
    return true;
}

bool near(const CFrame& a, const CFrame& b)
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (!near(a.r[i][j], b.r[i][j]))
                return false;
    return near(a.position, b.position);
}

template <class T>
std::shared_ptr<T> make(Instance* parent = nullptr, std::string_view name = {})
{
    auto instance = std::make_shared<T>();
    if (!name.empty())
        instance->setName(std::string(name));
    if (parent)
        instance->setParent(parent);
    return instance;
}

} // namespace

TEST(registry_finds_declared_classes)
{
    for (std::string_view name : {"Part", "MeshPart", "WedgePart", "UnionOperation", "Attachment", "Motor6D",
                                  "Humanoid", "StringValue", "ObjectValue", "Decal", "PointLight", "Sound",
                                  "ParticleEmitter", "Script", "LocalScript", "ModuleScript", "Tool", "Accessory"}) {
        const ClassInfo* info = ClassRegistry::find(name);
        CHECK(info != nullptr);
        CHECK(info->kind == ClassKind::Creatable);
        CHECK_EQ(ClassRegistry::create(name)->className(), name);
    }
    for (std::string_view name : {"Lighting", "Players", "ReplicatedStorage", "ServerScriptService", "StarterGui",
                                  "StarterPack", "StarterPlayer", "SoundService", "Teams", "Chat"})
        CHECK(ClassRegistry::find(name)->kind == ClassKind::Service);
    CHECK(ClassRegistry::find("BasePart")->kind == ClassKind::Abstract);
    CHECK(ClassRegistry::find("Terrain")->kind == ClassKind::NotCreatable);
    CHECK(ClassRegistry::find("UnknownInstance") == nullptr);
    CHECK(ClassRegistry::find("Ref") == nullptr);

    auto all = ClassRegistry::all();
    CHECK(all.size() > 150u);
    CHECK(std::ranges::is_sorted(all, {}, &ClassInfo::name));
    CHECK(ClassRegistry::find("LocalScript")->isA("Script"));
    CHECK(ClassRegistry::find("Seat")->isA("Part"));
    CHECK(ClassRegistry::find("Workspace")->isA("Model"));
}

// Checked against the reflection data in RobloxPlayer 0.742.
TEST(class_hierarchy_matches_roblox)
{
    auto baseOf = [](std::string_view name) { return ClassRegistry::find(name)->base->name; };
    CHECK_EQ(baseOf("KeyframeSequence"), "AnimationClip");
    CHECK_EQ(baseOf("RemoteEvent"), "BaseRemoteEvent");
    CHECK_EQ(baseOf("UnreliableRemoteEvent"), "BaseRemoteEvent");
    CHECK_EQ(baseOf("PlayerGui"), "BasePlayerGui");
    CHECK_EQ(baseOf("StarterGui"), "BasePlayerGui");
    CHECK_EQ(baseOf("UIComponent"), "UIBase");
    CHECK_EQ(baseOf("StarterCharacterScripts"), "StarterPlayerScripts");
    CHECK_EQ(baseOf("CSGDictionaryService"), "FlyweightService");
    CHECK(ClassRegistry::find("StarterGui")->kind == ClassKind::Service);

    // Loop and Priority moved up to AnimationClip but still work on KeyframeSequence.
    auto sequence = ClassRegistry::create("KeyframeSequence");
    sequence->set("Loop", false);
    CHECK(sequence->get("Loop") == Value(false));
    CHECK(sequence->classInfo().findProperty("Priority") != nullptr);
}

TEST(starter_player_settings_reach_the_humanoid)
{
    StarterPlayer starter;
    starter.setCharacterWalkSpeed(18.0f);
    starter.setCharacterJumpPower(31.0f);
    starter.setCharacterUseJumpPower(true);
    Humanoid humanoid;
    starter.applyCharacterSettings(humanoid);
    CHECK_EQ(humanoid.walkSpeed(), 18.0f);
    CHECK_EQ(humanoid.jumpPower(), 31.0f);
    CHECK(humanoid.useJumpPower());
}

TEST(enum_values_match_roblox)
{
    CHECK_EQ(enums::item(Enum::CollisionFidelity::Tunable).value, 4u);
    CHECK_EQ(enums::item(Enum::ModelLevelOfDetail::SLIM).value, 4u);
    CHECK_EQ(enums::item(Enum::AlphaMode::Opaque).value, 3u);
    CHECK_EQ(enums::item(Enum::Material::Plastic).value, 256u);
    CHECK_EQ(enums::item(Enum::NormalId::Front).value, 5u);
}

TEST(enum_properties_take_items_names_and_numbers)
{
    Part part;
    CHECK(part.get("Material") == Value(enums::item(Enum::Material::Plastic)));
    part.set("Material", "Neon"s);
    CHECK(part.material() == Enum::Material::Neon);
    part.set("Material", 512.0);
    CHECK(part.material() == Enum::Material::Wood);
    part.set("Material", enums::item(Enum::Material::Glass));
    CHECK(part.material() == Enum::Material::Glass);
    CHECK_THROWS(part.set("Material", "Lava"s), Error);
    CHECK_THROWS(part.set("Material", 3.0), Error);
    CHECK_THROWS(part.set("Material", enums::item(Enum::PartType::Ball)), Error);
    CHECK_THROWS(part.set("Material", true), Error);
    CHECK_EQ(part.classInfo().findProperty("Material")->type, "Enum.Material");
    CHECK_EQ(part.classInfo().findProperty("Shape")->type, "Enum.PartType");
}

TEST(optional_properties_take_nil)
{
    Part part;
    const Property* physics = part.classInfo().findProperty("CustomPhysicalProperties");
    CHECK_EQ(physics->type, "PhysicalProperties?");
    CHECK(std::holds_alternative<std::monostate>(part.get("CustomPhysicalProperties")));
    part.set("CustomPhysicalProperties", PhysicalProperties{2, 0.5f, 0.1f, 1, 1});
    CHECK(part.customPhysicalProperties() == PhysicalProperties{2, 0.5f, 0.1f, 1, 1});
    part.set("CustomPhysicalProperties", Value());
    CHECK(!part.customPhysicalProperties());

    Model model;
    CHECK_EQ(model.classInfo().findProperty("WorldPivotData")->type, "CFrame?");
}

TEST(brick_color_follows_color)
{
    Part part;
    std::vector<std::string> changes;
    part.changed.connect([&](std::string_view property) { changes.emplace_back(property); });
    CHECK_EQ(part.brickColor().number, 194);
    part.setBrickColor(BrickColor::fromNumber(21));
    CHECK(part.color() == Color3::fromRGB(196, 40, 28));
    part.setColor(Color3::fromRGB(250, 2, 1));
    CHECK_EQ(part.brickColor().name(), "Really red");
    CHECK(part.color() == Color3::fromRGB(250, 2, 1)); // Color keeps its exact value
    part.set("BrickColor", 1.0);                       // files store palette numbers
    CHECK_EQ(part.brickColor().name(), "White");
    CHECK_EQ(changes, (std::vector<std::string>{"Color", "BrickColor", "Color", "BrickColor", "Color", "BrickColor"}));
}

TEST(fields_fire_changed)
{
    PointLight light;
    std::vector<std::string> changes;
    light.changed.connect([&](std::string_view property) { changes.emplace_back(property); });
    light.setRange(12);
    light.setRange(12);
    light.set("Brightness", 4.0);
    CHECK_EQ(light.range(), 12.0f);
    CHECK_EQ(light.brightness(), 4.0f);
    CHECK_EQ(changes, (std::vector<std::string>{"Range", "Brightness"}));

    Lighting lighting;
    lighting.set("ColorShift_Top", Color3{1, 0, 0}); // setter named after the property
    CHECK(lighting.colorShiftTop() == (Color3{1, 0, 0}));
}

TEST(orientation_is_yxz_degrees)
{
    Part part;
    part.setPosition({1, 2, 3});
    part.setOrientation({30, 45, 60});
    CHECK(near(part.orientation(), {30, 45, 60}));
    CHECK(near(part.position(), {1, 2, 3}));
    constexpr float rad = std::numbers::pi_v<float> / 180.0f;
    CFrame expected = CFrame(1, 2, 3) * CFrame::angles(0, 45 * rad, 0) * CFrame::angles(30 * rad, 0, 0)
                      * CFrame::angles(0, 0, 60 * rad);
    CHECK(near(part.cframe(), expected));
}

TEST(clone_remaps_references_inside_the_copy)
{
    auto outside = make<Part>(nullptr, "Outside");
    auto model = make<Model>(nullptr, "Car");
    auto body = make<Part>(model.get(), "Body");
    auto wheel = make<Part>(model.get(), "Wheel");
    auto weld = make<Weld>(body.get());
    weld->setPart0(body.get());
    weld->setPart1(wheel.get());
    auto link = make<ObjectValue>(model.get(), "Link");
    link->setValue(outside.get());
    model->setPrimaryPart(body.get());

    auto copy = std::dynamic_pointer_cast<Model>(model->clone());
    CHECK(copy != nullptr);
    auto* copyBody = dynamic_cast<Part*>(copy->findFirstChild("Body"));
    auto* copyWheel = dynamic_cast<Part*>(copy->findFirstChild("Wheel"));
    auto* copyWeld = dynamic_cast<Weld*>(copyBody->findFirstChildOfClass("Weld"));
    CHECK(copyBody != body.get());
    CHECK(copy->primaryPart() == copyBody);
    CHECK(copyWeld->part0() == copyBody);
    CHECK(copyWeld->part1() == copyWheel);
    // References leaving the copied subtree keep pointing at the original.
    CHECK(dynamic_cast<ObjectValue*>(copy->findFirstChild("Link"))->value() == outside.get());
    // The original is untouched.
    CHECK(model->primaryPart() == body.get());
    CHECK(weld->part1() == wheel.get());
}

TEST(refs_drop_destroyed_targets)
{
    auto value = std::make_shared<ObjectValue>();
    {
        auto part = std::make_shared<Part>();
        value->setValue(part.get());
        CHECK(value->value() == part.get());
    }
    CHECK(value->value() == nullptr);
    CHECK(std::holds_alternative<std::monostate>(value->get("Value")));
}

TEST(model_pivots)
{
    auto model = make<Model>();
    auto a = make<Part>(model.get());
    auto b = make<Part>(model.get());
    a->setSize({2, 2, 2});
    b->setSize({2, 2, 2});
    a->setPosition({0, 0, 0});
    b->setPosition({10, 0, 0});

    // No PrimaryPart and no WorldPivotData: the bounding box center.
    CHECK(near(model->getPivot().position, {5, 0, 0}));
    auto [box, size] = model->getBoundingBox();
    CHECK(near(box.position, {5, 0, 0}));
    CHECK(near(size, {12, 2, 2}));

    model->pivotTo(CFrame(5, 10, 0));
    CHECK(near(a->position(), {0, 10, 0}));
    CHECK(near(b->position(), {10, 10, 0}));
    CHECK(model->worldPivotData().has_value());
    CHECK(near(model->worldPivot().position, {5, 10, 0}));

    // A PrimaryPart's pivot wins, PivotOffset included.
    b->setPivotOffset(CFrame(0, 1, 0));
    model->setPrimaryPart(b.get());
    CHECK(near(model->getPivot().position, {10, 11, 0}));
    model->pivotTo(CFrame(0, 1, 0) * CFrame::angles(0, std::numbers::pi_v<float> / 2, 0));
    CHECK(near(b->position(), {0, 0, 0}));
    CHECK(near(b->getPivot().position, {0, 1, 0}));
    CHECK(near(a->position(), {0, 0, 10}));
}

// The game pivots other players' characters every frame (and its effect models,
// and its training dummy). PivotTo moves the parts by the target times the
// pivot's inverse(), the transpose, so a pivot that rounding had left a
// little off a rotation came back off by twice as much: in under 40 frames
// the character was billions of studs across, then NaN, and a triangle of
// it would sweep across the screen.
TEST(model_pivoted_every_frame_stays_a_rotation)
{
    auto model = make<Model>();
    auto root = make<Part>(model.get());
    auto arm = make<Part>(model.get());
    model->setPrimaryPart(root.get());
    root->setCFrame(CFrame(0, 3, 0) * CFrame::angles(0.3f, 0.7f, 1.1f));
    arm->setCFrame(root->cframe() * CFrame(1.5f, 0.5f, 0));
    const CFrame step = CFrame(0.05f, 0, 0.1f) * CFrame::angles(0.013f, 0.37f, -0.021f);
    for (int frame = 0; frame < 300; ++frame)
        model->pivotTo(model->getPivot() * step);
    CHECK(isRotation(root->cframe()));
    CHECK(isRotation(arm->cframe()));
    CHECK(near((arm->position() - root->position()).magnitude(), std::sqrt(2.5f)));
    CHECK(root->position().magnitude() < 1e3f);
}

// The client keeps a part's CFrame a rotation whatever it's set to; a
// rotation already is one and is kept to the bit.
TEST(part_cframe_is_kept_a_rotation)
{
    auto part = make<Part>();
    const CFrame turned = CFrame(1, 2, 3) * CFrame::angles(0.2f, 0.4f, 0.6f);
    CFrame scaled = turned;
    for (auto& row : scaled.r)
        for (float& v : row)
            v *= 2.0f;
    scaled.r[0][1] += 0.3f; // and skewed
    part->setCFrame(scaled);
    CHECK(isRotation(part->cframe()));
    CHECK(near(part->position(), {1, 2, 3}));
    CHECK(near(part->cframe().rightVector(), turned.rightVector()));

    part->setCFrame(turned);
    CHECK(part->cframe() == turned);

    // No rotation is near one with a zero column: it becomes the identity,
    // at the same position, as Studio does.
    CFrame flat = CFrame(4, 5, 6);
    flat.r[0][0] = flat.r[1][1] = flat.r[2][2] = 0.0f;
    part->setCFrame(flat);
    CHECK(part->cframe() == CFrame(4, 5, 6));
}

TEST(attachment_world_cframe)
{
    auto part = make<Part>();
    part->setCFrame(CFrame(0, 5, 0) * CFrame::angles(0, std::numbers::pi_v<float> / 2, 0));
    auto attachment = make<Attachment>(part.get());
    attachment->setPosition({1, 0, 0});
    CHECK(near(attachment->worldPosition(), {0, 5, -1}));

    attachment->setWorldPosition({0, 7, 0});
    CHECK(near(attachment->position(), {0, 2, 0}));

    auto bone = make<Bone>(attachment.get());
    bone->setCFrame(CFrame(0, 1, 0));
    CHECK(near(bone->worldPosition(), {0, 8, 0}));

    Attachment loose;
    loose.setCFrame(CFrame(1, 2, 3));
    CHECK(near(loose.worldCFrame(), CFrame(1, 2, 3)));
}

TEST(attachment_axes_and_orientation)
{
    // The battle royale template's skydive attachments: Orientation, then
    // Axis and SecondaryAxis, all reshaping CFrame's rotation.
    auto part = make<Part>();
    part->setCFrame(CFrame(0, 5, 0) * CFrame::angles(0, std::numbers::pi_v<float> / 2, 0));
    auto pitch = make<Attachment>(part.get());
    pitch->setPosition({0, -0.35f, 0});
    pitch->setOrientation({0, -90, 0});
    CHECK(near(pitch->orientation(), {0, -90, 0}));
    CHECK(near(pitch->axis(), {0, 0, 1}));
    pitch->setAxis({0, 0, 1});
    pitch->setSecondaryAxis({0, 1, 0});
    CHECK(near(pitch->axis(), {0, 0, 1}));
    CHECK(near(pitch->secondaryAxis(), {0, 1, 0}));
    CHECK(near(pitch->cframe().lookVector(), {1, 0, 0}));
    CHECK(near(pitch->position(), {0, -0.35f, 0}));

    // Axis comes first; SecondaryAxis is made perpendicular to it.
    auto skew = make<Attachment>(part.get());
    skew->setSecondaryAxis({1, 1, 0});
    CHECK(near(skew->axis(), {1, 0, 0}));
    CHECK(near(skew->secondaryAxis(), {0, 1, 0}));
    skew->setAxis({0, 0, -1});
    CHECK(near(skew->axis(), {0, 0, -1}));
    CHECK(near(skew->secondaryAxis(), {0, 1, 0}));
    // Parallel axes leave it alone.
    skew->setSecondaryAxis({0, 0, -2});
    CHECK(near(skew->secondaryAxis(), {0, 1, 0}));

    // The world versions go through the part: it's turned 90 degrees about Y.
    auto world = make<Attachment>(part.get());
    CHECK(near(world->worldAxis(), {0, 0, -1}));
    world->setWorldAxis({1, 0, 0});
    CHECK(near(world->worldAxis(), {1, 0, 0}));
    CHECK(near(world->axis(), {0, 0, 1}));
    world->setWorldOrientation({0, 0, 0});
    CHECK(near(world->worldCFrame().rotation(), CFrame()));
}

TEST(lighting_time_of_day)
{
    Lighting lighting;
    CHECK_EQ(lighting.timeOfDay(), "14:00:00");
    lighting.setTimeOfDay("6:30:00");
    CHECK_EQ(lighting.clockTime(), 6.5);
    lighting.setTimeOfDay("18");
    CHECK_EQ(lighting.timeOfDay(), "18:00:00");
    lighting.setClockTime(25.25);
    CHECK_EQ(lighting.timeOfDay(), "01:15:00");
    lighting.setClockTime(-1);
    CHECK_EQ(lighting.clockTime(), 23.0);
    CHECK_THROWS(lighting.setTimeOfDay("noon"), Error);
    CHECK_THROWS(lighting.setTimeOfDay("1:2:3:4"), Error);
}

TEST(scripts_disabled_mirrors_enabled)
{
    Script script;
    CHECK(script.enabled());
    script.setDisabled(true);
    CHECK(!script.enabled());
    CHECK(script.get("Disabled") == Value(true));
    script.set("Enabled", true);
    CHECK(!script.disabled());
}

TEST(humanoid_and_workspace_lookups)
{
    auto character = make<Model>();
    auto root = make<Part>(character.get(), "HumanoidRootPart");
    auto humanoid = make<Humanoid>(character.get());
    CHECK(humanoid->rootPart() == root.get());
    CHECK(humanoid->get("RootPart") == Value(InstancePtr(root)));

    auto game = DataModel::create();
    Workspace* workspace = game->workspace();
    CHECK(workspace->terrain() == nullptr);
    auto terrain = std::make_shared<Terrain>();
    terrain->setParent(workspace);
    CHECK(workspace->terrain() == terrain.get());
    Camera* made = workspace->ensureCurrentCamera();
    CHECK(made != nullptr);
    CHECK(made->parent() == workspace);
    CHECK(workspace->ensureCurrentCamera() == made);
    auto camera = make<Camera>(workspace);
    workspace->setCurrentCamera(camera.get());
    CHECK(workspace->get("CurrentCamera") == Value(InstancePtr(camera)));

    BodyColors colors;
    colors.setHeadColor(BrickColor::fromNumber(1004));
    CHECK(colors.headColor3() == (Color3{1, 0, 0}));
    CHECK(colors.get("HeadColor") == Value(BrickColor{1004}));
}

TEST(loaders_apply_aliases_and_skip_ignored_properties)
{
    auto model = loadModel(R"(<roblox version="4">
        <Item class="Part"><Properties>
            <token name="shape">0</token>
            <token name="formFactorRaw">3</token>
            <string name="MaterialVariantSerialized">Shiny</string>
            <Vector3 name="Velocity"><X>1</X><Y>2</Y><Z>3</Z></Vector3>
            <float name="TopParamA">-0.5</float>
            <int64 name="SourceAssetId">-1</int64>
            <token name="TopSurface">3</token>
            <PhysicalProperties name="CustomPhysicalProperties"><CustomPhysics>false</CustomPhysics></PhysicalProperties>
        </Properties></Item>
        <Item class="Humanoid"><Properties>
            <float name="Health_XML">75</float>
            <token name="RigType">1</token>
        </Properties></Item>
        <Item class="Sound"><Properties>
            <float name="xmlRead_MaxDistance_3">500</float>
            <float name="EmitterSize">5</float>
        </Properties></Item>
    </roblox>)");
    CHECK(model.has_value());
    CHECK(model->warnings.empty());
    auto* part = dynamic_cast<Part*>(model->roots[0].get());
    CHECK(part != nullptr);
    CHECK(part->shape() == Enum::PartType::Ball);
    CHECK(part->formFactor() == Enum::FormFactor::Custom);
    CHECK_EQ(part->materialVariant(), "Shiny");
    CHECK(part->assemblyLinearVelocity() == (Vector3{1, 2, 3}));
    CHECK(part->topSurface() == Enum::SurfaceType::Studs);
    CHECK(!part->customPhysicalProperties());
    auto* humanoid = dynamic_cast<Humanoid*>(model->roots[1].get());
    CHECK_EQ(humanoid->health(), 75.0f);
    CHECK(humanoid->rigType() == Enum::HumanoidRigType::R15);
    auto* sound = dynamic_cast<Sound*>(model->roots[2].get());
    CHECK_EQ(sound->rollOffMaxDistance(), 500.0f);
    CHECK_EQ(sound->rollOffMinDistance(), 5.0f);
}

TEST(xml_loader_reads_newer_value_types)
{
    auto model = loadModel(R"(<roblox version="4">
        <Item class="ParticleEmitter"><Properties>
            <NumberSequence name="Size">0 1 0 1 3 0.5 </NumberSequence>
            <ColorSequence name="Color">0 1 0 0 0 1 0 0 1 0 </ColorSequence>
            <NumberRange name="Lifetime">2 4 </NumberRange>
            <Vector2 name="SpreadAngle"><X>10</X><Y>20</Y></Vector2>
            <token name="Shape">1</token>
        </Properties></Item>
        <Item class="SpawnLocation"><Properties>
            <BrickColor name="TeamColor">21</BrickColor>
            <Color3 name="Color">4278190335</Color3>
        </Properties></Item>
        <Item class="Model"><Properties>
            <OptionalCoordinateFrame name="WorldPivotData"><CFrame>
                <X>1</X><Y>2</Y><Z>3</Z><R00>1</R00><R01>0</R01><R02>0</R02>
                <R10>0</R10><R11>1</R11><R12>0</R12><R20>0</R20><R21>0</R21><R22>1</R22>
            </CFrame></OptionalCoordinateFrame>
            <SharedString name="ModelMeshData">abc</SharedString>
        </Properties></Item>
        <Item class="MeshPart"><Properties>
            <Content name="MeshId"><url>rbxassetid://123</url></Content>
            <Content name="TextureID"><null></null></Content>
            <PhysicalProperties name="CustomPhysicalProperties">
                <CustomPhysics>true</CustomPhysics><Density>3</Density><Friction>0.1</Friction>
                <Elasticity>0.2</Elasticity><FrictionWeight>4</FrictionWeight><ElasticityWeight>5</ElasticityWeight>
            </PhysicalProperties>
        </Properties></Item>
        <SharedStrings><SharedString md5="abc">aGVsbG8=</SharedString></SharedStrings>
    </roblox>)");
    CHECK(model.has_value());
    CHECK_EQ(model->warnings, std::vector<std::string>{});
    auto* emitter = dynamic_cast<ParticleEmitter*>(model->roots[0].get());
    CHECK(emitter->size() == NumberSequence({{0, 1, 0}, {1, 3, 0.5f}}));
    CHECK(emitter->color() == ColorSequence({{0, {1, 0, 0}}, {1, {0, 0, 1}}}));
    CHECK(emitter->lifetime() == (NumberRange{2, 4}));
    CHECK(emitter->spreadAngle() == (Vector2{10, 20}));
    CHECK(emitter->shape() == Enum::ParticleEmitterShape::Sphere);
    auto* spawn = dynamic_cast<SpawnLocation*>(model->roots[1].get());
    CHECK_EQ(spawn->teamColor().name(), "Bright red");
    CHECK(spawn->color() == (Color3{0, 0, 1}));
    auto* pivoted = dynamic_cast<Model*>(model->roots[2].get());
    CHECK(pivoted->worldPivotData() == CFrame(1, 2, 3));
    auto* mesh = dynamic_cast<MeshPart*>(model->roots[3].get());
    CHECK_EQ(mesh->meshId(), "rbxassetid://123");
    CHECK_EQ(mesh->textureId(), "");
    CHECK(mesh->customPhysicalProperties() == PhysicalProperties{3, 0.1f, 0.2f, 4, 5});
}

TEST(gui_classes_load_their_layout)
{
    auto model = loadModel(R"(<roblox version="4">
        <Item class="ScreenGui"><Properties>
            <int name="DisplayOrder">5</int>
            <token name="ZIndexBehavior">1</token>
        </Properties>
            <Item class="TextLabel"><Properties>
                <UDim2 name="Size"><XS>0.5</XS><XO>10</XO><YS>0</YS><YO>40</YO></UDim2>
                <Vector2 name="AnchorPoint"><X>0.5</X><Y>0</Y></Vector2>
                <string name="Text">Hello</string>
                <Font name="FontFace">
                    <Family><url>rbxasset://fonts/families/GothamSSm.json</url></Family>
                    <Weight>700</Weight><Style>Italic</Style>
                    <CachedFaceId><url>rbxasset://fonts/GothamSSm-BoldItalic.otf</url></CachedFaceId>
                </Font>
                <token name="TextXAlignment">0</token>
            </Properties>
                <Item class="UICorner"><Properties>
                    <UDim name="CornerRadius"><S>0.25</S><O>4</O></UDim>
                </Properties></Item>
            </Item>
            <Item class="ImageLabel"><Properties>
                <Content name="Image"><url>rbxassetid://42</url></Content>
                <Rect2D name="SliceCenter"><min><X>8</X><Y>8</Y></min><max><X>24</X><Y>24</Y></max></Rect2D>
                <token name="ScaleType">1</token>
            </Properties></Item>
        </Item>
    </roblox>)");
    CHECK(model.has_value());
    CHECK_EQ(model->warnings, std::vector<std::string>{});
    auto* gui = dynamic_cast<ScreenGui*>(model->roots[0].get());
    CHECK_EQ(gui->displayOrder(), 5);
    CHECK(gui->zIndexBehavior() == Enum::ZIndexBehavior::Sibling);
    CHECK(dynamic_cast<ScreenGui&>(*ClassRegistry::create("ScreenGui")).zIndexBehavior()
          == Enum::ZIndexBehavior::Global);
    auto* label = dynamic_cast<TextLabel*>(gui->findFirstChild("TextLabel"));
    CHECK(label != nullptr);
    CHECK(label->size() == (UDim2{{0.5f, 10}, {0, 40}}));
    CHECK(label->anchorPoint() == (Vector2{0.5f, 0}));
    CHECK_EQ(label->text(), "Hello");
    CHECK(label->fontFace()
          == (Font{"rbxasset://fonts/families/GothamSSm.json", Enum::FontWeight::Bold, Enum::FontStyle::Italic}));
    CHECK(label->textXAlignment() == Enum::TextXAlignment::Left);
    CHECK(label->isA("GuiObject"));
    auto* corner = dynamic_cast<UICorner*>(label->findFirstChild("UICorner"));
    CHECK(corner != nullptr);
    CHECK(corner->cornerRadius() == (UDim{0.25f, 4}));
    auto* image = dynamic_cast<ImageLabel*>(gui->findFirstChild("ImageLabel"));
    CHECK(image != nullptr);
    CHECK_EQ(image->image(), "rbxassetid://42");
    CHECK(image->sliceCenter() == (Rect{{8, 8}, {24, 24}}));
    CHECK(image->scaleType() == Enum::ScaleType::Slice);
}

TEST(workspace_collision_groups)
{
    auto legacy = loadPlace(R"(<roblox version="4"><Item class="Workspace"><Properties>
        <string name="CollisionGroups">Default^0^-5\PlayerCharacters^1^-1\Fixtures^2^-2</string>
    </Properties></Item></roblox>)");
    CHECK(legacy.has_value());
    CHECK(legacy->warnings.empty());
    Workspace& workspace = *legacy->game->workspace();
    CHECK_EQ(workspace.collisionGroups().size(), 3u);
    CHECK(workspace.collisionGroups()[2] == (CollisionGroup{"Fixtures", 2, 0xFFFFFFFE}));
    CHECK(!workspace.collisionGroupsCollide("Default", "Fixtures"));
    CHECK(workspace.collisionGroupsCollide("PlayerCharacters", "Fixtures"));
    CHECK(workspace.collisionGroupsCollide("Default", "PlayerCharacters"));
    CHECK(workspace.collisionGroupsCollide("Nope", "Default"));
    // A group the network sent by its id alone (CollisionGroupReplicate "\x02").
    CHECK(!workspace.collisionGroupsCollide(std::string_view("\x02", 1), "Default"));
    CHECK(workspace.collisionGroupsCollide(std::string_view("\x01", 1), std::string_view("\x02", 1)));

    auto binary = loadPlace(R"(<roblox version="4"><Item class="Workspace"><Properties>
        <BinaryString name="CollisionGroupData">AQIABP3///8HRGVmYXVsdAEE/v///wRXYWxs</BinaryString>
    </Properties></Item></roblox>)");
    CHECK(binary.has_value());
    CHECK(binary->warnings.empty());
    const auto& groups = binary->game->workspace()->collisionGroups();
    CHECK_EQ(groups.size(), 2u);
    CHECK(groups[1] == (CollisionGroup{"Wall", 1, 0xFFFFFFFE}));
    CHECK(!binary->game->workspace()->collisionGroupsCollide("Default", "Wall"));

    auto broken = loadPlace(R"(<roblox version="4"><Item class="Workspace"><Properties>
        <string name="CollisionGroups">Default^zero^1</string>
    </Properties></Item></roblox>)");
    CHECK_EQ(broken->warnings,
             (std::vector<std::string>{"Workspace.CollisionGroups: bad collision group 'Default^zero^1'"}));
}

TEST(runtime_properties_are_read_only_for_scripts)
{
    auto game = DataModel::create();
    auto* players = game->getService<Players>();
    auto player = std::make_shared<Player>();
    player->setParent(players);
    CHECK(players->localPlayer() == nullptr);
    players->assignLocalPlayer(player.get());
    CHECK(players->get("LocalPlayer") == Value(InstancePtr(player)));
    CHECK_THROWS(players->set("LocalPlayer", Value()), Error);
    CHECK_THROWS(ClassRegistry::create("Player"), Error); // only the engine makes players

    Humanoid humanoid;
    humanoid.updateMovement({0, 0, -1}, Enum::Material::Grass);
    CHECK(humanoid.get("FloorMaterial") == Value(enums::item(Enum::Material::Grass)));
    CHECK_THROWS(humanoid.set("MoveDirection", Vector3{}), Error);
    humanoid.set("Sit", true);
    CHECK(humanoid.sit());

    Camera camera;
    camera.updateViewportSize({1920, 1080});
    CHECK(camera.get("ViewportSize") == Value(Vector2{1920, 1080}));
    CHECK_THROWS(camera.set("ViewportSize", Vector2{}), Error);
}

TEST(network_content_names_reach_the_maps)
{
    // The network names SurfaceAppearance's and MaterialVariant's maps
    // "<Map>Content"; they're the ColorMap, NormalMap... properties.
    SurfaceAppearance appearance;
    const Property* color = findSerializedProperty(appearance, "ColorMapContent");
    CHECK(color != nullptr);
    color->set(appearance, "rbxassetid://1"s);
    CHECK_EQ(appearance.colorMap(), "rbxassetid://1");
    MaterialVariant variant;
    const Property* roughness = findSerializedProperty(variant, "RoughnessMapContent");
    CHECK(roughness != nullptr);
    roughness->set(variant, "rbxassetid://2"s);
    CHECK_EQ(variant.roughnessMap(), "rbxassetid://2");
    // TextureContent is each class's own texture: MeshPart.TextureID,
    // Decal.Texture and SpecialMesh.TextureId (the game's hair lost its texture).
    SpecialMesh mesh;
    const Property* meshTexture = findSerializedProperty(mesh, "TextureContent");
    CHECK(meshTexture != nullptr);
    meshTexture->set(mesh, "rbxassetid://3"s);
    CHECK_EQ(mesh.textureId(), "rbxassetid://3");
    MeshPart part;
    CHECK_EQ(findSerializedProperty(part, "TextureContent")->name, "TextureID");
    Decal decal;
    CHECK_EQ(findSerializedProperty(decal, "TextureContent")->name, "Texture");
}

TEST(material_variants_are_found_by_name_and_base_material)
{
    auto service = std::make_shared<MaterialService>();
    auto folder = make<Folder>(service.get(), "Variants");
    auto carpet = make<MaterialVariant>(folder.get(), "Carpet");
    carpet->setBaseMaterial(Enum::Material::Fabric);
    auto boards = make<MaterialVariant>(service.get(), "Boards");
    boards->setBaseMaterial(Enum::Material::WoodPlanks);

    // A part's own MaterialVariant, in a folder too, of its base material only.
    CHECK(findMaterialVariant(*service, Enum::Material::Fabric, "Carpet") == carpet.get());
    CHECK(findMaterialVariant(*service, Enum::Material::Plastic, "Carpet") == nullptr);
    CHECK(findMaterialVariant(*service, Enum::Material::Fabric, "Missing") == nullptr);
    // Without one, MaterialService's <Material>Name, unless it names the material itself.
    CHECK(findMaterialVariant(*service, Enum::Material::WoodPlanks) == nullptr);
    service->setWoodPlanksName("Boards");
    CHECK(findMaterialVariant(*service, Enum::Material::WoodPlanks) == boards.get());
    CHECK(findMaterialVariant(*service, Enum::Material::WoodPlanks, "Missing") == boards.get());
    // Materials without a <Material>Name keep their own look.
    CHECK(findMaterialVariant(*service, Enum::Material::Neon) == nullptr);
}

// A live server's CurrentCamera is its own Camera, which never replicates (or
// goes away again): the client's is in the Workspace, as Workspace.Camera.
TEST(workspace_current_camera_lives_in_the_workspace)
{
    auto game = DataModel::create();
    Workspace& ws = *game->workspace();
    auto loose = std::make_shared<Camera>();
    ws.setCurrentCamera(loose.get());
    CHECK(ws.ensureCurrentCamera() == loose.get());
    CHECK(loose->parent() == &ws);

    loose->destroy();
    Camera* fresh = ws.ensureCurrentCamera();
    CHECK(fresh != loose.get());
    CHECK(fresh->parent() == &ws);
    CHECK(ws.currentCamera() == fresh);
}

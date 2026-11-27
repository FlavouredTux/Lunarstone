#include "framework.hpp"

#include <lunar/PlaceFile.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/avatar/Character.hpp>
#include <lunar/avatar/Clothing.hpp>
#include <lunar/avatar/Rig.hpp>
#include <lunar/render/Image.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <source_location>
#include <string>
#include <thread>

using namespace lunar;
using namespace lunar::avatar;

namespace {

bool near(float a, float b, float tolerance = 1e-3f)
{
    return std::abs(a - b) <= tolerance;
}

std::shared_ptr<assets::AssetCache> offlineCache()
{
    assets::AssetConfig config = assets::AssetConfig::fromEnvironment();
    config.download = false;
    return std::make_shared<assets::AssetCache>(config);
}

bool haveContent()
{
    return std::filesystem::exists(assets::AssetConfig::fromEnvironment().contentDir / "avatar/characterR15.rbxm");
}

std::shared_ptr<Model> loadRig(std::string_view file)
{
    auto bytes = readFile(assets::AssetConfig::fromEnvironment().contentDir / "avatar" / file);
    auto model = loadModel(*bytes);
    return std::dynamic_pointer_cast<Model>(model->roots.front());
}

BasePart* part(const Model& model, std::string_view name)
{
    auto* found = dynamic_cast<BasePart*>(model.findFirstChild(name));
    if (!found)
        ::lt::fail(std::format("the rig has no part {}", name), std::source_location::current());
    return found;
}

} // namespace

TEST(avatar_hip_height_matches_the_stock_rigs)
{
    if (!haveContent())
        SKIP("no RobloxPlayer content folder");
    // The formula reproduces the HipHeight each rig file stores.
    auto classic = loadRig("characterR15.rbxm");
    auto hip = computeR15HipHeight(*classic);
    CHECK(hip.has_value() && near(*hip, 1.35f));
    auto dynamic = loadRig("characterR15DynamicHeadV2.rbxm");
    hip = computeR15HipHeight(*dynamic);
    CHECK(hip.has_value() && near(*hip, 2.0f));
}

TEST(avatar_rthro_scale_tables)
{
    // BodyTypeScale 0 leaves every part alone.
    CHECK(rthroScale("LeftUpperLeg", "Classic", 0.0f, 0.7f) == (Vector3{1.0f, 1.0f, 1.0f}));
    // Full body type, proportion 0: table A (LeftUpperLeg 1.023, 1.506, 1.023).
    const Vector3 a = rthroScale("LeftUpperLeg", "", 1.0f, 0.0f);
    CHECK(near(a.x, 1.023f) && near(a.y, 1.506f) && near(a.z, 1.023f));
    // Proportions parts are divided by their own table, so A on a
    // ProportionsNormal part is 1.
    const Vector3 normal = rthroScale("LeftUpperLeg", "ProportionsNormal", 1.0f, 0.0f);
    CHECK(near(normal.x, 1.0f) && near(normal.y, 1.0f));
    // Unknown parts (the root) don't scale by body type.
    CHECK(rthroScale("HumanoidRootPart", "", 1.0f, 1.0f) == (Vector3{1.0f, 1.0f, 1.0f}));
}

TEST(avatar_automatic_scaling_resizes_from_originals)
{
    if (!haveContent())
        SKIP("no RobloxPlayer content folder");
    auto rig = loadRig("characterR15.rbxm");
    auto* humanoid = dynamic_cast<Humanoid*>(rig->findFirstChildOfClass("Humanoid"));
    // Without scale values nothing happens.
    CHECK(!applyAutomaticScaling(*rig));
    auto height = std::make_shared<NumberValue>();
    height->setName("BodyHeightScale");
    height->setValue(2.0);
    height->setParent(humanoid);
    const Vector3 before = part(*rig, "LeftUpperLeg")->size();
    buildRigFromAttachments(*rig);
    const Vector3 after = part(*rig, "LeftUpperLeg")->size();
    CHECK(near(after.y, before.y * 2.0f));
    CHECK(near(after.x, before.x));
    // Twice as tall legs, about twice the hip height (the root part keeps
    // its share in the formula).
    CHECK(humanoid->hipHeight() > 2.5f);
    // Scaling again from the originals gives the same sizes.
    buildRigFromAttachments(*rig);
    CHECK(near(part(*rig, "LeftUpperLeg")->size().y, after.y));
}

TEST(avatar_create_default_r15_character)
{
    if (!haveContent())
        SKIP("no RobloxPlayer content folder");
    CharacterOptions options;
    options.assets = offlineCache();
    options.name = "Builderman";
    auto character = createCharacter(options);
    CHECK(character.has_value());
    const Model& model = **character;
    CHECK_EQ(model.name(), "Builderman");
    BasePart* root = model.primaryPart();
    if (!root)
        lt::fail("no PrimaryPart", std::source_location::current());
    CHECK_EQ(root->name(), "HumanoidRootPart");
    const auto* humanoid = dynamic_cast<const Humanoid*>(model.findFirstChildOfClass("Humanoid"));
    if (!humanoid)
        lt::fail("no Humanoid", std::source_location::current());
    CHECK(humanoid->rigType() == Enum::HumanoidRigType::R15);
    // 2 for the dynamic head rig, 1.35 for the classic one it falls back to
    // when the head mesh can't be downloaded.
    CHECK(near(humanoid->hipHeight(), computeR15HipHeight(model).value_or(-1.0f)));
    CHECK(humanoid->findFirstChildOfClass("Animator") != nullptr);
    // Standing on y = 0: the root floats HipHeight above it.
    CHECK(near(root->position().y, humanoid->hipHeight() + root->size().y * 0.5f));
    // The Animate object carries the animation sets.
    const Instance* animate = model.findFirstChild("Animate");
    if (!animate)
        lt::fail("no Animate", std::source_location::current());
    CHECK(animate->findFirstChild("walk") != nullptr);
    CHECK(model.findFirstChildOfClass("Shirt") != nullptr);
    CHECK(model.findFirstChildOfClass("Pants") != nullptr);
    // Feet on the floor, head on top, all joined up.
    const BasePart* foot = part(model, "LeftFoot");
    CHECK(near(foot->position().y - foot->size().y * 0.5f, 0.0f, 0.05f));
    CHECK(part(model, "Head")->position().y > part(model, "UpperTorso")->position().y);
}

TEST(avatar_create_r6_character_builds_its_joints)
{
    if (!haveContent())
        SKIP("no RobloxPlayer content folder");
    CharacterOptions options;
    options.assets = offlineCache();
    options.rigType = Enum::HumanoidRigType::R6;
    auto character = createCharacter(options);
    CHECK(character.has_value());
    const Model& model = **character;
    const auto* shoulder = dynamic_cast<const Motor6D*>(part(model, "Torso")->findFirstChild("Right Shoulder"));
    CHECK(shoulder != nullptr);
    CHECK(shoulder->part1() == part(model, "Right Arm"));
    // CFrame.new(1, 0.5, 0, 0, 0, 1, 0, 1, 0, -1, 0, 0)
    CHECK(near(shoulder->c0().position.x, 1.0f) && near(shoulder->c0().r[0][2], 1.0f)
          && near(shoulder->c0().r[2][0], -1.0f));
    CHECK(part(model, "HumanoidRootPart")->findFirstChild("RootJoint") != nullptr);
    // Arms hang beside the torso, legs reach the floor.
    CHECK(near(part(model, "Right Arm")->position().x, 1.5f));
    CHECK(near(part(model, "Left Leg")->position().y, 1.0f));
    CHECK(near(part(model, "Head")->position().y, 4.5f));
}

TEST(avatar_accessory_weld_convention)
{
    auto character = std::make_shared<Model>();
    auto head = std::make_shared<Part>();
    head->setName("Head");
    head->setParent(character.get());
    auto hat = std::make_shared<Attachment>();
    hat->setName("HatAttachment");
    hat->setCFrame(CFrame(0.0f, 0.6f, 0.0f));
    hat->setParent(head.get());

    auto accessory = std::make_shared<Accessory>();
    accessory->setParent(character.get());
    auto handle = std::make_shared<Part>();
    handle->setName("Handle");
    handle->setParent(accessory.get());
    auto own = std::make_shared<Attachment>();
    own->setName("HatAttachment");
    own->setCFrame(CFrame(0.0f, -0.2f, 0.0f));
    own->setParent(handle.get());

    CHECK(attachAccessory(*character, *accessory));
    const auto* weld = dynamic_cast<const Weld*>(handle->findFirstChild("AccessoryWeld"));
    CHECK(weld != nullptr);
    CHECK(weld->part0() == handle.get());
    CHECK(weld->part1() == head.get());
    CHECK(near(weld->c0().position.y, -0.2f));
    CHECK(near(weld->c1().position.y, 0.6f));
    // The two attachments meet: handle = head * C1 * C0^-1.
    solveJoints(*character, *head);
    CHECK(near(handle->position().y, 0.8f));
}

TEST(avatar_composite_draws_template_pixels_into_layout)
{
    // A 2x2 source: red top-left, green top-right, blue bottom-left, clear bottom-right.
    Image source;
    source.width = 2;
    source.height = 2;
    source.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 0, 0, 0, 0};
    // A layout quad covering a 4x4 target, UVs over the whole source.
    MeshData layout;
    auto vertex = [](float x, float y, float u, float v) {
        MeshVertex out;
        out.position = {x, y, 0.0f};
        out.u = u;
        out.v = v;
        return out;
    };
    layout.vertices = {vertex(0, 0, 0, 1), vertex(4, 0, 1, 1), vertex(4, 4, 1, 0), vertex(0, 4, 0, 0)};
    layout.indices = {0, 1, 2, 0, 2, 3};
    Image target;
    target.width = 4;
    target.height = 4;
    target.rgba.assign(target.byteSize(), 0);
    drawComposite(target, layout, source);
    auto pixel = [&](std::size_t x, std::size_t y) { return &target.rgba[(y * 4 + x) * 4]; };
    // Layout Y runs up, so the source's top row lands on the target's top.
    CHECK_EQ(pixel(0, 0)[0], 255);
    CHECK_EQ(pixel(3, 0)[1], 255);
    CHECK_EQ(pixel(0, 3)[2], 255);
    CHECK_EQ(pixel(3, 3)[3], 0); // transparent source leaves it clear
}

// drawComposite splits a big target into bands of rows drawn on threads of
// their own; every pixel must come out as one pass over the triangles in
// order would leave it, overlaps and blending included.
TEST(avatar_composite_bands_match_one_pass)
{
    Image source;
    source.width = 64;
    source.height = 64;
    source.rgba.resize(source.byteSize());
    std::uint32_t seed = 12345;
    auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (std::uint8_t& byte : source.rgba)
        byte = static_cast<std::uint8_t>(next());
    auto unit = [&] { return static_cast<float>(next() % 100000) / 100000.0f; };
    // Overlapping triangles over a 1024x512 atlas (R6's), some off its edges.
    MeshData layout;
    for (int t = 0; t < 120; ++t) {
        const float cx = unit() * 1100.0f - 40.0f, cy = unit() * 560.0f - 20.0f;
        for (int k = 0; k < 3; ++k) {
            MeshVertex vertex;
            vertex.position = {cx + (unit() - 0.5f) * 300.0f, cy + (unit() - 0.5f) * 200.0f, 0.0f};
            vertex.u = unit();
            vertex.v = unit();
            layout.indices.push_back(static_cast<std::uint32_t>(layout.vertices.size()));
            layout.vertices.push_back(vertex);
        }
    }
    Image target;
    target.width = 1024;
    target.height = 512;
    target.rgba.assign(target.byteSize(), 0);
    drawComposite(target, layout, source);
    drawComposite(target, layout, source); // over itself: blending what's there

    // One pass, as drawComposite was first written.
    Image expected;
    expected.width = target.width;
    expected.height = target.height;
    expected.rgba.assign(expected.byteSize(), 0);
    auto draw = [&] {
        for (std::size_t i = 0; i + 2 < layout.indices.size(); i += 3) {
            const MeshVertex& a = layout.vertices[layout.indices[i]];
            const MeshVertex& b = layout.vertices[layout.indices[i + 1]];
            const MeshVertex& c = layout.vertices[layout.indices[i + 2]];
            const float area = (b.position.x - a.position.x) * (c.position.y - a.position.y)
                               - (c.position.x - a.position.x) * (b.position.y - a.position.y);
            if (std::abs(area) < 1e-9f)
                continue;
            const float minX = std::max(0.0f, std::floor(std::min({a.position.x, b.position.x, c.position.x})));
            const float maxX = std::min(1023.0f, std::ceil(std::max({a.position.x, b.position.x, c.position.x})));
            const float minY = std::max(0.0f, std::floor(std::min({a.position.y, b.position.y, c.position.y})));
            const float maxY = std::min(511.0f, std::ceil(std::max({a.position.y, b.position.y, c.position.y})));
            if (maxX < minX || maxY < minY)
                continue;
            for (auto y = static_cast<std::uint32_t>(minY); y <= static_cast<std::uint32_t>(maxY); ++y)
                for (auto x = static_cast<std::uint32_t>(minX); x <= static_cast<std::uint32_t>(maxX); ++x) {
                    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                    const float wa
                        = ((b.position.x - px) * (c.position.y - py) - (c.position.x - px) * (b.position.y - py))
                          / area;
                    const float wb
                        = ((c.position.x - px) * (a.position.y - py) - (a.position.x - px) * (c.position.y - py))
                          / area;
                    const float wc = 1.0f - wa - wb;
                    if (wa < -1e-4f || wb < -1e-4f || wc < -1e-4f)
                        continue;
                    const float u = wa * a.u + wb * b.u + wc * c.u, v = wa * a.v + wb * b.v + wc * c.v;
                    const auto sx = static_cast<std::uint32_t>(std::clamp(u * 64.0f, 0.0f, 63.0f));
                    const auto sy = static_cast<std::uint32_t>(std::clamp(v * 64.0f, 0.0f, 63.0f));
                    const std::uint8_t* texel = &source.rgba[(sy * 64 + sx) * 4];
                    const float alpha = static_cast<float>(texel[3]) / 255.0f;
                    if (alpha <= 0.0f)
                        continue;
                    std::uint8_t* out = &expected.rgba[((511 - y) * 1024 + x) * 4];
                    const float below = static_cast<float>(out[3]) / 255.0f;
                    const float total = alpha + below * (1.0f - alpha);
                    for (std::size_t k = 0; k < 3; ++k) {
                        const float mixed = (static_cast<float>(texel[k]) * alpha
                                             + static_cast<float>(out[k]) * below * (1.0f - alpha))
                                            / std::max(total, 1e-6f);
                        out[k] = static_cast<std::uint8_t>(std::clamp(std::lround(mixed), 0L, 255L));
                    }
                    out[3] = static_cast<std::uint8_t>(std::clamp(std::lround(total * 255.0f), 0L, 255L));
                }
        }
    };
    draw();
    draw();
    std::size_t differing = 0, drawn = 0;
    for (std::size_t i = 0; i < target.rgba.size(); ++i) {
        differing += target.rgba[i] != expected.rgba[i];
        drawn += expected.rgba[i] != 0;
    }
    CHECK_EQ(differing, std::size_t{0});
    CHECK(drawn > target.rgba.size() / 4); // the triangles cover a good part of it
}

// A live client dresses characters as they arrive and can't stall a frame
// on a download: ready() only asks for the templates, and is true once each
// has loaded or failed.
TEST(avatar_clothing_ready_asks_without_waiting)
{
    const std::filesystem::path dir
        = std::filesystem::temp_directory_path()
          / std::format("lunarstone-clothing-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
    {
        render::PixelImage red{2, 2, std::vector<render::Rgba>(4, {255, 0, 0, 255})};
        std::ofstream(dir / "123", std::ios::binary) << render::encodePng(red);
    }
    auto cache = std::make_shared<assets::AssetCache>(
        assets::AssetConfig{.contentDir = dir, .cacheDir = dir, .download = false});
    ClothingCompositor clothing(cache);
    auto character = std::make_shared<Model>();
    CHECK(clothing.ready(*character)); // nothing to wear
    auto shirt = std::make_shared<Shirt>();
    shirt->setShirtTemplate("rbxassetid://123");
    shirt->setParent(character);
    auto pants = std::make_shared<Pants>();
    pants->setPantsTemplate("rbxassetid://456"); // not in the cache, and no downloads: it fails
    pants->setParent(character);
    bool ready = false;
    for (int i = 0; i < 500 && !ready; ++i) {
        ready = clothing.ready(*character);
        if (!ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(ready);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// A live client draws a character's clothing on a thread of its own:
// prepare on the instances' thread, composite on another, finish back on
// the first. That gives what apply() does in one go: the same parts
// textured, with the same ids, the same images.
TEST(avatar_clothing_drawn_elsewhere_matches_apply)
{
    if (!haveContent())
        SKIP("no RobloxPlayer content folder");
    const std::filesystem::path dir
        = std::filesystem::temp_directory_path()
          / std::format("lunarstone-clothing-split-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
    {
        // Templates with something in them: stripes, and a half clear.
        render::PixelImage shirt{585, 559, std::vector<render::Rgba>(585 * 559)};
        render::PixelImage pants = shirt;
        for (std::uint32_t y = 0; y < 559; ++y)
            for (std::uint32_t x = 0; x < 585; ++x) {
                shirt.pixels[y * 585 + x] = {static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), 40, 255};
                pants.pixels[y * 585 + x]
                    = {20, static_cast<std::uint8_t>(x + y), 200, static_cast<std::uint8_t>(x < 290 ? 255 : 0)};
            }
        std::ofstream(dir / "501", std::ios::binary) << render::encodePng(shirt);
        std::ofstream(dir / "502", std::ios::binary) << render::encodePng(pants);
    }
    assets::AssetConfig config = assets::AssetConfig::fromEnvironment();
    config.cacheDir = dir;
    config.download = false;
    auto dress = [](Model& character) {
        auto shirt = std::make_shared<Shirt>();
        shirt->setShirtTemplate("rbxassetid://501");
        shirt->setParent(&character);
        auto pants = std::make_shared<Pants>();
        pants->setPantsTemplate("rbxassetid://502");
        pants->setParent(&character);
    };
    for (bool r6 : {false, true}) {
        std::shared_ptr<Model> together, apart;
        if (r6) {
            together = std::make_shared<Model>();
            auto humanoid = std::make_shared<Humanoid>();
            humanoid->setRigType(Enum::HumanoidRigType::R6);
            humanoid->setParent(together);
            apart = std::static_pointer_cast<Model>(together->clone());
        } else {
            together = loadRig("characterR15.rbxm");
            apart = loadRig("characterR15.rbxm");
        }
        dress(*together);
        dress(*apart);
        ClothingCompositor one(std::make_shared<assets::AssetCache>(config));
        ClothingCompositor two(std::make_shared<assets::AssetCache>(config));
        CHECK(one.apply(*together));
        std::optional<ClothingCompositor::Dressing> dressing = two.prepare(*apart);
        CHECK(dressing.has_value());
        std::jthread([&] { two.composite(*dressing); }).join();
        CHECK(two.finish(*apart, *dressing));
        // The same ids on the same parts (or the Humanoid), the same images.
        std::vector<std::string> ids;
        if (r6) {
            const auto* a = dynamic_cast<const Humanoid*>(together->findFirstChildOfClass("Humanoid"));
            const auto* b = dynamic_cast<const Humanoid*>(apart->findFirstChildOfClass("Humanoid"));
            CHECK_EQ(a->bodyTexture(), b->bodyTexture());
            ids.push_back(a->bodyTexture());
        } else {
            for (const InstancePtr& child : together->children())
                if (const auto* part = dynamic_cast<const MeshPart*>(child.get());
                    part && part->textureId().starts_with("rbxlunar-composite://")) {
                    const auto* other = dynamic_cast<const MeshPart*>(apart->findFirstChild(part->name()));
                    CHECK(other && other->textureId() == part->textureId());
                    ids.push_back(part->textureId());
                }
            CHECK(ids.size() >= 13); // every limb group's parts
        }
        bool drawn = false;
        for (const std::string& id : ids) {
            const auto a = one.image(id), b = two.image(id);
            CHECK(a && b && a->rgba == b->rgba);
            drawn = drawn || (a && std::ranges::any_of(a->rgba, [](std::uint8_t v) { return v != 0; }));
        }
        CHECK(drawn);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

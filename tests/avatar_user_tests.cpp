// Real users' avatars: the avatar-fetch reply as a HumanoidDescription, user
// lookups, and applying a dynamic head and an animation pack. Everything
// goes through the fake transport; the live test is opt-in and anonymous.

#include "fake_web.hpp"
#include "framework.hpp"

#include <lunar/assets/AssetCache.hpp>
#include <lunar/avatar/Character.hpp>
#include <lunar/avatar/UserAvatar.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <string>

using namespace lunar;
using namespace lunar::avatar;
using namespace fakeweb;

namespace {

// avatar.roblox.com/v2/avatar/avatar-fetch/?placeId=0&userId=1, the shape
// the endpoint answered with (public data).
constexpr std::string_view robloxAvatar
    = R"({"resolvedAvatarType":"R15","equippedGearVersionIds":[],"backpackGearVersionIds":[],"assetAndAssetTypeIds":[{"assetId":301811432,"assetTypeId":12},{"assetId":607702162,"assetTypeId":8},{"assetId":607785314,"assetTypeId":11},{"assetId":10638267973,"assetTypeId":79},{"assetId":10647852134,"assetTypeId":78},{"assetId":12995014400,"assetTypeId":29},{"assetId":12995015868,"assetTypeId":30},{"assetId":12995017412,"assetTypeId":28},{"assetId":12995018829,"assetTypeId":31},{"assetId":12995020128,"assetTypeId":27}],"animationAssetIds":{"climb":2510230574,"fall":2510233257,"idle":2510235063,"jump":2510236649,"run":2510238627,"swim":2510240941,"walk":2510242378,"mood":10647852134},"bodyColor3s":{"headColor3":"A3A2A5","torsoColor3":"0D69AC","rightArmColor3":"A3A2A5","leftArmColor3":"A3A2A5","rightLegColor3":"6E99CA","leftLegColor3":"6E99CA"},"scales":{"height":1.0,"width":1.0,"head":1.0,"depth":1.00,"proportion":0.0,"bodyType":0.0},"emotes":[]})";

class TempDir {
public:
    TempDir()
    {
        static std::atomic<int> next = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / std::format("lunarstone-avatar-{}-{}", stamp, next++);
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// One triangle as a version 2.00 mesh.
std::string triangleMesh()
{
    std::string out = "version 2.00\n";
    auto put = [&](auto value) {
        char raw[sizeof(value)];
        std::memcpy(raw, &value, sizeof(value));
        out.append(raw, sizeof(value));
    };
    put(std::uint16_t{12});
    put(std::uint8_t{36});
    put(std::uint8_t{12});
    put(std::uint32_t{3});
    put(std::uint32_t{1});
    for (int i = 0; i < 3; ++i) {
        put(i == 1 ? 1.0f : 0.0f);
        put(i == 2 ? 1.0f : 0.0f);
        put(0.0f);
        for (float f : {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f})
            put(f);
    }
    for (std::uint32_t i : {0u, 1u, 2u})
        put(i);
    return out;
}

std::string model(std::string_view items)
{
    return std::format(R"(<roblox version="4">{}</roblox>)", items);
}

bool haveContent()
{
    return std::filesystem::exists(assets::AssetConfig::fromEnvironment().contentDir / "avatar/characterR15.rbxm");
}

} // namespace

TEST(avatar_fetch_becomes_a_description)
{
    auto avatar = parseAvatarFetch(robloxAvatar);
    if (!avatar)
        lt::fail(avatar.error(), std::source_location::current());
    CHECK_EQ(avatar->rigType, Enum::HumanoidRigType::R15);
    const HumanoidDescription& d = *avatar->description;
    // A DynamicHead (79) is the Head.
    CHECK_EQ(d.head(), 10638267973);
    CHECK_EQ(d.torso(), 12995020128);
    CHECK_EQ(d.leftArm(), 12995014400);
    CHECK_EQ(d.rightArm(), 12995017412);
    CHECK_EQ(d.leftLeg(), 12995015868);
    CHECK_EQ(d.rightLeg(), 12995018829);
    CHECK_EQ(d.shirt(), 607785314);
    CHECK_EQ(d.pants(), 301811432);
    CHECK_EQ(d.hatAccessory(), "607702162");
    CHECK_EQ(d.idleAnimation(), 2510235063);
    CHECK_EQ(d.walkAnimation(), 2510242378);
    CHECK_EQ(d.moodAnimation(), 10647852134);
    CHECK(d.torsoColor() == Color3::fromRGB(0x0D, 0x69, 0xAC));
    CHECK(d.leftLegColor() == Color3::fromRGB(0x6E, 0x99, 0xCA));
    CHECK_EQ(d.bodyTypeScale(), 0.0f);
    CHECK_EQ(d.proportionScale(), 0.0f);
    CHECK_EQ(d.heightScale(), 1.0f);
}

TEST(avatar_fetch_r6_layered_clothing_and_emotes)
{
    const std::string_view json
        = R"({"resolvedAvatarType":"R6","assetAndAssetTypeIds":[{"assetId":1142116,"assetTypeId":2},{"assetId":1080951,"assetTypeId":8},{"assetId":11,"assetTypeId":8},{"assetId":12,"assetTypeId":41},{"assetId":13,"assetTypeId":67,"meta":{"order":5,"puffiness":0.5,"version":1}},{"assetId":14,"assetTypeId":76},{"assetId":15,"assetTypeId":19},{"assetId":16,"assetTypeId":61}],"bodyColors":{"headColorId":24,"torsoColorId":23},"emotes":[{"assetId":3576686446,"assetName":"Hello","position":1}]})";
    auto avatar = parseAvatarFetch(json);
    if (!avatar)
        lt::fail(avatar.error(), std::source_location::current());
    CHECK_EQ(avatar->rigType, Enum::HumanoidRigType::R6);
    const HumanoidDescription& d = *avatar->description;
    CHECK_EQ(d.graphicTShirt(), 1142116);
    CHECK_EQ(d.hatAccessory(), "1080951,11");
    CHECK_EQ(d.hairAccessory(), "12");
    // The older BrickColor numbers.
    CHECK(d.headColor() == BrickColor::fromNumber(24).color());
    CHECK(d.torsoColor() == BrickColor::fromNumber(23).color());
    // Layered clothing and eyebrows become AccessoryDescriptions.
    std::vector<const AccessoryDescription*> items;
    for (const InstancePtr& child : d.children())
        if (const auto* item = dynamic_cast<const AccessoryDescription*>(child.get()))
            items.push_back(item);
    CHECK_EQ(items.size(), 2u);
    CHECK_EQ(items[0]->assetId(), 13);
    CHECK_EQ(items[0]->accessoryType(), Enum::AccessoryType::Jacket);
    CHECK(items[0]->isLayered());
    CHECK_EQ(items[0]->order(), 5);
    CHECK_EQ(items[0]->puffiness(), 0.5f);
    CHECK_EQ(items[1]->accessoryType(), Enum::AccessoryType::Eyebrow);
    CHECK(!items[1]->isLayered());
    CHECK_EQ(avatar->gear.size(), 1u);
    CHECK_EQ(avatar->emotes.size(), 1u);
    CHECK_EQ(avatar->emotes[0].name, "Hello");
    CHECK_EQ(avatar->emotes[0].position, 1);
    CHECK_EQ(avatar->description->emotes().at("Hello"), (std::set<std::int64_t>{3576686446}));
    CHECK_EQ(avatar->description->equippedEmotes().at(1), "Hello");

    CHECK(!parseAvatarFetch("{}").has_value());
    CHECK(!parseAvatarFetch(R"({"resolvedAvatarType":"R7"})").has_value());
    CHECK(!parseAvatarFetch("[").has_value());
}

TEST(avatar_user_lookups)
{
    FakeServer server;
    server.answers["https://users.roblox.com/v1/users/156"]
        = {.body = R"({"hasVerifiedBadge":true,"id":156,"name":"builderman","displayName":"Builderman"})"};
    server.answers["https://users.roblox.com/v1/usernames/users"] = {
        .body
        = R"({"data":[{"requestedUsername":"builderman","id":156,"name":"builderman","displayName":"Builderman"}]})"};
    server.signedInAnswers["https://users.roblox.com/v1/users/authenticated"]
        = {.body = R"({"id":4242,"name":"someone","displayName":"Someone"})"};
    const assets::RobloxWeb web(server.transport(), fakeLogin());

    auto byId = findUser(web, "156");
    CHECK(byId.has_value());
    CHECK_EQ(byId->name, "builderman");
    CHECK_EQ(byId->displayName, "Builderman");
    CHECK_EQ(server.seen.back().method, "GET");

    auto byName = findUser(web, "builderman");
    CHECK(byName.has_value());
    CHECK_EQ(byName->id, 156);
    CHECK_EQ(server.seen.back().method, "POST");
    CHECK(server.seen.back().body.contains(R"("usernames":["builderman"])"));
    // Lookups are anonymous even with a login at hand.
    CHECK_EQ(server.seen.back().headers.size(), 2u);
    CHECK(!hasHeader(server.seen.back(), "Cookie: "));

    // Names that aren't usernames never go out.
    const std::size_t sent = server.seen.size();
    CHECK(!findUser(web, "bad name\"]}").has_value());
    CHECK(!findUser(web, "-5").has_value());
    CHECK(!findUser(web, "").has_value());
    CHECK_EQ(server.seen.size(), sent);
    server.answers["https://users.roblox.com/v1/usernames/users"] = {.body = R"({"data":[]})"};
    auto nobodyHere = findUser(web, "nobody_here");
    CHECK(!nobodyHere.has_value());
    CHECK(nobodyHere.error().contains("no user called"));

    // --me: the cookie goes to users.roblox.com, nowhere else.
    auto me = authenticatedUser(web);
    CHECK(me.has_value());
    CHECK_EQ(me->id, 4242);
    CHECK(hasHeader(server.seen.back(), "Cookie: "));
    const assets::RobloxWeb anonymousWeb(server.transport(), std::make_shared<const Credentials>());
    const std::size_t before = server.seen.size();
    auto nobody = authenticatedUser(anonymousWeb);
    CHECK(!nobody.has_value());
    CHECK(nobody.error().contains("not signed in"));
    CHECK_EQ(server.seen.size(), before);
    server.checkNoLeaks();
}

TEST(avatar_fetch_goes_out_anonymously)
{
    FakeServer server;
    server.answers["https://avatar.roblox.com/v2/avatar/avatar-fetch/?placeId=0&userId=1"]
        = {.body = std::string(robloxAvatar)};
    const assets::RobloxWeb web(server.transport(), fakeLogin());
    auto avatar = fetchUserAvatar(web, 1);
    CHECK(avatar.has_value());
    CHECK_EQ(avatar->description->torso(), 12995020128);
    CHECK(!hasHeader(server.seen.back(), "Cookie: "));
    CHECK(!fetchUserAvatar(web, 2).has_value()); // the fake answers 404
    CHECK(!fetchUserAvatar(web, 0).has_value());
    server.checkNoLeaks();
}

TEST(avatar_dynamic_head_and_animation_pack)
{
    if (!haveContent())
        SKIP("no RobloxPlayer content folder");
    // With a login, the client's MeshPart form of a dynamic head (the batch
    // API's avatar_meshpart_head) replaces the classic head; its mesh is only
    // there signed in. The animation pack fills Animate's sets.
    FakeServer server;
    const std::string denied = R"({"errors":[{"code":0,"message":"Authentication required to access Asset."}]})";
    // The plain download of a dynamic head: a classic SpecialMesh head.
    server.answers["https://assetdelivery.roblox.com/v1/asset/?id=900000"] = {
        .body = model(
            R"(<Item class="SpecialMesh" referent="A"><Properties><string name="Name">Mesh</string></Properties></Item>)")};
    server.answers["https://assetdelivery.roblox.com/v1/assets/batch"]
        = {.body = R"([{"location":"https://fts.rbxcdn.com/head","requestId":"0","assetTypeId":79}])"};
    server.answers["https://fts.rbxcdn.com/head"] = {
        .body = model(
            R"(<Item class="MeshPart" referent="H"><Properties><string name="Name">Head</string><Content name="MeshId"><url>rbxassetid://900001</url></Content><Vector3 name="size"><X>1.2</X><Y>1.3</Y><Z>1.2</Z></Vector3></Properties><Item class="Attachment" referent="N"><Properties><string name="Name">NeckRigAttachment</string><CoordinateFrame name="CFrame"><X>0</X><Y>-0.6</Y><Z>0</Z><R00>1</R00><R01>0</R01><R02>0</R02><R10>0</R10><R11>1</R11><R12>0</R12><R20>0</R20><R21>0</R21><R22>1</R22></CoordinateFrame></Properties></Item><Item class="FaceControls" referent="F"><Properties><string name="Name">FaceControls</string></Properties></Item></Item>)")};
    server.answers["https://assetdelivery.roblox.com/v1/asset/?id=900001"] = {.status = 401, .body = denied};
    server.signedInAnswers["https://assetdelivery.roblox.com/v2/asset/?id=900001"]
        = {.body = R"({"locations":[{"assetFormat":"source","location":"https://fts.rbxcdn.com/headmesh"}]})"};
    server.answers["https://fts.rbxcdn.com/headmesh"] = {.body = triangleMesh()};
    server.answers["https://assetdelivery.roblox.com/v1/asset/?id=900010"] = {
        .body = model(
            R"(<Item class="Folder" referent="R"><Properties><string name="Name">R15Anim</string></Properties><Item class="StringValue" referent="I"><Properties><string name="Name">idle</string></Properties><Item class="Animation" referent="A1"><Properties><string name="Name">Animation1</string><Content name="AnimationId"><url>rbxassetid://900011</url></Content></Properties></Item></Item><Item class="StringValue" referent="P"><Properties><string name="Name">pose</string></Properties></Item></Item>)")};

    TempDir dir;
    assets::AssetConfig config = assets::AssetConfig::fromEnvironment();
    config.cacheDir = dir.path();
    config.download = true;
    config.web = std::make_shared<const assets::RobloxWeb>(server.transport(), fakeLogin());
    auto cache = std::make_shared<assets::AssetCache>(config);

    auto description = std::make_shared<HumanoidDescription>();
    description->setHead(900000);
    description->setIdleAnimation(900010);
    description->setHeadColor(Color3::fromRGB(10, 20, 30));
    std::vector<std::string> problems;
    auto character = createCharacter({.description = description.get(), .assets = cache, .problems = &problems});
    if (!character)
        lt::fail(character.error(), std::source_location::current());
    for (const std::string& problem : problems) {
        CHECK(!problem.contains("900000"));
        CHECK(!problem.contains("900010"));
    }

    auto* head = dynamic_cast<MeshPart*>((*character)->findFirstChild("Head"));
    CHECK(head != nullptr);
    CHECK_EQ(head->meshId(), "rbxassetid://900001");
    CHECK(head->findFirstChildOfClass("FaceControls") != nullptr);
    CHECK(head->findFirstChild("face") == nullptr);
    CHECK(head->color() == Color3::fromRGB(10, 20, 30));
    // The neck was rebuilt onto the new head.
    auto* neck = dynamic_cast<Motor6D*>(head->findFirstChild("Neck"));
    CHECK(neck != nullptr);
    CHECK(neck->part1() == head);

    Instance* animate = (*character)->findFirstChild("Animate");
    CHECK(animate != nullptr);
    Instance* idle = animate->findFirstChild("idle");
    CHECK(idle != nullptr);
    CHECK_EQ(idle->children().size(), 1u);
    const auto* animation = dynamic_cast<const Animation*>(idle->findFirstChild("Animation1"));
    CHECK(animation != nullptr);
    CHECK_EQ(animation->animationId(), "rbxassetid://900011");
    CHECK(animate->findFirstChild("pose") != nullptr);
    CHECK(std::filesystem::is_regular_file(dir.path() / "900000.avatar_meshpart_head"));
    server.checkNoLeaks();

    // Without a login the mesh can't come, and the classic head stays.
    FakeServer anonymousServer;
    anonymousServer.answers = server.answers;
    TempDir other;
    config.cacheDir = other.path();
    config.web
        = std::make_shared<const assets::RobloxWeb>(anonymousServer.transport(), std::make_shared<const Credentials>());
    problems.clear();
    auto classic = createCharacter({.description = description.get(),
                                    .assets = std::make_shared<assets::AssetCache>(config),
                                    .problems = &problems});
    CHECK(classic.has_value());
    const auto* classicHead = dynamic_cast<const MeshPart*>((*classic)->findFirstChild("Head"));
    if (classicHead)
        CHECK(classicHead->meshId() != "rbxassetid://900001");
    bool reported = false;
    for (const std::string& problem : problems)
        reported = reported || problem.contains("keeping the classic head");
    CHECK(reported);
}

TEST(avatar_live_public_avatar)
{
    const char* enabled = std::getenv("LUNAR_NETWORK_TESTS"); // NOLINT(concurrency-mt-unsafe)
    if (!enabled || std::string_view(enabled) != "1")
        SKIP("set LUNAR_NETWORK_TESTS=1 to ask Roblox");
    if (!assets::curlAvailable())
        SKIP("built without LUNAR_WITH_CURL");
    // Public data, asked for with no login at all.
    const assets::RobloxWeb web(assets::curlTransport(), std::make_shared<const Credentials>());
    auto user = findUser(web, "builderman");
    if (!user)
        lt::fail(user.error(), std::source_location::current());
    CHECK_EQ(user->id, 156);
    auto avatar = fetchUserAvatar(web, user->id);
    if (!avatar)
        lt::fail(avatar.error(), std::source_location::current());
    CHECK(avatar->description != nullptr);
}

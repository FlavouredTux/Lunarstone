#include <lunar/avatar/Character.hpp>
#include <lunar/avatar/Rig.hpp>

#include <lunar/PlaceFile.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

namespace lunar::avatar {

namespace {

std::expected<std::vector<InstancePtr>, std::string> loadAssetModel(assets::AssetCache& cache, std::string_view uri)
{
    auto bytes = cache.fetch(uri);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto model = loadModel(**bytes);
    if (!model)
        return std::unexpected(std::format("{}: {}", uri, model.error()));
    return std::move(model->roots);
}

std::expected<std::vector<InstancePtr>, std::string> loadContentModel(assets::AssetCache& cache, std::string_view path)
{
    return loadAssetModel(cache, std::format("rbxasset://{}", path));
}

std::expected<std::vector<InstancePtr>, std::string> loadCatalogAsset(assets::AssetCache& cache, std::int64_t id)
{
    return loadAssetModel(cache, std::format("rbxassetid://{}", id));
}

// The first instance of class T in a loaded file.
template <class T>
std::shared_ptr<T> firstOf(const std::vector<InstancePtr>& roots)
{
    for (const InstancePtr& root : roots) {
        if (auto found = std::dynamic_pointer_cast<T>(root))
            return found;
        for (const InstancePtr& descendant : root->descendants())
            if (auto found = std::dynamic_pointer_cast<T>(descendant))
                return found;
    }
    return nullptr;
}

// The part groups BodyColors and body part assets address, R15 names first.
struct Limb {
    Enum::BodyPart part;
    std::string_view r6;
    std::vector<std::string_view> r15; // not an initializer_list: its array would die with the initializer
};

const std::vector<Limb>& limbs()
{
    static const std::vector<Limb> table{
        {Enum::BodyPart::Head, "Head", {"Head"}},
        {Enum::BodyPart::Torso, "Torso", {"UpperTorso", "LowerTorso"}},
        {Enum::BodyPart::LeftArm, "Left Arm", {"LeftUpperArm", "LeftLowerArm", "LeftHand"}},
        {Enum::BodyPart::RightArm, "Right Arm", {"RightUpperArm", "RightLowerArm", "RightHand"}},
        {Enum::BodyPart::LeftLeg, "Left Leg", {"LeftUpperLeg", "LeftLowerLeg", "LeftFoot"}},
        {Enum::BodyPart::RightLeg, "Right Leg", {"RightUpperLeg", "RightLowerLeg", "RightFoot"}},
    };
    return table;
}

void setNumber(Instance& parent, std::string_view name, double value)
{
    auto* number = instanceCast<NumberValue>(parent.findFirstChild(name));
    if (!number) {
        auto created = std::make_shared<NumberValue>();
        created->setName(std::string(name));
        created->setParent(&parent);
        number = created.get();
    }
    number->setValue(value);
}

// Colours every part of the limb group and records it in BodyColors.
void applyColors(Model& character, const HumanoidDescription& description)
{
    auto* colors = instanceCast<BodyColors>(character.findFirstChildOfClass("BodyColors"));
    if (!colors) {
        auto created = std::make_shared<BodyColors>();
        created->setName("Body Colors");
        created->setParent(&character);
        colors = created.get();
    }
    colors->setHeadColor3(description.headColor());
    colors->setTorsoColor3(description.torsoColor());
    colors->setLeftArmColor3(description.leftArmColor());
    colors->setRightArmColor3(description.rightArmColor());
    colors->setLeftLegColor3(description.leftLegColor());
    colors->setRightLegColor3(description.rightLegColor());
    applyBodyColors(character);
}

// Replaces the character's clothing item of class T with the one in an asset.
template <class T>
void applyClothing(Model& character, std::int64_t assetId, assets::AssetCache& cache,
                   std::vector<std::string>& problems)
{
    if (assetId <= 0)
        return;
    auto roots = loadCatalogAsset(cache, assetId);
    if (!roots) {
        problems.push_back(roots.error());
        return;
    }
    std::shared_ptr<T> item = firstOf<T>(*roots);
    if (!item) {
        problems.push_back(std::format("asset {} has no {}", assetId, T::staticClassInfo().name));
        return;
    }
    while (Instance* old = character.findFirstChildOfClass(T::staticClassInfo().name))
        old->destroy();
    item->setParent(&character);
}

// Gives a body part the mesh, texture, size and rig attachments of the
// part of the same name in a body part asset.
void takePart(const MeshPart& source, MeshPart& target)
{
    target.setMeshId(source.meshId());
    target.setTextureID(source.textureId());
    target.setInitialSize(source.initialSize());
    target.setSize(source.size());
    for (const char* name : {"OriginalSize", "AvatarPartScaleType"})
        if (Instance* old = target.findFirstChild(name))
            old->destroy();
    for (const InstancePtr& item : source.children()) {
        if (auto* attachment = instanceCast<const Attachment>(item.get())) {
            auto* existing = instanceCast<Attachment>(target.findFirstChild(attachment->name()));
            if (existing) {
                existing->setCFrame(attachment->cframe());
                if (Instance* original = existing->findFirstChild("OriginalPosition"))
                    original->destroy();
            } else if (InstancePtr copy = attachment->clone()) {
                copy->setParent(&target);
            }
        } else if (item->name() == "OriginalSize" || item->name() == "AvatarPartScaleType") {
            if (InstancePtr copy = item->clone())
                copy->setParent(&target);
        }
    }
}

// Takes the parts of a body part asset into the character (takePart).
// GUESS: the asset's R15ArtistIntent folder is preferred over R15Fixed, then
// R15. False when the asset has none of them (a dynamic head).
bool applyBodyPart(Model& character, std::int64_t assetId, assets::AssetCache& cache,
                   std::vector<std::string>& problems, bool reportMissingFolder = true)
{
    if (assetId <= 0)
        return true;
    auto roots = loadCatalogAsset(cache, assetId);
    if (!roots) {
        problems.push_back(roots.error());
        return true;
    }
    const Instance* folder = nullptr;
    for (std::string_view name : {"R15ArtistIntent", "R15Fixed", "R15"}) {
        for (const InstancePtr& root : *roots)
            if (!folder)
                folder = root->name() == name ? root.get() : root->findFirstChild(name, true);
        if (folder)
            break;
    }
    if (!folder) {
        if (reportMissingFolder)
            problems.push_back(std::format("body part asset {} has no R15 parts", assetId));
        return false;
    }
    for (const InstancePtr& child : folder->children()) {
        const auto* source = instanceCast<const MeshPart>(child.get());
        auto* target = instanceCast<MeshPart>(character.findFirstChild(child->name()));
        if (source && target)
            takePart(*source, *target);
    }
    return true;
}

// A dynamic head isn't an R15 body part asset: its plain download is a
// classic SpecialMesh head. The client asks for it as "avatar_meshpart_head"
// (0x103687a2f), a MeshPart Head with FaceControls, its attachments and a
// WrapTarget. That part takes the old head's place (and colour); the rig's
// joints are rebuilt from the attachments afterwards, and the old face Decal
// goes with the old head (a dynamic head's face is in its mesh). Its mesh
// needs a login: without one the classic head stays. True when applied.
bool applyDynamicHead(Model& character, std::int64_t assetId, assets::AssetCache& cache,
                      std::vector<std::string>& problems)
{
    auto* old = instanceCast<BasePart>(character.findFirstChild("Head"));
    if (!old)
        return false;
    auto bytes = cache.fetchFormat(static_cast<std::uint64_t>(assetId), "avatar_meshpart_head");
    if (!bytes) {
        problems.push_back(std::format("dynamic head: {}; keeping the classic head", bytes.error()));
        return false;
    }
    auto model = loadModel(**bytes);
    if (!model) {
        problems.push_back(std::format("dynamic head {}: {}", assetId, model.error()));
        return false;
    }
    std::shared_ptr<MeshPart> head = firstOf<MeshPart>(model->roots);
    if (!head) {
        problems.push_back(std::format("dynamic head {} has no MeshPart", assetId));
        return false;
    }
    if (auto mesh = cache.fetch(head->meshId()); !mesh) {
        problems.push_back(std::format("dynamic head {}: {}; keeping the classic head", assetId, mesh.error()));
        return false;
    }
    head->setName("Head");
    head->setColor(old->color());
    head->setCFrame(old->cframe());
    old->destroy();
    head->setParent(&character);
    return true;
}

// Takes an animation pack into the character's Animate object: each of the
// description's animation assets holds an R15Anim folder of StringValues
// named after Animate's sets ("idle", "walk", "mood"...); their Animation
// children replace the set's. GUESS: this is what ApplyDescription does to
// the Animate script's values; sets Animate doesn't have yet (the Rthro
// "pose", "mood") are added as they are.
void applyAnimations(Model& character, const HumanoidDescription& description, assets::AssetCache& cache,
                     std::vector<std::string>& problems)
{
    Instance* animate = character.findFirstChild("Animate");
    if (!animate)
        return;
    for (std::int64_t id : {description.idleAnimation(), description.walkAnimation(), description.runAnimation(),
                            description.jumpAnimation(), description.fallAnimation(), description.climbAnimation(),
                            description.swimAnimation(), description.moodAnimation()}) {
        if (id <= 0)
            continue;
        auto roots = loadCatalogAsset(cache, id);
        if (!roots) {
            problems.push_back(roots.error());
            continue;
        }
        std::vector<std::shared_ptr<StringValue>> sets;
        for (const InstancePtr& root : *roots) {
            if (auto set = std::dynamic_pointer_cast<StringValue>(root))
                sets.push_back(set);
            for (const InstancePtr& descendant : root->descendants())
                if (auto set = std::dynamic_pointer_cast<StringValue>(descendant))
                    sets.push_back(set);
        }
        if (sets.empty())
            problems.push_back(std::format("animation asset {} has no animation sets", id));
        for (const std::shared_ptr<StringValue>& set : sets) {
            if (Instance* existing = animate->findFirstChild(set->name())) {
                const std::vector<InstancePtr> old(existing->children().begin(), existing->children().end());
                for (const InstancePtr& item : old)
                    item->destroy();
                const std::vector<InstancePtr> items(set->children().begin(), set->children().end());
                for (const InstancePtr& item : items)
                    item->setParent(existing);
            } else {
                set->setParent(animate);
            }
        }
    }
}

std::vector<std::int64_t> parseIdList(std::string_view list)
{
    std::vector<std::int64_t> ids;
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        std::string_view item = list.substr(0, comma);
        while (!item.empty() && item.front() == ' ')
            item.remove_prefix(1);
        std::int64_t id = 0;
        if (std::from_chars(item.data(), item.data() + item.size(), id).ec == std::errc{} && id > 0)
            ids.push_back(id);
        if (comma == std::string_view::npos)
            break;
        list.remove_prefix(comma + 1);
    }
    return ids;
}

} // namespace

void applyBodyColors(Model& character)
{
    const auto* colors = instanceCast<const BodyColors>(character.findFirstChildOfClass("BodyColors"));
    if (!colors)
        return;
    for (const Limb& limb : limbs()) {
        Color3 color;
        switch (limb.part) {
        case Enum::BodyPart::Head:
            color = colors->headColor3();
            break;
        case Enum::BodyPart::Torso:
            color = colors->torsoColor3();
            break;
        case Enum::BodyPart::LeftArm:
            color = colors->leftArmColor3();
            break;
        case Enum::BodyPart::RightArm:
            color = colors->rightArmColor3();
            break;
        case Enum::BodyPart::LeftLeg:
            color = colors->leftLegColor3();
            break;
        case Enum::BodyPart::RightLeg:
            color = colors->rightLegColor3();
            break;
        }
        if (auto* part = instanceCast<BasePart>(character.findFirstChild(limb.r6)))
            part->setColor(color);
        for (std::string_view name : limb.r15)
            if (auto* part = instanceCast<BasePart>(character.findFirstChild(name)))
                part->setColor(color);
    }
}

bool attachAccessory(Model& character, Accoutrement& accessory)
{
    auto* handle = instanceCast<BasePart>(accessory.findFirstChild("Handle"));
    if (!handle)
        return false;
    if (Instance* old = handle->findFirstChild("AccessoryWeld"))
        old->destroy();
    // A Handle attachment matching one on the body (0x10405cb68, 0x10405babe).
    for (const InstancePtr& child : handle->children()) {
        const auto* own = instanceCast<const Attachment>(child.get());
        if (!own)
            continue;
        for (const InstancePtr& descendant : character.descendants()) {
            const auto* target = instanceCast<const Attachment>(descendant.get());
            auto* body = target ? instanceCast<BasePart>(target->parent()) : nullptr;
            if (!body || target->name() != own->name() || body->isDescendantOf(&accessory))
                continue;
            auto weld = std::make_shared<Weld>();
            weld->setName("AccessoryWeld");
            weld->setPart0(handle);
            weld->setPart1(body);
            weld->setC0(own->cframe());
            weld->setC1(target->cframe());
            weld->setParent(handle);
            return true;
        }
    }
    // No match: the legacy hat weld to the head. GUESS: C1 at the top of the
    // head, as old hats were made for.
    auto* head = instanceCast<BasePart>(character.findFirstChild("Head"));
    if (!head)
        return false;
    auto weld = std::make_shared<Weld>();
    weld->setName("HeadWeld");
    weld->setPart0(handle);
    weld->setPart1(head);
    weld->setC0(accessory.attachmentPoint());
    weld->setC1(CFrame(0.0f, head->size().y * 0.5f, 0.0f));
    weld->setParent(handle);
    return true;
}

std::vector<std::string> applyDescription(Model& character, const HumanoidDescription& description,
                                          assets::AssetCache& assets)
{
    std::vector<std::string> problems;
    auto* humanoid = instanceCast<Humanoid>(character.findFirstChildOfClass("Humanoid"));
    if (!humanoid)
        return {"the character has no Humanoid"};
    const bool r15 = humanoid->rigType() == Enum::HumanoidRigType::R15;

    bool dynamicHead = false;
    if (r15) {
        if (!applyBodyPart(character, description.head(), assets, problems, false))
            dynamicHead = applyDynamicHead(character, description.head(), assets, problems);
        applyBodyPart(character, description.torso(), assets, problems);
        applyBodyPart(character, description.leftArm(), assets, problems);
        applyBodyPart(character, description.rightArm(), assets, problems);
        applyBodyPart(character, description.leftLeg(), assets, problems);
        applyBodyPart(character, description.rightLeg(), assets, problems);
        // The scale values automatic scaling reads.
        setNumber(*humanoid, "BodyHeightScale", description.heightScale());
        setNumber(*humanoid, "BodyWidthScale", description.widthScale());
        setNumber(*humanoid, "BodyDepthScale", description.depthScale());
        setNumber(*humanoid, "HeadScale", description.headScale());
        setNumber(*humanoid, "BodyTypeScale", description.bodyTypeScale());
        setNumber(*humanoid, "BodyProportionScale", description.proportionScale());
    }
    applyColors(character, description);
    applyClothing<Shirt>(character, description.shirt(), assets, problems);
    applyClothing<Pants>(character, description.pants(), assets, problems);
    applyClothing<ShirtGraphic>(character, description.graphicTShirt(), assets, problems);
    if (description.face() > 0 && !dynamicHead) {
        auto roots = loadCatalogAsset(assets, description.face());
        auto* head = character.findFirstChild("Head");
        if (!roots)
            problems.push_back(roots.error());
        else if (std::shared_ptr<Decal> face = firstOf<Decal>(*roots); face && head) {
            if (Instance* old = head->findFirstChild("face"))
                old->destroy();
            face->setName("face");
            face->setParent(head);
        }
    }

    if (r15) {
        applyAnimations(character, description, assets, problems);
        buildRigFromAttachments(character);
    } else {
        buildR6Joints(character);
    }

    // Accessories go on after scaling so they meet the final attachments.
    std::vector<std::int64_t> accessories;
    for (const std::string* list :
         {&description.hatAccessory(), &description.hairAccessory(), &description.faceAccessory(),
          &description.neckAccessory(), &description.shouldersAccessory(), &description.frontAccessory(),
          &description.backAccessory(), &description.waistAccessory()})
        for (std::int64_t id : parseIdList(*list))
            accessories.push_back(id);
    for (const InstancePtr& child : description.children())
        if (const auto* item = instanceCast<const AccessoryDescription>(child.get()); item && item->assetId() > 0)
            if (std::ranges::find(accessories, item->assetId()) == accessories.end())
                accessories.push_back(item->assetId());
    for (std::int64_t id : accessories) {
        auto roots = loadCatalogAsset(assets, id);
        if (!roots) {
            problems.push_back(roots.error());
            continue;
        }
        std::shared_ptr<Accoutrement> accessory = firstOf<Accoutrement>(*roots);
        if (!accessory) {
            problems.push_back(std::format("asset {} has no Accessory", id));
            continue;
        }
        accessory->setParent(&character);
        if (!attachAccessory(character, *accessory))
            problems.push_back(std::format("accessory {} found nothing to attach to", id));
    }
    return problems;
}

std::expected<std::shared_ptr<Model>, std::string> createCharacter(const CharacterOptions& options)
{
    std::shared_ptr<assets::AssetCache> cache = options.assets;
    if (!cache)
        cache = std::make_shared<assets::AssetCache>();

    // loadCharacterRig (0x104debc36): the dynamic head rig when the
    // Workspace's MeshPartHeadsAndAccessories allows it (it does unless
    // turned off), else characterR15.rbxm; for R6 character.rbxm (the
    // NoFace copy only behind a flag).
    const bool r15 = options.rigType == Enum::HumanoidRigType::R15;
    std::string_view rigPath
        = r15 ? (options.classicHead ? "avatar/characterR15.rbxm" : "avatar/characterR15DynamicHeadV2.rbxm")
              : "avatar/character.rbxm";
    auto rig = loadContentModel(*cache, rigPath);
    if (!rig)
        return std::unexpected(rig.error());
    std::shared_ptr<Model> character = firstOf<Model>(*rig);
    // The dynamic head's mesh isn't a public asset: without a login it can't
    // be downloaded, and a headless character is worse than the classic
    // head, so fall back to the classic R15 rig then.
    if (character && r15 && !options.classicHead) {
        const auto* head = instanceCast<const MeshPart>(character->findFirstChild("Head"));
        if (head && !cache->fetch(head->meshId())) {
            rigPath = "avatar/characterR15.rbxm";
            rig = loadContentModel(*cache, rigPath);
            if (!rig)
                return std::unexpected(rig.error());
            character = firstOf<Model>(*rig);
        }
    }
    if (!character)
        return std::unexpected(std::format("{} has no Model", rigPath));
    character->setParent(nullptr);
    character->setName(options.name);

    auto* humanoid = instanceCast<Humanoid>(character->findFirstChildOfClass("Humanoid"));
    if (!humanoid)
        return std::unexpected(std::format("{} has no Humanoid", rigPath));
    if (!humanoid->findFirstChildOfClass("Animator"))
        std::make_shared<Animator>()->setParent(humanoid);

    // The Animate object: a LocalScript holding the animation sets
    // (avatar/animations/humanoidR15AnimateChildren.rbxm and the R6 one).
    auto animate = std::make_shared<LocalScript>();
    animate->setName("Animate");
    auto children = loadContentModel(*cache, r15 ? "avatar/animations/humanoidR15AnimateChildren.rbxm"
                                                 : "avatar/animations/humanoidR6AnimateChildren.rbxm");
    if (children) {
        for (const InstancePtr& folder : *children) {
            const std::vector<InstancePtr> items(folder->children().begin(), folder->children().end());
            for (const InstancePtr& item : items)
                item->setParent(animate.get());
        }
    }
    animate->setParent(character.get());

    if (r15)
        buildRigFromAttachments(*character);
    else
        buildR6Joints(*character);

    if (options.description) {
        std::vector<std::string> problems = applyDescription(*character, *options.description, *cache);
        if (options.problems)
            options.problems->insert(options.problems->end(), problems.begin(), problems.end());
    }
    // GUESS: the content folder's default shirt and pants are what a
    // character without clothing wears (the client names them next to
    // AvatarEditorService's default clothing lists).
    if (options.defaultClothing) {
        for (auto [path, className] :
             {std::pair{"avatar/defaultShirt.rbxm", "Shirt"}, std::pair{"avatar/defaultPants.rbxm", "Pants"}}) {
            if (character->findFirstChildOfClass(className))
                continue;
            if (auto item = loadContentModel(*cache, path); item && !item->empty())
                item->front()->setParent(character.get());
        }
    }

    BasePart* root = findRootPart(*character);
    if (!root)
        return std::unexpected("the character has no root part");
    // Stand on y = 0: an R15 root part floats HipHeight above the floor; R6
    // legs reach 3 studs below the root's centre.
    CFrame placed = options.rootCFrame.value_or(CFrame());
    if (!options.rootCFrame) {
        const float lift = r15 ? humanoid->hipHeight() + root->size().y * 0.5f : 3.0f;
        placed = CFrame(0.0f, lift, 0.0f);
    }
    root->setCFrame(placed);
    character->setPrimaryPart(root);
    solveJoints(*character, *root);
    return character;
}

std::expected<std::shared_ptr<Model>, std::string> loadCharacter(Player& player, DataModel& game,
                                                                 CharacterOptions options)
{
    options.name = player.name();
    auto character = createCharacter(options);
    if (!character)
        return character;
    if (!options.rootCFrame) {
        // Stand on the first enabled SpawnLocation.
        for (const InstancePtr& descendant : game.workspace()->descendants()) {
            const auto* spawn = instanceCast<const SpawnLocation>(descendant.get());
            if (!spawn || !spawn->enabled())
                continue;
            BasePart* root = findRootPart(**character);
            const Vector3 top = spawn->position() + Vector3{0.0f, spawn->size().y * 0.5f, 0.0f};
            root->setCFrame(root->cframe() + top);
            solveJoints(**character, *root);
            break;
        }
    }
    if (Model* old = player.character(); old && old != character->get())
        old->destroy();
    (*character)->setParent(game.workspace());
    player.setCharacter(character->get());
    return character;
}

} // namespace lunar::avatar

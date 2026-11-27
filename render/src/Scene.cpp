#include <lunar/render/Scene.hpp>

#include <lunar/Classes.hpp>
#include <lunar/render/Material.hpp>
#include <lunar/render/Primitives.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <numbers>
#include <string>
#include <unordered_map>
#include <utility>

namespace lunar::render {

namespace {

Vector3 mul(Vector3 a, Vector3 b)
{
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

// What a SpecialMesh of MeshType Head draws (the client also knows the old
// rbxasset://fonts/head.mesh spelling).
constexpr std::string_view classicHeadMesh = "rbxasset://avatar/heads/head.mesh";

// What a clothed R6 character's torso and limbs draw instead of blocks.
std::string_view r6BodyMesh(std::string_view partName)
{
    if (partName == "Torso")
        return "rbxasset://avatar/meshes/torso.mesh";
    if (partName == "Left Arm")
        return "rbxasset://avatar/meshes/leftarm.mesh";
    if (partName == "Right Arm")
        return "rbxasset://avatar/meshes/rightarm.mesh";
    if (partName == "Left Leg")
        return "rbxasset://avatar/meshes/leftleg.mesh";
    if (partName == "Right Leg")
        return "rbxasset://avatar/meshes/rightleg.mesh";
    return {};
}

float minComponent(Vector3 v)
{
    return std::min({v.x, v.y, v.z});
}

// A provider mesh and the matrix that fits its bounding box into the unit box.
struct Fitted {
    MeshPtr mesh;
    Mat4 toUnit;
};

// A block face as a decal sees it: outward normal, the image's right and up
// directions, and which of the part's size components span it.
struct FaceFrame {
    Vector3 normal;
    Vector3 right;
    Vector3 up;
};

FaceFrame faceFrame(Enum::NormalId face)
{
    switch (face) {
    case Enum::NormalId::Right:
        return {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
    case Enum::NormalId::Top:
        return {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}};
    case Enum::NormalId::Back:
        return {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}};
    case Enum::NormalId::Left:
        return {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}};
    case Enum::NormalId::Bottom:
        return {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}};
    case Enum::NormalId::Front:
        break;
    }
    return {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}};
}

float along(Vector3 size, Vector3 axis)
{
    return std::abs(axis.x) * size.x + std::abs(axis.y) * size.y + std::abs(axis.z) * size.z;
}

// Object-to-part matrix for a decalQuad: x spans right, y spans up, z is the
// unit normal, centred at center.
Mat4 quadMatrix(Vector3 center, Vector3 right, Vector3 up, Vector3 normal)
{
    Mat4 m;
    m.m[0] = {right.x, right.y, right.z, 0.0f};
    m.m[1] = {up.x, up.y, up.z, 0.0f};
    m.m[2] = {normal.x, normal.y, normal.z, 0.0f};
    m.m[3] = {center.x, center.y, center.z, 1.0f};
    return m;
}

TextureAlpha textureAlpha(Enum::AlphaMode mode)
{
    switch (mode) {
    case Enum::AlphaMode::Overlay:
        return TextureAlpha::Overlay;
    case Enum::AlphaMode::Transparency:
        return TextureAlpha::Transparency;
    case Enum::AlphaMode::TintMask:
        return TextureAlpha::TintMask;
    case Enum::AlphaMode::Opaque:
        break;
    }
    return TextureAlpha::Opaque;
}

// Where decals go on each shape; faces a shape doesn't have stay empty.
enum class DecalShape : std::uint8_t { None, Block, Wedge };

class Collector {
public:
    Collector(const MeshProvider& provider, const TextureProvider& textures, std::vector<DrawItem>& out,
              std::vector<const Instance*>* owners)
        : provider_(provider), textureProvider_(textures), out_(out), owners_(owners)
    {
    }

    void collect(const Instance& instance)
    {
        collectPart(instance);
        for (const InstancePtr& child : instance.children())
            collect(*child);
    }

    // The instance alone, if it's a part.
    void collectPart(const Instance& instance)
    {
        // Terrain is voxels, not a box; drawing its bounds would bury the map.
        if (instance.isA("BasePart") && !instance.isA("Terrain")) {
            addPart(static_cast<const BasePart&>(instance));
            if (owners_)
                owners_->resize(out_.size(), &instance);
        }
    }

private:
    // Meshes and textures are looked up once each, but a miss is asked again
    // for every part: whoever wraps the provider can tell which parts wait
    // for what (PartExtractor's users extract those again once it's there).
    const Fitted* fetch(std::string_view meshId)
    {
        if (!provider_ || meshId.empty())
            return nullptr;
        auto it = meshes_.find(meshId);
        if (it == meshes_.end()) {
            Fitted fitted{provider_(meshId), {}};
            if (!fitted.mesh)
                return nullptr;
            if (!fitted.mesh->empty()) {
                const Bounds bounds = meshBounds(fitted.mesh.get());
                Vector3 extent = bounds.extent();
                // Flat meshes keep their zero thickness instead of dividing by it.
                extent = {extent.x > 1e-6f ? extent.x : 1.0f, extent.y > 1e-6f ? extent.y : 1.0f,
                          extent.z > 1e-6f ? extent.z : 1.0f};
                fitted.toUnit = Mat4::scale({1.0f / extent.x, 1.0f / extent.y, 1.0f / extent.z})
                                * Mat4::translation(-bounds.center());
            } else {
                fitted.mesh = nullptr;
            }
            it = meshes_.emplace(std::string(meshId), std::move(fitted)).first;
        }
        return it->second.mesh ? &it->second : nullptr;
    }

    ImagePtr texture(std::string_view id)
    {
        if (!textureProvider_ || id.empty())
            return nullptr;
        auto it = textures_.find(id);
        if (it == textures_.end()) {
            ImagePtr image = textureProvider_(id);
            if (!image)
                return nullptr;
            it = textures_.emplace(std::string(id), std::move(image)).first;
        }
        return it->second;
    }

    // A textured mesh (MeshPart.TextureID, FileMesh.TextureId): the texture
    // covers the part's colour where it's opaque; with any Transparency its
    // alpha is see-through too, as in Roblox. SurfaceAppearance overrides it.
    void applyTexture(const BasePart& part, std::string_view textureId, Color3 tint, DrawItem& item)
    {
        if (const auto* appearance
            = static_cast<const SurfaceAppearance*>(part.findFirstChildWhichIsA("SurfaceAppearance"))) {
            item.texture = texture(appearance->colorMap());
            item.normalMap = texture(appearance->normalMap());
            item.roughnessMap = texture(appearance->roughnessMap());
            item.metalnessMap = texture(appearance->metalnessMap());
            item.textureColor = appearance->color();
            item.alphaMode = textureAlpha(appearance->alphaMode());
            item.physical = true;
            return;
        }
        item.texture = texture(textureId);
        item.textureColor = tint;
        item.alphaMode = part.transparency() > 0.0f ? TextureAlpha::Transparency : TextureAlpha::Overlay;
    }

    // Decal and Texture children, on the part's faces. They show even when
    // the part itself is invisible.
    void addDecals(const BasePart& part, DecalShape shape)
    {
        if (!textureProvider_ || shape == DecalShape::None)
            return;
        const Vector3 size = part.size();
        const Mat4 placed = Mat4::fromCFrame(part.cframe());
        // Blocks and wedges are drawn untextured, so only their own
        // transparency can let what's behind them through.
        const bool solid = part.transparency() <= 0.0f && materialLook(part.material()).minTransparency <= 0.0f;
        for (const InstancePtr& child : part.children()) {
            if (!child->isA("Decal"))
                continue;
            const auto& decal = static_cast<const Decal&>(*child);
            if (decal.transparency() >= 1.0f)
                continue;
            ImagePtr image = texture(decal.texture());
            if (!image)
                continue;

            const FaceFrame frame = faceFrame(decal.face());
            float width = along(size, frame.right);
            float height = along(size, frame.up);
            Vector3 center = frame.normal * (along(size, frame.normal) * 0.5f);
            Vector3 up = frame.up * height;
            Vector3 normal = frame.normal;
            if (shape == DecalShape::Wedge) {
                // The front face is the slope; the sides are triangles a quad can't cover.
                if (decal.face() == Enum::NormalId::Left || decal.face() == Enum::NormalId::Right)
                    continue;
                if (decal.face() == Enum::NormalId::Top)
                    continue;
                if (decal.face() == Enum::NormalId::Front) {
                    up = {0.0f, size.y, size.z};
                    height = up.magnitude();
                    normal = Vector3{0.0f, size.z, -size.y}.unit();
                    center = {};
                }
            }
            // A hair off the surface; the decal pipeline's depth bias does the rest.
            center = center + normal * 0.002f;

            DrawItem item{
                .model = placed * quadMatrix(center, frame.right * width, up, normal),
                .transparency = std::max(decal.transparency(), 0.0f),
                .mesh = decalQuad(),
                .castShadow = false,
                .texture = std::move(image),
                .textureColor = decal.color3(),
                .alphaMode = TextureAlpha::Transparency,
                .clampUv = true,
                .decal = true,
                .zIndex = decal.zIndex(),
                .onSolid = solid,
            };
            if (child->isA("Texture")) {
                // Tiles StudsPerTile apart, shifted by OffsetStuds.
                const auto& tiled = static_cast<const Texture&>(*child);
                const float tileU = std::max(tiled.studsPerTileU(), 1e-3f);
                const float tileV = std::max(tiled.studsPerTileV(), 1e-3f);
                item.uvScale[0] = width / tileU;
                item.uvScale[1] = height / tileV;
                item.uvOffset[0] = -tiled.offsetStudsU() / tileU;
                item.uvOffset[1] = -tiled.offsetStudsV() / tileV;
                item.clampUv = false;
            }
            out_.push_back(std::move(item));
        }
    }

    void addPart(const BasePart& part)
    {
        addDecals(part, decalShape(part));
        if (part.transparency() >= 1.0f)
            return;
        DrawItem item{
            .model = {},
            .color = part.color(),
            .transparency = std::max(part.transparency(), 0.0f),
            .material = part.material(),
            .materialVariant = part.materialVariant(),
            .reflectance = std::clamp(part.reflectance(), 0.0f, 1.0f),
            .mesh = nullptr,
            .castShadow = part.castShadow(),
        };
        const Mat4 placed = Mat4::fromCFrame(part.cframe());
        const Vector3 size = part.size();

        // A union: its geometry asset, coloured by its vertices unless UsePartColor.
        if (const auto* union_ = instanceCast<const PartOperation>(&part); union_ && !union_->assetId().empty()) {
            item.renderFidelity = union_->renderFidelity();
            if (const Fitted* fitted = fetch(union_->assetId())) {
                item.mesh = fitted->mesh;
                item.model = placed * Mat4::scale(size) * fitted->toUnit;
                if (!union_->usePartColor())
                    item.color = Color3{1.0f, 1.0f, 1.0f};
                out_.push_back(std::move(item));
                return;
            }
        }

        if (part.isA("MeshPart")) {
            const auto& meshPart = static_cast<const MeshPart&>(part);
            item.renderFidelity = meshPart.renderFidelity();
            if (const Fitted* fitted = fetch(meshPart.meshId())) {
                item.mesh = fitted->mesh;
                item.model = placed * Mat4::scale(size) * fitted->toUnit;
                applyTexture(part, meshPart.textureId(), {1.0f, 1.0f, 1.0f}, item);
            } else {
                item.model = placed * Mat4::scale(size);
            }
            out_.push_back(std::move(item));
            return;
        }

        if (const auto* special = static_cast<const DataModelMesh*>(part.findFirstChildWhichIsA("DataModelMesh"))) {
            addWithMesh(part, *special, placed, item);
            return;
        }

        if (addR6Body(part, placed, item))
            return;

        Primitive shape = Primitive::Block;
        Vector3 scale = size;
        if (part.isA("WedgePart")) {
            shape = Primitive::Wedge;
        } else if (part.isA("CornerWedgePart")) {
            shape = Primitive::CornerWedge;
        } else if (part.isA("Part")) {
            switch (static_cast<const Part&>(part).shape()) {
            case Enum::PartType::Ball:
                // Roblox fits the ball to the smallest side.
                shape = Primitive::Ball;
                scale = Vector3{1.0f, 1.0f, 1.0f} * minComponent(size);
                break;
            case Enum::PartType::Cylinder: {
                // Along X; round in the smaller of the other two sides.
                shape = Primitive::Cylinder;
                const float diameter = std::min(size.y, size.z);
                scale = {size.x, diameter, diameter};
                break;
            }
            case Enum::PartType::Wedge:
                shape = Primitive::Wedge;
                break;
            case Enum::PartType::CornerWedge:
                shape = Primitive::CornerWedge;
                break;
            case Enum::PartType::Block:
                break;
            }
        }
        if (shape != Primitive::Block)
            item.mesh = primitiveMesh(shape);
        else
            item.surfaces = surfaces(part);
        item.model = placed * Mat4::scale(scale);
        out_.push_back(std::move(item));
    }

    // A plain block torso or limb of an R6 character whose Humanoid has
    // composited clothing: the limb's mesh with the clothing atlas over the
    // part's colour.
    bool addR6Body(const BasePart& part, const Mat4& placed, DrawItem& item)
    {
        if (!part.isA("Part") || static_cast<const Part&>(part).shape() != Enum::PartType::Block)
            return false;
        const std::string_view meshId = r6BodyMesh(part.name());
        const Instance* model = part.parent();
        if (meshId.empty() || !model)
            return false;
        const auto* humanoid = instanceCast<const Humanoid>(model->findFirstChildOfClass("Humanoid"));
        if (!humanoid || humanoid->rigType() != Enum::HumanoidRigType::R6 || humanoid->bodyTexture().empty())
            return false;
        const Fitted* fitted = fetch(meshId);
        if (!fitted)
            return false;
        item.mesh = fitted->mesh;
        item.model = placed * Mat4::scale(part.size()) * fitted->toUnit;
        applyTexture(part, humanoid->bodyTexture(), {1.0f, 1.0f, 1.0f}, item);
        out_.push_back(std::move(item));
        return true;
    }

    static std::uint16_t surfaces(const BasePart& part)
    {
        using enum Enum::NormalId;
        return static_cast<std::uint16_t>(
            surfaceBits(Top, part.topSurface()) | surfaceBits(Bottom, part.bottomSurface())
            | surfaceBits(Front, part.frontSurface()) | surfaceBits(Back, part.backSurface())
            | surfaceBits(Left, part.leftSurface()) | surfaceBits(Right, part.rightSurface()));
    }

    static DecalShape decalShape(const BasePart& part)
    {
        if (part.isA("MeshPart") || part.findFirstChildWhichIsA("DataModelMesh"))
            return DecalShape::None;
        if (part.isA("WedgePart"))
            return DecalShape::Wedge;
        if (part.isA("Part")) {
            switch (static_cast<const Part&>(part).shape()) {
            case Enum::PartType::Block:
                return DecalShape::Block;
            case Enum::PartType::Wedge:
                return DecalShape::Wedge;
            default:
                return DecalShape::None;
            }
        }
        return part.isA("CornerWedgePart") ? DecalShape::None : DecalShape::Block;
    }

    // A head mesh's Decals: the mesh again with the image at its UVs, which
    // the head's UV map lays out so a face texture covers the front.
    void addMeshFaces(const BasePart& part, const DrawItem& surface)
    {
        if (!textureProvider_)
            return;
        // A texture whose alpha is transparency cuts holes in the mesh.
        const bool solid = surface.transparency <= 0.0f && materialLook(surface.material).minTransparency <= 0.0f
                           && (!surface.texture || surface.alphaMode != TextureAlpha::Transparency);
        for (const InstancePtr& child : part.children()) {
            if (!child->isA("Decal") || child->isA("Texture"))
                continue;
            const auto& decal = static_cast<const Decal&>(*child);
            if (decal.transparency() >= 1.0f || decal.face() != Enum::NormalId::Front)
                continue;
            ImagePtr image = texture(decal.texture());
            if (!image)
                continue;
            out_.push_back(DrawItem{
                .model = surface.model,
                .transparency = std::max(decal.transparency(), 0.0f),
                .mesh = surface.mesh,
                .castShadow = false,
                .texture = std::move(image),
                .textureColor = decal.color3(),
                .alphaMode = TextureAlpha::Transparency,
                .clampUv = true,
                .decal = true,
                .zIndex = decal.zIndex(),
                .onSolid = solid,
            });
        }
    }

    // BlockMesh, CylinderMesh, SpecialMesh and FileMesh replace the part's
    // shape; Scale and Offset apply on top.
    void addWithMesh(const BasePart& part, const DataModelMesh& special, const Mat4& placed, DrawItem& item)
    {
        const Vector3 size = part.size();
        const Mat4 offset = placed * Mat4::translation(special.offset());
        const Vector3 stretched = mul(size, special.scale());
        std::optional<Primitive> shape = Primitive::Block;
        Vector3 scale = stretched;

        if (special.isA("CylinderMesh")) {
            shape = Primitive::CylinderY;
        } else if (special.isA("FileMesh")) {
            const bool isSpecial = special.isA("SpecialMesh");
            const Enum::MeshType type
                = isSpecial ? static_cast<const SpecialMesh&>(special).meshType() : Enum::MeshType::FileMesh;
            switch (type) {
            case Enum::MeshType::Head:
                // A classic head: the content folder's head mesh, uniform on the
                // part's smallest side; a cylinder until the mesh arrives.
                scale = mul(Vector3{1.0f, 1.0f, 1.0f} * minComponent(size), special.scale());
                if (const Fitted* head = fetch(classicHeadMesh)) {
                    item.mesh = head->mesh;
                    item.model = offset * Mat4::scale(scale) * head->toUnit;
                    const DrawItem face = item;
                    out_.push_back(std::move(item));
                    addMeshFaces(part, face);
                    return;
                }
                shape = Primitive::CylinderY;
                break;
            case Enum::MeshType::Wedge:
                shape = Primitive::Wedge;
                break;
            case Enum::MeshType::CornerWedge:
                shape = Primitive::CornerWedge;
                break;
            case Enum::MeshType::Sphere:
                shape = Primitive::Ball;
                break;
            case Enum::MeshType::Cylinder:
                shape = Primitive::CylinderY;
                break;
            case Enum::MeshType::FileMesh:
                shape = std::nullopt;
                break;
            default:
                break;
            }
            if (!shape) {
                // A file draws at its own size times Scale, whatever the part's Size.
                const auto& file = static_cast<const FileMesh&>(special);
                if (const Fitted* fitted = fetch(file.meshId())) {
                    item.mesh = fitted->mesh;
                    item.model = offset * Mat4::scale(special.scale());
                    const Vector3 tint = special.vertexColor();
                    applyTexture(part, file.textureId(), {tint.x, tint.y, tint.z}, item);
                } else {
                    item.model = placed * Mat4::scale(size);
                }
                out_.push_back(std::move(item));
                return;
            }
        }
        if (*shape != Primitive::Block)
            item.mesh = primitiveMesh(*shape);
        item.model = offset * Mat4::scale(scale);
        out_.push_back(std::move(item));
    }

    const MeshProvider& provider_;
    const TextureProvider& textureProvider_;
    std::vector<DrawItem>& out_;
    std::vector<const Instance*>* owners_;
    std::map<std::string, Fitted, std::less<>> meshes_;
    std::map<std::string, ImagePtr, std::less<>> textures_;
};

} // namespace

std::vector<DrawItem> extractScene(const Instance& root, const MeshProvider& meshProvider,
                                   const TextureProvider& textureProvider, std::vector<const Instance*>* owners)
{
    std::vector<DrawItem> items;
    if (owners)
        owners->clear();
    Collector(meshProvider, textureProvider, items, owners).collect(root);
    return items;
}

struct PartExtractor::State {
    State(MeshProvider meshes_, TextureProvider textures_) : meshes(std::move(meshes_)), textures(std::move(textures_))
    {
    }

    MeshProvider meshes;
    TextureProvider textures;
    std::vector<DrawItem> items;
    Collector collector{meshes, textures, items, nullptr};
};

PartExtractor::PartExtractor(MeshProvider meshProvider, TextureProvider textureProvider)
    : state_(std::make_unique<State>(std::move(meshProvider), std::move(textureProvider)))
{
}

PartExtractor::~PartExtractor() = default;

std::size_t PartExtractor::add(const BasePart& part, std::vector<DrawItem>& out)
{
    state_->items.clear();
    state_->collector.collectPart(part);
    out.insert(out.end(), std::make_move_iterator(state_->items.begin()), std::make_move_iterator(state_->items.end()));
    return state_->items.size();
}

const MeshPtr& decalQuad()
{
    static const MeshPtr quad = [] {
        auto mesh = std::make_shared<MeshData>();
        const Vector3 normal{0.0f, 0.0f, 1.0f};
        mesh->vertices = {
            {{-0.5f, -0.5f, 0.0f}, normal, 0.0f, 1.0f},
            {{0.5f, -0.5f, 0.0f}, normal, 1.0f, 1.0f},
            {{0.5f, 0.5f, 0.0f}, normal, 1.0f, 0.0f},
            {{-0.5f, 0.5f, 0.0f}, normal, 0.0f, 0.0f},
        };
        mesh->indices = {0, 1, 2, 0, 2, 3};
        return MeshPtr(std::move(mesh));
    }();
    return quad;
}

DrawItem worldMesh(MeshPtr mesh, Enum::Material material, Color3 color)
{
    return {.model = {},
            .color = color,
            .transparency = 0.0f,
            .material = material,
            .reflectance = 0.0f,
            .mesh = std::move(mesh)};
}

Bounds meshBounds(const MeshData* mesh)
{
    if (!mesh || mesh->vertices.empty())
        return {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    Bounds bounds{mesh->vertices.front().position, mesh->vertices.front().position};
    auto add = [&](Vector3 p) {
        bounds.min = {std::min(bounds.min.x, p.x), std::min(bounds.min.y, p.y), std::min(bounds.min.z, p.z)};
        bounds.max = {std::max(bounds.max.x, p.x), std::max(bounds.max.y, p.y), std::max(bounds.max.z, p.z)};
    };
    // Coarser levels of detail may use vertices of their own, a little off
    // the full-detail shape the part is fitted to: only that one counts.
    if (mesh->lodEnds.empty() || mesh->indices.empty()) {
        for (const MeshVertex& v : mesh->vertices)
            add(v.position);
    } else {
        bool first = true;
        for (std::uint32_t index : mesh->indices) {
            if (index >= mesh->vertices.size())
                continue;
            const Vector3 p = mesh->vertices[index].position;
            if (first)
                bounds = {p, p};
            first = false;
            add(p);
        }
    }
    return bounds;
}

std::optional<Bounds> sceneBounds(std::span<const DrawItem> items)
{
    if (items.empty())
        return std::nullopt;

    std::unordered_map<const MeshData*, Bounds> cache;
    std::optional<Bounds> bounds;
    for (const DrawItem& item : items) {
        auto it = cache.find(item.mesh.get());
        if (it == cache.end())
            it = cache.emplace(item.mesh.get(), meshBounds(item.mesh.get())).first;
        const Bounds& local = it->second;
        for (int corner = 0; corner < 8; ++corner) {
            const Vector3 p = item.model.transformPoint({(corner & 1) ? local.max.x : local.min.x,
                                                         (corner & 2) ? local.max.y : local.min.y,
                                                         (corner & 4) ? local.max.z : local.min.z});
            if (!bounds)
                bounds = Bounds{p, p};
            bounds->min = {std::min(bounds->min.x, p.x), std::min(bounds->min.y, p.y), std::min(bounds->min.z, p.z)};
            bounds->max = {std::max(bounds->max.x, p.x), std::max(bounds->max.y, p.y), std::max(bounds->max.z, p.z)};
        }
    }
    return bounds;
}

Camera framingCamera(std::span<const DrawItem> items, float aspect)
{
    Camera camera;
    const auto bounds = sceneBounds(items);
    if (!bounds) {
        camera.cframe = Camera::lookAt({0.0f, 10.0f, 20.0f}, {});
        return camera;
    }
    const float radius = std::max(bounds->extent().magnitude() / 2.0f, 1.0f);
    const float fovY = camera.fieldOfView * std::numbers::pi_v<float> / 180.0f;
    const float fov = aspect < 1.0f ? 2.0f * std::atan(std::tan(fovY / 2.0f) * aspect) : fovY;
    const float distance = radius / std::sin(fov / 2.0f);
    const Vector3 direction = Vector3{1.0f, 0.8f, 1.0f}.unit();
    camera.cframe = Camera::lookAt(bounds->center() + direction * distance, bounds->center());
    camera.farPlane = std::max(camera.farPlane, distance + 2.0f * radius);
    return camera;
}

} // namespace lunar::render

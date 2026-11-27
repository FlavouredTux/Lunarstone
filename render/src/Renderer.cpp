#include <lunar/render/Renderer.hpp>

#include "EffectPass.hpp"
#include "HighlightPass.hpp"
#include "PostPass.hpp"
#include "RayScene.hpp"
#include "Shaders.hpp"
#include "Textures.hpp"

#include <lunar/render/Context.hpp>
#include <lunar/render/Frustum.hpp>
#include <lunar/render/LightGrid.hpp>
#include <lunar/render/Lod.hpp>
#include <lunar/render/Material.hpp>
#include <lunar/render/Primitives.hpp>
#include <lunar/render/RayTracing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lunar::render {

// Matches frame.glsl (std140: every member here is 16-byte aligned).
struct FrameUniforms {
    struct TerrainLayer { // frame.glsl's TerrainLayer
        float color[4]{};
        std::uint32_t maps[4]{};
        float tiling[4]{};
        float blend[4]{};
    };

    Mat4 viewProjection;
    float cameraPosition[4]{};
    float sunDirection[4]{};
    float sunColor[4]{};
    float ambient[4]{}; // w: exposure
    float skyAmbient[4]{};
    float sky[4]{};
    float fogColor[4]{};
    float fogRange[4]{};
    Mat4 shadowViewProjection;
    float shadow[4]{};                  // enabled, texel size in uv, normal offset in studs
    std::uint32_t surfaceTextures[4]{}; // Studs, Inlet
    float skyForward[4]{};
    float skyRight[4]{};
    float skyUp[4]{};
    float skyRotation[3][4]{};
    std::uint32_t skyFaces0[4]{}; // Rt, Lf, Up, Dn
    std::uint32_t skyFaces1[4]{}; // Bk, Ft, sun, moon
    float skyTint[4]{};
    float sunSprite[4]{};
    float moonSprite[4]{};
    float skyParams[4]{};
    float skyAverage[6][4]{};
    float atmosphereColor[4]{};
    float atmosphereDecay[4]{};
    float atmosphereParams[4]{};
    float backColor[4]{};
    float skyGradientTop[4]{};    // w: EnvironmentDiffuseScale
    float skyGradientBottom[4]{}; // w: EnvironmentSpecularScale
    float ambientCube[6][4]{};
    float colorCorrection[3][4]{};
    float depthParams[4]{};         // near, far, seconds of game time
    float clouds[4]{};              // Cover, Density, 1 when shown
    float cloudColor[4]{};          // linear
    TerrainLayer terrainLayers[23]; // Terrain::materials
    float lightGridMin[4]{};        // studs; w: 1 when the grid is on
    float lightGridMax[4]{};
    float lightGridScale[4]{}; // world x, z, y to texture coordinates
    float quality[4]{};        // x: 1 for the sharp skybox in reflections (GraphicsSettings), y: ray traced sun,
                               // z: ray traced reflections, w: everything reflects
    float localLights[4]{};    // count, 1 lit per pixel, localLightDist, hybridLerpSlope
    float lightClusters[4]{};  // tiles across, down, pixels per tile; depth slices
    float lightMap[4]{};       // on, cap on ambient plus local light, ray traced shadows
    float occlusion[4]{};      // ray traced ambient occlusion on, its reach in studs
    float tone[4]{};           // 1 for the Retro tone curve; 1 for Potato
    float decals[4]{};         // 1 with the decal layer; its decals' specular, shininess
};

namespace {

constexpr std::size_t terrainMaterialCount = 23; // Terrain::materials

std::uint64_t mixHash(std::uint64_t h, std::uint64_t v)
{
    return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

// Per-pixel local lights reach out as far from the camera as the sun's
// shadows (the client's localLightDist is its sunDist, which is the shadow
// distance), then fade into the light map's over this many studs more (its
// hybridLerpSlope is one over the quality level's blend, 20 at the top
// levels). [code: 0x10498cb38, 0x10486bef1]
constexpr float localLightBlend = 20.0f;
// The most lights a frame lights per pixel; the nearest are kept (the
// client's buffer takes 819).
constexpr std::size_t maxPixelLights = 4096;
// What the grid shaders cap ambient plus local light at (LightConfig0.w).
// [code: 0x1048645e8]
constexpr float localLightCap = 100.0f;
// How far --ambient-occlusion's rays look for what's around: a room's
// corners and what stands on its floor, not the far wall.
constexpr float ambientOcclusionStuds = 8.0f;

struct Vertex {
    float position[3];
    float normal[3];
    // Terrain has no UVs: its meshes keep the eight material weights here
    // instead (read through attributes 10 and 11), which keeps the biggest
    // meshes there are at 40 bytes a vertex.
    float uv[2];
    std::uint8_t color[4];
    std::int8_t tangent[4]; // w: which way the bitangent goes
};
static_assert(sizeof(Vertex) == 40);
static_assert(sizeof(Vertex::uv) == sizeof(MeshVertex::materialWeights));

// Flags in Instance::extra[2]; the low two bits are the TextureAlpha.
constexpr std::uint32_t flagClamp = 4;
constexpr std::uint32_t flagPhysical = 8;
constexpr std::uint32_t flagDecal = 16;
constexpr std::uint32_t flagTintMask = 32; // the material colour map's alpha masks the part colour
constexpr std::uint32_t flagTerrain = 64;  // maps.xy hold the terrain palette
constexpr std::uint32_t flagWater = 128;   // terrain water

// What the vertex shader reads per instance; kept small since instance
// attributes are fetched per vertex.
struct Instance {
    Mat4 model;
    float color[4]{}; // linear RGB, alpha
};
static_assert(sizeof(Instance) == 80);

// The rest, for the fragment shader: one per instance in the shade buffer,
// matching frame.glsl's Shade (std430).
struct Shade {
    float surface[4]{};                        // specular, shininess, emissive, reflectance
    float pattern[4]{};                        // kind, features per stud, strength, studs per material texture tile
    float tint[4]{};                           // linear texture tint
    std::uint32_t maps[4]{};                   // texture indices: colour, normal, roughness, metalness
    std::uint32_t extra[4]{};                  // material colour and normal textures, flags, surfaces
    float uvTransform[4]{};                    // scale u, v, offset u, v
    float rotation[4]{0.0f, 0.0f, 0.0f, 1.0f}; // object to world quaternion, for material normal maps
};
static_assert(sizeof(Shade) == 112);

// The rotation part of a rotation-times-scale model matrix, as a quaternion (x, y, z, w).
std::array<float, 4> rotationOf(const Mat4& model)
{
    float r[3][3]; // r[row][column]
    for (std::size_t c = 0; c < 3; ++c) {
        const auto& column = model.m[c];
        const float length
            = std::max(std::sqrt(column[0] * column[0] + column[1] * column[1] + column[2] * column[2]), 1e-12f);
        for (std::size_t row = 0; row < 3; ++row)
            r[row][c] = column[row] / length;
    }
    std::array<float, 4> q{};
    const float trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0.0f) {
        const float t = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(r[2][1] - r[1][2]) / t, (r[0][2] - r[2][0]) / t, (r[1][0] - r[0][1]) / t, 0.25f * t};
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float t = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        q = {0.25f * t, (r[0][1] + r[1][0]) / t, (r[0][2] + r[2][0]) / t, (r[2][1] - r[1][2]) / t};
    } else if (r[1][1] > r[2][2]) {
        const float t = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        q = {(r[0][1] + r[1][0]) / t, 0.25f * t, (r[1][2] + r[2][1]) / t, (r[0][2] - r[2][0]) / t};
    } else {
        const float t = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        q = {(r[0][2] + r[2][0]) / t, (r[1][2] + r[2][1]) / t, 0.25f * t, (r[1][0] - r[0][1]) / t};
    }
    return q;
}

struct ShadowInstance {
    Mat4 model;
    float uvTransform[4]{};
    std::uint32_t texture = 0; // cut-out casters' colour map
    std::uint32_t padding[3]{};
};
static_assert(sizeof(ShadowInstance) == 96);

float toLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// toLinear from a table, close enough for what rays see and far cheaper
// over every part in the place each frame.
float toLinearApprox(float c)
{
    constexpr int steps = 1024;
    static const auto table = [] {
        std::array<float, steps + 1> t{};
        for (int i = 0; i <= steps; ++i)
            t[static_cast<std::size_t>(i)] = toLinear(static_cast<float>(i) / steps);
        return t;
    }();
    const float x = std::clamp(c, 0.0f, 1.0f) * steps;
    const auto i = std::min(static_cast<int>(x), steps - 1);
    const float f = x - static_cast<float>(i);
    return table[static_cast<std::size_t>(i)] * (1.0f - f) + table[static_cast<std::size_t>(i) + 1] * f;
}

void putLinear(float (&out)[4], Color3 srgb, float w = 0.0f)
{
    out[0] = toLinear(srgb.r);
    out[1] = toLinear(srgb.g);
    out[2] = toLinear(srgb.b);
    out[3] = w;
}

std::uint8_t unorm8(float v)
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

std::int8_t snorm8(float v)
{
    return static_cast<std::int8_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f));
}

// Per-vertex tangents along +u, with the bitangent sign that makes
// cross(normal, tangent) * sign point up the image (towards -v, since Roblox
// UVs run down). Vertices without usable UVs get any perpendicular. Vertices
// only coarser levels of detail use take their tangents from those levels.
std::vector<std::array<float, 4>> tangents(const MeshData& mesh)
{
    std::vector<Vector3> tangent(mesh.vertices.size());
    std::vector<Vector3> bitangent(mesh.vertices.size());
    std::vector<std::uint8_t> full(mesh.lodIndices.empty() ? 0 : mesh.vertices.size());
    auto accumulate = [&](std::span<const std::uint32_t> indices, bool coarse) {
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            const std::uint32_t a = indices[i];
            const std::uint32_t b = indices[i + 1];
            const std::uint32_t c = indices[i + 2];
            if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size())
                continue;
            const MeshVertex& va = mesh.vertices[a];
            const MeshVertex& vb = mesh.vertices[b];
            const MeshVertex& vc = mesh.vertices[c];
            const Vector3 e1 = vb.position - va.position;
            const Vector3 e2 = vc.position - va.position;
            const float u1 = vb.u - va.u;
            const float u2 = vc.u - va.u;
            const float v1 = va.v - vb.v; // up the image
            const float v2 = va.v - vc.v;
            const float det = u1 * v2 - u2 * v1;
            if (std::abs(det) < 1e-12f)
                continue;
            const float r = 1.0f / det;
            const Vector3 t = (e1 * v2 - e2 * v1) * r;
            const Vector3 bt = (e2 * u1 - e1 * u2) * r;
            for (std::uint32_t k : {a, b, c}) {
                if (coarse && full[k] != 0)
                    continue;
                tangent[k] = tangent[k] + t;
                bitangent[k] = bitangent[k] + bt;
            }
        }
    };
    accumulate(mesh.indices, false);
    if (!full.empty()) {
        for (std::uint32_t index : mesh.indices)
            if (index < full.size())
                full[index] = 1;
        accumulate(mesh.lodIndices, true);
    }
    std::vector<std::array<float, 4>> out(mesh.vertices.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        const Vector3 n = mesh.vertices[i].normal;
        Vector3 t = tangent[i] - n * n.dot(tangent[i]);
        if (t.dot(t) < 1e-12f) {
            const Vector3 axis = std::abs(n.x) < 0.9f ? Vector3{1.0f, 0.0f, 0.0f} : Vector3{0.0f, 1.0f, 0.0f};
            t = axis - n * n.dot(axis);
        }
        t = t.dot(t) > 1e-12f ? t.unit() : Vector3{1.0f, 0.0f, 0.0f};
        const float sign = n.cross(t).dot(bitangent[i]) < 0.0f ? -1.0f : 1.0f;
        out[i] = {t.x, t.y, t.z, sign};
    }
    return out;
}

UniqueShaderModule makeShader(VkDevice device, std::span<const std::uint32_t> code)
{
    VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size_bytes(),
        .pCode = code.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return {device, module};
}

void imageBarrier(VkCommandBuffer cmd, const VkImageMemoryBarrier2& barrier)
{
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

float maxScale(const Mat4& m)
{
    auto length2 = [&](std::size_t c) { return m.m[c][0] * m.m[c][0] + m.m[c][1] * m.m[c][1] + m.m[c][2] * m.m[c][2]; };
    return std::sqrt(std::max({length2(0), length2(1), length2(2)}));
}

// How far (squared) a point can be from `center`, an object-space box of
// half size `half` moved by m, and still be inside it, with room for float
// error; infinity unless m's axes are square to each other, where a box's
// corners say nothing about the point's coordinates along them.
float boxReach2(const Mat4& m, Vector3 half, Vector3 center)
{
    const Vector3 axes[3]
        = {{m.m[0][0], m.m[0][1], m.m[0][2]}, {m.m[1][0], m.m[1][1], m.m[1][2]}, {m.m[2][0], m.m[2][1], m.m[2][2]}};
    const float lengths2[3] = {axes[0].dot(axes[0]), axes[1].dot(axes[1]), axes[2].dot(axes[2])};
    for (std::size_t a = 0; a < 3; ++a) {
        const std::size_t b = (a + 1) % 3;
        const float dot = axes[a].dot(axes[b]);
        if (dot * dot > 1e-10f * lengths2[a] * lengths2[b])
            return std::numeric_limits<float>::infinity();
    }
    const float reach
        = std::sqrt(lengths2[0] * half.x * half.x + lengths2[1] * half.y * half.y + lengths2[2] * half.z * half.z);
    const float slack = reach * 1e-3f + 0.05f + 1e-5f * center.magnitude();
    return (reach + slack) * (reach + slack);
}

// Half the size of the world-space box around an object-space box of half
// size `half` that m moves.
Vector3 boxHalfExtent(const Mat4& m, Vector3 half)
{
    auto row = [&](std::size_t r) {
        return std::abs(m.m[0][r]) * half.x + std::abs(m.m[1][r]) * half.y + std::abs(m.m[2][r]) * half.z;
    };
    return {row(0), row(1), row(2)};
}

// The 3D image's size against the target's: Potato's is half the render scale.
float sceneScale(const GraphicsSettings& graphics)
{
    return graphics.renderScale * (graphics.potato ? 0.5f : 1.0f);
}

} // namespace

std::expected<std::unique_ptr<MeshRenderer>, std::string> MeshRenderer::create(Context& context, VkFormat colorFormat,
                                                                               std::uint32_t framesInFlight,
                                                                               bool rayQueries)
{
    std::unique_ptr<MeshRenderer> renderer(new MeshRenderer(context));
    renderer->colorFormat_ = colorFormat;
    renderer->slots_.resize(std::max(framesInFlight, 1u));
    try {
        renderer->textures_
            = std::make_unique<TextureCache>(context, static_cast<std::uint32_t>(renderer->slots_.size()));
        if (context.rayTracing() && rayQueries)
            renderer->rays_ = std::make_unique<RayScene>(context, renderer->slots_.size());
        renderer->createDescriptors();
        renderer->createPipelines();
        if (renderer->rays_)
            renderer->createDenoiser();
        VkFormatProperties depthProperties{};
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice(), depthFormat, &depthProperties);
        constexpr VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        renderer->depthBlit_ = (depthProperties.optimalTilingFeatures & blit) == blit;
    } catch (const VulkanError& e) {
        return std::unexpected(e.what());
    }
    return renderer;
}

MeshRenderer::MeshRenderer(Context& context) : context_(&context) {}

MeshRenderer::~MeshRenderer() = default;

std::size_t MeshRenderer::textureCount() const
{
    return textures_->resident();
}

bool MeshRenderer::setPassTimestamps(bool on)
{
    if (!on) {
        passTimestamps_ = {};
        passMilliseconds_.fill(0.0);
        return false;
    }
    if (!context_->timestampQueries())
        return false;
    if (!passTimestamps_) {
        // What asking for it costs, every frame: one vkCmdWriteTimestamp2 at
        // each of the 19 pass boundaries (the frame's start and one after
        // each pass) and one vkCmdResetQueryPool of 20 queries, each a 64-bit
        // write on the GPU's timestamp path, and nothing when it is off.
        const VkQueryPoolCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = passQueryCount,
        };
        VkQueryPool pool = VK_NULL_HANDLE;
        vkCheck(vkCreateQueryPool(context_->device(), &info, nullptr, &pool), "vkCreateQueryPool");
        passTimestamps_ = UniqueQueryPool(context_->device(), pool);
    }
    return true;
}

void MeshRenderer::readPassTimestamps()
{
    if (!passTimestamps_) {
        passMilliseconds_.fill(0.0);
        return;
    }
    std::array<std::uint64_t, passQueryCount> ticks{};
    const VkResult ready
        = vkGetQueryPoolResults(context_->device(), passTimestamps_.get(), 0, passQueryCount, sizeof(ticks),
                                ticks.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (ready != VK_SUCCESS) {
        passMilliseconds_.fill(0.0);
        return;
    }
    // A pass takes the gap from the stamp before it to its own. Ticks wrap
    // after timestampValidBits of them, far longer than a frame takes.
    const double nanoseconds = context_->timestampNanoseconds();
    for (std::size_t i = 0; i < gpuPassCount; ++i)
        passMilliseconds_[i] = static_cast<double>(ticks[i + 1] - ticks[i]) * nanoseconds / 1e6;
}

void MeshRenderer::endPassTimestamps(VkCommandBuffer cmd)
{
    if (!passTimestamps_)
        return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, passTimestamps_.get(),
                         static_cast<std::uint32_t>(GpuPass::Readback) + 1);
}

// One stamp per pass, at a fixed point in record()'s control flow whether or
// not the pass had anything to do, so the indices stay aligned with GpuPass
// and a pass that drew nothing leaves a gap of about nothing.
void MeshRenderer::markPass(VkCommandBuffer cmd, GpuPass pass)
{
    if (!passTimestamps_)
        return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, passTimestamps_.get(),
                         static_cast<std::uint32_t>(pass) + 1);
}

void MeshRenderer::createDenoiser()
{
    VkDevice device = context_->device();
    denoise_ = (rayTracingSkipMask() & static_cast<std::uint32_t>(RayTracingSkip::Denoise)) == 0;

    // What the shared ray changes, that times what it found, the surface,
    // the visibility a pass reads (the composite too) and the one it writes.
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (std::uint32_t i = 0; i < 4; ++i)
        bindings[i] = {
            .binding = i,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    bindings[4] = {
        .binding = 4,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo setInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    vkCheck(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
    denoiseSetLayout_ = UniqueDescriptorSetLayout(device, setLayout);

    const VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &setLayout,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCheck(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    denoiseLayout_ = UniquePipelineLayout(device, layout);

    // Two sets a slot: blurring into one visibility image and into the other.
    const auto sets = static_cast<std::uint32_t>(slots_.size() * 2);
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets},
    };
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = sets,
        .poolSizeCount = 2,
        .pPoolSizes = sizes,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCheck(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    denoisePool_ = UniqueDescriptorPool(device, pool);
    for (Slot& slot : slots_) {
        const VkDescriptorSetLayout layouts[2] = {setLayout, setLayout};
        const VkDescriptorSetAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool,
            .descriptorSetCount = 2,
            .pSetLayouts = layouts,
        };
        vkCheck(vkAllocateDescriptorSets(device, &allocInfo, slot.denoiseSets), "vkAllocateDescriptorSets");
    }

    // A pipeline per blur pass, its step and whether it's the first (which
    // reads the rays) specialized in (denoise.comp).
    UniqueShaderModule blurShader = makeShader(device, shaders::denoiseComp());
    VkPipeline pipeline = VK_NULL_HANDLE;
    for (std::size_t pass = 0; pass < denoiseBlur_.size(); ++pass) {
        const struct {
            std::int32_t step;
            VkBool32 first;
        } blurConstants{std::int32_t{1} << pass, pass == 0 ? VK_TRUE : VK_FALSE};
        const VkSpecializationMapEntry blurEntries[2]
            = {{0, 0, sizeof(std::int32_t)}, {1, sizeof(std::int32_t), sizeof(VkBool32)}};
        const VkSpecializationInfo blurSpecialization{2, blurEntries, sizeof blurConstants, &blurConstants};
        const VkComputePipelineCreateInfo blurInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = blurShader.get(),
                      .pName = "main",
                      .pSpecializationInfo = &blurSpecialization},
            .layout = layout,
        };
        vkCheck(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &blurInfo, nullptr, &pipeline),
                "vkCreateComputePipelines");
        denoiseBlur_[pass] = UniquePipeline(device, pipeline);
    }

    // The composite: one triangle over the scene, adding to its colour what
    // the denoised rays add, and another taking away what they take away (a
    // normalized target clamps what's drawn before blending), alpha left
    // alone. It draws in the scope that goes on after the opaque pass's,
    // depth and all, without testing it.
    UniqueShaderModule vert = makeShader(device, shaders::fullscreenVert());
    UniqueShaderModule compositeFrag = makeShader(device, shaders::denoiseCompositeFrag());
    VkBool32 takeAway = VK_FALSE;
    const VkSpecializationMapEntry takeAwayEntry{0, 0, sizeof(VkBool32)};
    const VkSpecializationInfo specialization{1, &takeAwayEntry, sizeof takeAway, &takeAway};
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert.get(),
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = compositeFrag.get(),
         .pName = "main",
         .pSpecializationInfo = &specialization},
    };
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    VkPipelineColorBlendAttachmentState add{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask
        = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &add,
    };
    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates,
    };
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &colorFormat_,
        .depthAttachmentFormat = depthFormat,
    };
    const VkGraphicsPipelineCreateInfo compositeInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = layout,
    };
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &compositeInfo, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    denoiseComposite_[0] = UniquePipeline(device, pipeline);
    // The scene less what's drawn: alpha still the scene's (less nothing).
    takeAway = VK_TRUE;
    add.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
    add.alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &compositeInfo, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    denoiseComposite_[1] = UniquePipeline(device, pipeline);
}

void MeshRenderer::createDescriptors()
{
    VkDevice device = context_->device();
    const VkSampler textureSamplers[2] = {textures_->repeatSampler(), textures_->clampSampler()};
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = textures_->capacity(),
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = 2,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = textureSamplers,
        },
        {
            .binding = 4,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 5,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 6,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        // Local lights (lights.glsl): the light map, the lights and their clusters.
        {
            .binding = 9,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 10,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 11,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        // Shadowed local lights' cube map atlas and its faces' matrices.
        {
            .binding = 12,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 13,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        // The decal layer, which the opaque pass reads.
        {
            .binding = 14,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };
    // Only the textures in use need to be valid.
    std::vector<VkDescriptorBindingFlags> bindingFlags
        = {0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    // Ray queries: the top level and what each of its instances is.
    if (rays_) {
        bindings.push_back({
            .binding = 7,
            .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        });
        bindings.push_back({
            .binding = 8,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        });
        bindingFlags.push_back(0);
        bindingFlags.push_back(0);
    }
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(bindingFlags.size()),
        .pBindingFlags = bindingFlags.data(),
    };
    const VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flagsInfo,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    vkCheck(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
    setLayout_ = UniqueDescriptorSetLayout(device, setLayout);

    const auto count = static_cast<std::uint32_t>(slots_.size());
    std::vector<VkDescriptorPoolSize> poolSizes = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count * 6},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count * textures_->capacity()},
        {VK_DESCRIPTOR_TYPE_SAMPLER, count * 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count * (rays_ ? 5 : 4)},
    };
    if (rays_)
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, count});
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = count,
        .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data(),
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCheck(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    descriptorPool_ = UniqueDescriptorPool(device, pool);

    // Hardware depth comparison with bilinear filtering; outside the map is lit.
    const VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .compareEnable = VK_TRUE,
        .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };
    VkSampler sampler = VK_NULL_HANDLE;
    vkCheck(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "vkCreateSampler");
    shadowSampler_ = UniqueSampler(device, sampler);

    const VkSamplerCreateInfo depthSamplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    VkSampler plainSampler = VK_NULL_HANDLE;
    vkCheck(vkCreateSampler(device, &depthSamplerInfo, nullptr, &plainSampler), "vkCreateSampler");
    depthSampler_ = UniqueSampler(device, plainSampler);

    // Trilinear, wrapping around like the grid does.
    const VkSamplerCreateInfo gridSamplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    };
    VkSampler gridSampler = VK_NULL_HANDLE;
    vkCheck(vkCreateSampler(device, &gridSamplerInfo, nullptr, &gridSampler), "vkCreateSampler");
    lightGridSampler_ = UniqueSampler(device, gridSampler);
    lightGrid_ = std::make_unique<LightGrid>();
    const GridPoint gridVoxels = lightGrid_->voxels();
    lightGridImage_ = Image::volume(context_->allocator(),
                                    {static_cast<std::uint32_t>(gridVoxels.x), static_cast<std::uint32_t>(gridVoxels.z),
                                     static_cast<std::uint32_t>(gridVoxels.y)},
                                    VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    lightMapImage_ = Image::volume(context_->allocator(),
                                   {static_cast<std::uint32_t>(gridVoxels.x), static_cast<std::uint32_t>(gridVoxels.z),
                                    static_cast<std::uint32_t>(gridVoxels.y)},
                                   VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    for (Slot& slot : slots_) {
        slot.uniforms = Buffer(context_->allocator(), sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               MemoryUsage::Upload);
        const VkDescriptorSetAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &setLayout,
        };
        vkCheck(vkAllocateDescriptorSets(device, &allocInfo, &slot.descriptors), "vkAllocateDescriptorSets");
        slot.shadowMap = Image(context_->allocator(), {graphics_.shadowResolution, graphics_.shadowResolution},
                               shadowFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT);
        const VkDescriptorBufferInfo bufferInfo{slot.uniforms.get(), 0, sizeof(FrameUniforms)};
        const VkDescriptorImageInfo imageInfo{sampler, slot.shadowMap.view(), VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo gridInfo{gridSampler, lightGridImage_.view(),
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo mapInfo{gridSampler, lightMapImage_.view(),
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet writes[4] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = slot.descriptors,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo = &bufferInfo,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = slot.descriptors,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &imageInfo,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = slot.descriptors,
                .dstBinding = 6,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &gridInfo,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = slot.descriptors,
                .dstBinding = 9,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &mapInfo,
            },
        };
        vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
        reserveInstances(slot, 256);
        writeLights(slot, {}, false, {});
        // No local shadows until a frame draws some: a texel to bind.
        slot.localShadows = Image(context_->allocator(), {1, 1}, shadowFormat,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  VK_IMAGE_ASPECT_DEPTH_BIT);
        // A matrix per tile of the atlas.
        slot.localShadowMatrixCapacity = (localShadowAtlas / localShadowTile) * (localShadowAtlas / localShadowTile);
        slot.localShadowMatrices
            = Buffer(context_->allocator(), VkDeviceSize{slot.localShadowMatrixCapacity} * sizeof(Mat4),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryUsage::Upload);
        writeLocalShadowDescriptors(slot);
    }
}

void MeshRenderer::reserveInstances(Slot& slot, std::size_t count)
{
    if (count <= slot.capacity)
        return;
    slot.capacity = std::bit_ceil(std::max<std::uint32_t>(static_cast<std::uint32_t>(count), 256));
    slot.instances = Buffer(context_->allocator(), VkDeviceSize{slot.capacity} * sizeof(Instance),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, MemoryUsage::Stream);
    slot.shades = Buffer(context_->allocator(), VkDeviceSize{slot.capacity} * sizeof(Shade),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryUsage::Stream);
    // The slot's last frame is done, so its set can change.
    const VkDescriptorBufferInfo bufferInfo{slot.shades.get(), 0, VK_WHOLE_SIZE};
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = slot.descriptors,
        .dstBinding = 4,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &bufferInfo,
    };
    vkUpdateDescriptorSets(context_->device(), 1, &write, 0, nullptr);
}

void MeshRenderer::createPipelines()
{
    VkDevice device = context_->device();

    VkDescriptorSetLayout setLayout = setLayout_.get();
    // A local light's cube face's view-projection, for its shadow casters,
    // and the highlight a mask draw is for (highlight_mask.frag).
    const VkPushConstantRange pushRanges[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)},
                                               {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(Mat4), 16}};
    VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &setLayout,
        .pushConstantRangeCount = 2,
        .pPushConstantRanges = pushRanges,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCheck(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    layout_ = UniquePipelineLayout(device, layout);

    // Specialization: 0 turns on the alpha test, 1 sizes the texture array.
    struct Specialization {
        VkBool32 alphaTest = VK_FALSE;
        std::int32_t textureCount = 0;
        VkBool32 water = VK_FALSE;        // terrain water: the swell moves its vertices
        VkBool32 terrainItems = VK_FALSE; // terrain and water shading compiled in
        VkBool32 localShadow = VK_FALSE;  // shadow casters into a local light's cube face
        std::uint32_t rtSkip = 0;         // LUNAR_RT_SKIP: mesh.frag's rays to leave out
        VkBool32 decalLayer = VK_FALSE;   // the opaque pass: the decal layer goes into its colour
    };
    // LUNAR_RT_SKIP is a specialization constant, so its branch is uniform
    // over the whole draw and the driver folds it away: with the variable
    // unset (the mask 0, which is what every frame runs unless a benchmark
    // asks for otherwise) the shader is specialized to exactly what it was
    // before, ray queries and all, and the switch costs nothing per pixel.
    Specialization constants{.textureCount = static_cast<std::int32_t>(textures_->capacity()),
                             .rtSkip = rayTracingSkipMask()};
    if (constants.rtSkip != 0)
        std::println(stderr, "lunarstone: LUNAR_RT_SKIP={}: left out (research only)",
                     rayTracingSkipText());
    const VkSpecializationMapEntry entries[7] = {
        {0, offsetof(Specialization, alphaTest), sizeof(VkBool32)},
        {1, offsetof(Specialization, textureCount), sizeof(std::int32_t)},
        {2, offsetof(Specialization, water), sizeof(VkBool32)},
        {3, offsetof(Specialization, terrainItems), sizeof(VkBool32)},
        {4, offsetof(Specialization, localShadow), sizeof(VkBool32)},
        {5, offsetof(Specialization, rtSkip), sizeof(std::uint32_t)},
        {6, offsetof(Specialization, decalLayer), sizeof(VkBool32)},
    };
    const VkSpecializationInfo specialization{7, entries, sizeof(Specialization), &constants};

    UniqueShaderModule vert = makeShader(device, shaders::meshVert());
    UniqueShaderModule frag = makeShader(device, rays_ ? shaders::meshRayFrag() : shaders::meshFrag());
    // Ray traced, the opaque pass and the decals also hand the denoiser what
    // the local lights' shared shadow ray found (withDenoise below).
    UniqueShaderModule denoiseFrag;
    if (rays_)
        denoiseFrag = makeShader(device, shaders::meshRayDenoiseFrag());
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert.get(),
         .pName = "main",
         .pSpecializationInfo = &specialization},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag.get(),
         .pName = "main",
         .pSpecializationInfo = &specialization},
    };

    const VkVertexInputBindingDescription bindings[2] = {
        {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    const VkVertexInputAttributeDescription attributes[12] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {10, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, uv)},
        {11, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, uv) + 4},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, color)},
        {8, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {9, 0, VK_FORMAT_R8G8B8A8_SNORM, offsetof(Vertex, tangent)},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
        {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Instance, color)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 2,
        .pVertexBindingDescriptions = bindings,
        .vertexAttributeDescriptionCount = 12,
        .pVertexAttributeDescriptions = attributes,
    };
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    // The projection flips Y, which keeps counter-clockwise faces front facing.
    VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    // Reversed Z: nearer is larger.
    VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
    };
    VkPipelineColorBlendAttachmentState blendAttachment{
        .colorWriteMask
        = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blendAttachment,
    };
    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates,
    };
    VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &colorFormat_,
        .depthAttachmentFormat = depthFormat,
    };
    VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = layout,
    };

    VkPipeline pipeline = VK_NULL_HANDLE;
    // Ray traced, what draws in the opaque pass's scope (the solid geometry,
    // the sky and the decals) draws into the denoiser's three inputs too,
    // each as the given blend says (a write mask of 0 leaves it alone), and
    // with the shader that writes them where shade says. Otherwise as is.
    const VkFormat denoiseFormats[4] = {colorFormat_, denoiseFormat, denoiseFormat, denoiseFormat};
    auto withDenoise = [&](UniquePipeline& out, const std::array<VkPipelineColorBlendAttachmentState, 3>& extra,
                           bool shade) {
        const VkPipelineColorBlendAttachmentState* attachments = blend.pAttachments;
        const std::uint32_t attachmentCount = blend.attachmentCount;
        const VkPipelineColorBlendAttachmentState all[4] = {blend.pAttachments[0], extra[0], extra[1], extra[2]};
        if (rays_) {
            rendering.colorAttachmentCount = 4;
            rendering.pColorAttachmentFormats = denoiseFormats;
            blend.attachmentCount = 4;
            blend.pAttachments = all;
            if (shade)
                stages[1].module = denoiseFrag.get();
        }
        vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
                "vkCreateGraphicsPipelines");
        out = UniquePipeline(device, pipeline);
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat_;
        blend.attachmentCount = attachmentCount;
        blend.pAttachments = attachments;
        stages[1].module = frag.get();
    };
    // The solid geometry's: written as the colour is (or not, by the pre-pass).
    auto createSolid = [&](UniquePipeline& out) {
        const VkPipelineColorBlendAttachmentState written{.colorWriteMask = blendAttachment.colorWriteMask};
        withDenoise(out, {written, written, written}, true);
    };
    const VkPipelineColorBlendAttachmentState untouched{.colorWriteMask = 0};
    // The depth pre-pass: the opaque items' depth alone, vertex stage only.
    // The opaque pipelines then shade only what's equal to it, each pixel
    // once instead of every surface the front-to-back order didn't hide.
    const VkColorComponentFlags allColor = blendAttachment.colorWriteMask;
    blendAttachment.colorWriteMask = 0;
    info.stageCount = 1;
    createSolid(depthPrepass_);
    // The same with nothing but the depth attachment, for a frame with
    // decals in the decal layer: they're drawn over the depth in between.
    {
        const VkPipelineRenderingCreateInfo depthRendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .depthAttachmentFormat = depthFormat,
        };
        const VkPipelineColorBlendStateCreateInfo noColor{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        };
        VkGraphicsPipelineCreateInfo depthInfo = info;
        depthInfo.pNext = &depthRendering;
        depthInfo.pColorBlendState = &noColor;
        vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &depthInfo, nullptr, &pipeline),
                "vkCreateGraphicsPipelines");
        depthOnly_ = UniquePipeline(device, pipeline);
    }
    info.stageCount = 2;
    blendAttachment.colorWriteMask = allColor;

    // Highlights' mask: each highlighted item's id where it shows over the
    // scene's depth, or wherever it is for AlwaysOnTop.
    {
        UniqueShaderModule maskFrag = makeShader(device, shaders::highlightMaskFrag());
        VkPipelineShaderStageCreateInfo maskStages[2] = {stages[0], stages[1]};
        maskStages[1].module = maskFrag.get();
        maskStages[1].pSpecializationInfo = nullptr;
        const VkFormat maskFormat = HighlightPass::maskFormat;
        VkPipelineRenderingCreateInfo maskRendering = rendering;
        maskRendering.pColorAttachmentFormats = &maskFormat;
        VkPipelineDepthStencilStateCreateInfo maskDepth = depth;
        maskDepth.depthWriteEnable = VK_FALSE;
        maskDepth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        VkGraphicsPipelineCreateInfo maskInfo = info;
        maskInfo.pNext = &maskRendering;
        maskInfo.pStages = maskStages;
        maskInfo.pDepthStencilState = &maskDepth;
        vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &maskInfo, nullptr, &pipeline),
                "vkCreateGraphicsPipelines");
        highlightMask_ = UniquePipeline(device, pipeline);
        maskDepth.depthTestEnable = VK_FALSE;
        vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &maskInfo, nullptr, &pipeline),
                "vkCreateGraphicsPipelines");
        highlightMaskOnTop_ = UniquePipeline(device, pipeline);
    }

    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_EQUAL;
    constants.decalLayer = VK_TRUE;
    createSolid(opaque_);
    constants.decalLayer = VK_FALSE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

    // Cut-out meshes: opaque, but with holes where the texture is clear. A
    // pipeline of their own so the rest keep early depth testing.
    constants.alphaTest = VK_TRUE;
    createSolid(cutout_);
    constants.alphaTest = VK_FALSE;

    // The same two for terrain (and water drawn opaque), the only ones that
    // need its shading.
    constants.terrainItems = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_EQUAL;
    constants.decalLayer = VK_TRUE;
    createSolid(opaqueTerrain_);
    constants.decalLayer = VK_FALSE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    constants.alphaTest = VK_TRUE;
    createSolid(cutoutTerrain_);
    constants.alphaTest = VK_FALSE;
    constants.terrainItems = VK_FALSE;

    // Transparent meshes: tested against the opaque depth but not written,
    // blended over what's behind them.
    depth.depthWriteEnable = VK_FALSE;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    transparent_ = UniquePipeline(device, pipeline);

    // Terrain water: blended like transparent meshes, its vertices moved by
    // the swell. Its depth goes first: only the nearest water surface then
    // passes the test, the way the client's water (opaque over a copy of the
    // scene) never shows other water through it. Vertex stage only, nothing
    // written but depth.
    constants.water = VK_TRUE;
    constants.terrainItems = VK_TRUE;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    water_ = UniquePipeline(device, pipeline);
    depth.depthWriteEnable = VK_TRUE;
    const VkColorComponentFlags colorMask = blendAttachment.colorWriteMask;
    blendAttachment.colorWriteMask = 0;
    info.stageCount = 1;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    waterDepth_ = UniquePipeline(device, pipeline);
    info.stageCount = 2;
    blendAttachment.colorWriteMask = colorMask;
    depth.depthWriteEnable = VK_FALSE;
    constants.water = VK_FALSE;
    constants.terrainItems = VK_FALSE;

    // Decals: blended like transparent meshes, pulled towards the camera
    // (reversed Z: positive bias is nearer) so they win over the face they lie on.
    rasterization.depthBiasEnable = VK_TRUE;
    rasterization.depthBiasConstantFactor = 4.0f;
    rasterization.depthBiasSlopeFactor = 2.0f;
    // Ray traced, they blend what the denoiser gets as they blend their
    // colour, over the surface's, and leave its surface alone.
    withDenoise(decal_, {blendAttachment, blendAttachment, untouched}, true);

    // The decal layer's: biased and tested like those, unlit, each decal's
    // premultiplied colour over the ones below it.
    {
        UniqueShaderModule layerFrag = makeShader(device, shaders::decalLayerFrag());
        VkPipelineShaderStageCreateInfo layerStages[2] = {stages[0], stages[1]};
        layerStages[1].module = layerFrag.get();
        const VkPipelineRenderingCreateInfo layerRendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &decalLayerFormat,
            .depthAttachmentFormat = depthFormat,
        };
        const VkPipelineColorBlendAttachmentState over{
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = allColor,
        };
        const VkPipelineColorBlendStateCreateInfo layerBlend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &over,
        };
        VkGraphicsPipelineCreateInfo layerInfo = info;
        layerInfo.pNext = &layerRendering;
        layerInfo.pStages = layerStages;
        layerInfo.pColorBlendState = &layerBlend;
        vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &layerInfo, nullptr, &pipeline),
                "vkCreateGraphicsPipelines");
        decalLayer_ = UniquePipeline(device, pipeline);
    }

    // Shadow casters: positions and model matrices only, depth only. Both
    // sides are drawn so open meshes still cast; slope-scaled bias keeps lit
    // faces from shadowing themselves.
    UniqueShaderModule shadowVert = makeShader(device, shaders::shadowVert());
    UniqueShaderModule shadowFrag = makeShader(device, shaders::shadowFrag());
    const VkPipelineShaderStageCreateInfo shadowStages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = shadowVert.get(),
         .pName = "main",
         .pSpecializationInfo = &specialization},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = shadowFrag.get(),
         .pName = "main",
         .pSpecializationInfo = &specialization},
    };
    const VkVertexInputBindingDescription shadowBindings[2] = {
        {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(ShadowInstance), VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    const VkVertexInputAttributeDescription shadowAttributes[8] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {7, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ShadowInstance, uvTransform)},
        {6, 1, VK_FORMAT_R32_UINT, offsetof(ShadowInstance, texture)},
    };
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = shadowBindings;
    vertexInput.vertexAttributeDescriptionCount = 8;
    vertexInput.pVertexAttributeDescriptions = shadowAttributes;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.depthBiasEnable = VK_TRUE;
    rasterization.depthBiasConstantFactor = 2.0f;
    rasterization.depthBiasSlopeFactor = 2.5f;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    blend.attachmentCount = 0;
    rendering.colorAttachmentCount = 0;
    rendering.depthAttachmentFormat = shadowFormat;
    blendAttachment.blendEnable = VK_FALSE;
    info.stageCount = 1;
    info.pStages = shadowStages;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    shadow_ = UniquePipeline(device, pipeline);
    info.stageCount = 2;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    shadowCutout_ = UniquePipeline(device, pipeline);
    // Local lights' cube faces: the same, the face's view in a push constant.
    const VkBool32 wasAlphaTest = constants.alphaTest;
    constants.localShadow = VK_TRUE;
    info.stageCount = 1;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    localShadow_ = UniquePipeline(device, pipeline);
    info.stageCount = 2;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    localShadowCutout_ = UniquePipeline(device, pipeline);
    constants.localShadow = VK_FALSE;
    constants.alphaTest = wasAlphaTest;

    // The sky: one screen-filling triangle at the far plane, drawn after the
    // solid geometry wherever it left the depth clear.
    UniqueShaderModule skyVert = makeShader(device, shaders::skyVert());
    UniqueShaderModule skyFrag = makeShader(device, shaders::skyFrag());
    const VkPipelineShaderStageCreateInfo skyStages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = skyVert.get(),
         .pName = "main",
         .pSpecializationInfo = &specialization},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = skyFrag.get(),
         .pName = "main",
         .pSpecializationInfo = &specialization},
    };
    vertexInput.vertexBindingDescriptionCount = 0;
    vertexInput.vertexAttributeDescriptionCount = 0;
    rasterization.depthBiasEnable = VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    blend.attachmentCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.depthAttachmentFormat = depthFormat;
    info.stageCount = 2;
    info.pStages = skyStages;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
    sky_ = UniquePipeline(device, pipeline);
    // Ray traced, the same in the opaque pass's scope, where it writes none of
    // the denoiser's inputs (with its own shaders: shade false).
    if (rays_)
        withDenoise(skyDenoise_, {untouched, untouched, untouched}, false);

    effects_ = std::make_unique<EffectPass>(*context_, layout, colorFormat_, depthFormat, specialization, slots_.size());
    post_ = std::make_unique<PostPass>(*context_, slots_.size(), rays_ != nullptr);
    highlights_ = std::make_unique<HighlightPass>(*context_, slots_.size(), colorFormat_);
}

void MeshRenderer::addEffects(EffectSource* source)
{
    if (source && std::ranges::find(effectSources_, source) == effectSources_.end())
        effectSources_.push_back(source);
}

void MeshRenderer::removeEffects(EffectSource* source)
{
    std::erase(effectSources_, source);
}

Color3 MeshRenderer::skyAverage(const ImagePtr& image)
{
    if (!image || image->empty())
        return {};
    auto [it, added] = skyAverages_.try_emplace(image.get());
    SkyAverage& entry = it->second;
    if (added || entry.image != image) {
        // Every 8th pixel each way is plenty for a blur this wide.
        double sum[3]{};
        std::size_t count = 0;
        for (std::uint32_t y = 0; y < image->height; y += 8)
            for (std::uint32_t x = 0; x < image->width; x += 8) {
                const std::size_t at = (std::size_t{y} * image->width + x) * 4;
                for (std::size_t c = 0; c < 3; ++c)
                    sum[c] += static_cast<double>(toLinear(static_cast<float>(image->rgba[at + c]) / 255.0f));
                ++count;
            }
        entry.image = image;
        entry.mean = {static_cast<float>(sum[0] / static_cast<double>(count)),
                      static_cast<float>(sum[1] / static_cast<double>(count)),
                      static_cast<float>(sum[2] / static_cast<double>(count))};
    }
    entry.lastUsed = frame_;
    return entry.mean;
}

SunShadow SunShadow::fit(const Camera& camera, float aspect, Vector3 sunDirection, float distance, std::uint32_t texels)
{
    // The smallest sphere around the view frustum out to shadowDistance: its
    // size depends only on the field of view, so it never changes as the
    // camera turns.
    const float d = std::min(distance, camera.farPlane);
    const float tanHalf = std::tan(camera.fieldOfView * std::numbers::pi_v<float> / 360.0f);
    const float w = d * tanHalf * std::sqrt(1.0f + aspect * aspect); // far corner off the axis
    const float along = w <= d ? (d * d + w * w) / (2.0f * d) : d;
    SunShadow shadow;
    shadow.radius = w <= d ? along : w;
    shadow.texels = texels;

    const Vector3 sun = sunDirection.unit();
    const CFrame orientation = Camera::lookAt({}, -sun);
    const Mat4 toLight = viewMatrix(orientation);
    Vector3 center = toLight.transformPoint(camera.cframe.position + camera.cframe.lookVector() * along);
    const float texel = 2.0f * shadow.radius / static_cast<float>(texels);
    center.x = std::round(center.x / texel) * texel;
    center.y = std::round(center.y / texel) * texel;
    center = Mat4::fromCFrame(orientation).transformPoint(center);

    CFrame eye = orientation;
    eye.position = center + sun * (shadow.radius + casterReach);
    shadow.view = viewMatrix(eye);
    shadow.depth = 2.0f * shadow.radius + casterReach;

    Mat4 ortho;
    ortho.m[0][0] = 1.0f / shadow.radius;
    ortho.m[1][1] = 1.0f / shadow.radius;
    ortho.m[2][2] = -1.0f / shadow.depth;
    shadow.viewProjection = ortho * shadow.view;
    return shadow;
}

bool SunShadow::reaches(Vector3 center, float sphereRadius) const
{
    const Vector3 p = view.transformPoint(center);
    return std::abs(p.x) <= radius + sphereRadius && std::abs(p.y) <= radius + sphereRadius && -p.z >= -sphereRadius
           && -p.z <= depth + sphereRadius;
}

void MeshRenderer::setGraphics(const GraphicsSettings& settings)
{
    const bool resized = settings.shadowResolution != graphics_.shadowResolution;
    graphics_ = settings;
    drawDistance_ = settings.drawDistance;
    textures_->setQualityCap(settings.textureQuality);
    textures_->setSideCap(settings.textureSide);
    if (!resized)
        return;
    // Every slot's shadow map at the new size; no frame may still use the old ones.
    vkCheck(vkDeviceWaitIdle(context_->device()), "vkDeviceWaitIdle");
    for (Slot& slot : slots_) {
        slot.shadowMap = Image(context_->allocator(), {settings.shadowResolution, settings.shadowResolution},
                               shadowFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT);
        const VkDescriptorImageInfo imageInfo{shadowSampler_.get(), slot.shadowMap.view(),
                                              VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = slot.descriptors,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfo,
        };
        vkUpdateDescriptorSets(context_->device(), 1, &write, 0, nullptr);
    }
}

Buffer& MeshRenderer::stagingBuffer(Slot& slot, VkDeviceSize size)
{
    auto best = slot.spareStaging.end();
    for (auto it = slot.spareStaging.begin(); it != slot.spareStaging.end(); ++it)
        if (it->size() >= size && (best == slot.spareStaging.end() || it->size() < best->size()))
            best = it;
    if (best != slot.spareStaging.end()) {
        slot.staging.push_back(std::move(*best));
        slot.spareStaging.erase(best);
        return slot.staging.back();
    }
    // Rounded up to a power of two (1 MiB at least), so the next frames'
    // slightly different sizes fit it too.
    const VkDeviceSize rounded = std::bit_ceil(std::max(size, VkDeviceSize{1} << 20));
    return slot.staging.emplace_back(context_->allocator(), rounded, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     MemoryUsage::Upload);
}

MeshRenderer::Slot& MeshRenderer::prepareSlot(std::uint32_t slot, VkExtent2D extent)
{
    Slot& s = slots_.at(slot);
    // The caller waited for this slot's last frame, so its uploads are done
    // and their staging is free again, for the next ones. Giving staging back
    // is what costs (one 32 MB buffer's memory going back to the driver held
    // a frame up 7 ms), so only what's past spareStagingBytes goes, the
    // largest first, right after the burst that needed it: a place loading.
    for (Buffer& buffer : s.staging)
        s.spareStaging.push_back(std::move(buffer));
    s.staging.clear();
    s.rayScratch.clear();
    VkDeviceSize spare = 0;
    for (const Buffer& buffer : s.spareStaging)
        spare += buffer.size();
    while (spare > spareStagingBytes) {
        const auto largest = std::ranges::max_element(s.spareStaging, {}, &Buffer::size);
        spare -= largest->size();
        s.spareStaging.erase(largest);
    }
    if (s.depth.get() == VK_NULL_HANDLE || s.depth.extent().width != extent.width
        || s.depth.extent().height != extent.height) {
        s.depth = Image(context_->allocator(), extent, depthFormat,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT);
        s.sceneDepth = Image(context_->allocator(), extent, depthFormat,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        // The caller waited for this slot's last frame, so its set can change.
        const VkDescriptorImageInfo imageInfo{depthSampler_.get(), s.sceneDepth.view(),
                                              VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = s.descriptors,
            .dstBinding = 5,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfo,
        };
        vkUpdateDescriptorSets(context_->device(), 1, &write, 0, nullptr);
        s.decalLayer
            = Image(context_->allocator(), extent, decalLayerFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        const VkDescriptorImageInfo layerInfo{depthSampler_.get(), s.decalLayer.view(),
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet layerWrite{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = s.descriptors,
            .dstBinding = 14,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &layerInfo,
        };
        vkUpdateDescriptorSets(context_->device(), 1, &layerWrite, 0, nullptr);
        if (rays_) {
            // The denoiser's inputs, drawn into and then read; its two
            // visibility images, blurred into and read in turn.
            constexpr VkImageUsageFlags drawn = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            for (Image* image : {&s.rest, &s.restSeen, &s.restGuide})
                *image = Image(context_->allocator(), extent, denoiseFormat, drawn, VK_IMAGE_ASPECT_COLOR_BIT);
            for (Image& image : s.restVisibility)
                image = Image(context_->allocator(), extent, denoiseVisibilityFormat,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            for (std::size_t into = 0; into < 2; ++into) {
                const VkImageLayout readOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                const VkDescriptorImageInfo images[5] = {
                    {depthSampler_.get(), s.rest.view(), readOnly},
                    {depthSampler_.get(), s.restSeen.view(), readOnly},
                    {depthSampler_.get(), s.restGuide.view(), readOnly},
                    {depthSampler_.get(), s.restVisibility[1 - into].view(), VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, s.restVisibility[into].view(), VK_IMAGE_LAYOUT_GENERAL},
                };
                VkWriteDescriptorSet writes[5]{};
                for (std::uint32_t b = 0; b < 5; ++b)
                    writes[b] = {
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = s.denoiseSets[into],
                        .dstBinding = b,
                        .descriptorCount = 1,
                        .descriptorType
                        = b == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        .pImageInfo = &images[b],
                    };
                vkUpdateDescriptorSets(context_->device(), 5, writes, 0, nullptr);
            }
        }
    }
    return s;
}

MeshRenderer::GpuMesh* MeshRenderer::findMesh(const MeshPtr& mesh, std::vector<GpuMesh*>& uploads)
{
    const MeshPtr& source = mesh ? mesh : primitiveMesh(Primitive::Block);
    auto it = meshes_.find(source.get());
    if (it == meshes_.end()) {
        if (source->empty() || source->vertices.empty())
            return nullptr;
        GpuMesh gpu;
        gpu.source = source;
        gpu.levels[0] = {0, static_cast<std::uint32_t>(source->indices.size())};
        // Coarser levels follow the full one in the index buffer.
        std::uint32_t begin = 0;
        for (std::uint32_t end : source->lodEnds) {
            if (gpu.levelCount == maxLevels || end <= begin || end > source->lodIndices.size())
                break;
            gpu.levels[gpu.levelCount++] = {static_cast<std::uint32_t>(source->indices.size()) + begin, end - begin};
            begin = end;
        }
        gpu.screenSized = gpu.levelCount > 1 && source->lodErrors.size() + 1 >= gpu.levelCount;
        for (std::uint32_t level = 1; gpu.screenSized && level < gpu.levelCount; ++level)
            gpu.errors[level] = source->lodErrors[level - 1];
        const Bounds bounds = meshBounds(source.get());
        gpu.center = bounds.center();
        gpu.halfExtent = bounds.extent() * 0.5f;
        gpu.radius = bounds.extent().magnitude() * 0.5f;
        it = meshes_.emplace(source.get(), std::move(gpu)).first;
        uploads.push_back(&it->second);
    } else if (it->second.retryFrame != 0 && it->second.retryFrame <= frame_) {
        it->second.retryFrame = 0;
        uploads.push_back(&it->second);
    }
    return &it->second;
}

void MeshRenderer::upload(VkCommandBuffer cmd, Slot& slot, std::span<GpuMesh* const> meshes)
{
    VkDeviceSize total = 0;
    for (const GpuMesh* mesh : meshes)
        total += mesh->source->vertices.size() * sizeof(Vertex)
                 + (mesh->source->indices.size() + mesh->source->lodIndices.size()) * sizeof(std::uint32_t);
    // A mesh that doesn't fit on the GPU now is drawn once it does, tried
    // again a second later (GUESS) while the textures make room.
    constexpr std::uint64_t retryFrames = 60;
    auto defer = [&](GpuMesh& mesh) {
        mesh.vertices = {};
        mesh.indices = {};
        mesh.bytes = 0;
        mesh.retryFrame = frame_ + retryFrames;
    };
    Allocator& allocator = context_->allocator();
    Buffer* stagingBuffer = nullptr;
    try {
        stagingBuffer = &this->stagingBuffer(slot, total);
    } catch (const OutOfMemoryError&) {
        ++meshOutOfMemory_;
        for (GpuMesh* mesh : meshes)
            defer(*mesh);
        // Nothing was recorded, so both of upload()'s passes took no time.
        markPass(cmd, GpuPass::MeshUpload);
        markPass(cmd, GpuPass::BlasBuild);
        return;
    }
    const Buffer& staging = *stagingBuffer;
    std::byte* out = staging.mapped().data();

    VkDeviceSize offset = 0;
    std::vector<Vertex> vertices;
    for (GpuMesh* mesh : meshes) {
        const MeshData& data = *mesh->source;
        const VkDeviceSize vertexBytes = data.vertices.size() * sizeof(Vertex);
        const VkDeviceSize fullBytes = data.indices.size() * sizeof(std::uint32_t);
        const VkDeviceSize indexBytes = fullBytes + data.lodIndices.size() * sizeof(std::uint32_t);

        vertices.clear();
        vertices.reserve(data.vertices.size());
        // Terrain vertices always weigh some material; nothing else does.
        const bool terrain
            = std::ranges::any_of(data.vertices.front().materialWeights, [](std::uint8_t w) { return w != 0; });
        const std::vector<std::array<float, 4>> frames
            = terrain ? std::vector<std::array<float, 4>>(data.vertices.size()) : tangents(data);
        for (std::size_t i = 0; i < data.vertices.size(); ++i) {
            const MeshVertex& v = data.vertices[i];
            const std::array<float, 4>& t = frames[i];
            Vertex& gpu = vertices.emplace_back(
                Vertex{{v.position.x, v.position.y, v.position.z},
                       {v.normal.x, v.normal.y, v.normal.z},
                       {v.u, v.v},
                       {unorm8(toLinear(v.color.r)), unorm8(toLinear(v.color.g)), unorm8(toLinear(v.color.b)), 255},
                       {snorm8(t[0]), snorm8(t[1]), snorm8(t[2]), snorm8(t[3])}});
            if (terrain)
                std::memcpy(gpu.uv, v.materialWeights.data(), sizeof(gpu.uv));
        }
        std::memcpy(out + offset, vertices.data(), vertexBytes);
        std::memcpy(out + offset + vertexBytes, data.indices.data(), fullBytes);
        if (!data.lodIndices.empty())
            std::memcpy(out + offset + vertexBytes + fullBytes, data.lodIndices.data(), indexBytes - fullBytes);

        // Ray queries build from them and shaders read their UVs by address.
        const VkBufferUsageFlags rayUsage = rays_ ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                                                        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                                  : 0;
        try {
            mesh->vertices = Buffer(allocator, vertexBytes,
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rayUsage,
                                    MemoryUsage::GpuOnly);
            mesh->indices = Buffer(allocator, indexBytes,
                                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rayUsage,
                                   MemoryUsage::GpuOnly);
        } catch (const OutOfMemoryError&) {
            ++meshOutOfMemory_;
            defer(*mesh);
            offset += vertexBytes + indexBytes;
            continue;
        }
        mesh->bytes = vertexBytes + indexBytes;
        const VkBufferCopy vertexCopy{offset, 0, vertexBytes};
        vkCmdCopyBuffer(cmd, staging.get(), mesh->vertices.get(), 1, &vertexCopy);
        const VkBufferCopy indexCopy{offset + vertexBytes, 0, indexBytes};
        vkCmdCopyBuffer(cmd, staging.get(), mesh->indices.get(), 1, &indexCopy);
        offset += vertexBytes + indexBytes;
    }

    // Ray queries build from the copies too, and alpha tests read their UVs.
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask
        = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
          | (rays_ ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                   : VK_PIPELINE_STAGE_2_NONE),
        .dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT
                         | (rays_ ? VK_ACCESS_2_SHADER_READ_BIT : VK_ACCESS_2_NONE),
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
    // The mesh uploads are one pass and the bottom levels built from them
    // another, but they are recorded by this one function; the stamps split
    // them here, where the copies have just been made readable.
    markPass(cmd, GpuPass::MeshUpload);

    if (!rays_) {
        markPass(cmd, GpuPass::BlasBuild); // no ray queries: no bottom levels to build
        return;
    }
    bool built = false;
    for (GpuMesh* mesh : meshes) {
        if (mesh->vertices.get() == VK_NULL_HANDLE || mesh->levels[0].indexCount < 3)
            continue;
        try {
            rays_->buildBottom(cmd, mesh->source.get(), mesh->vertices,
                               static_cast<std::uint32_t>(mesh->source->vertices.size()), sizeof(Vertex), mesh->indices,
                               mesh->levels[0].indexCount, slot.rayScratch);
            mesh->rayBottom = rays_->bottom(mesh->source.get());
            mesh->rayVertices = rays_->bufferAddress(mesh->vertices);
            mesh->rayIndices = rays_->bufferAddress(mesh->indices);
            built = true;
        } catch (const OutOfMemoryError&) {
            // Left out of the shadows for now; it's tried again when re-uploaded.
            ++meshOutOfMemory_;
        }
    }
    if (built)
        rays_->barrierAfterBottoms(cmd);
    markPass(cmd, GpuPass::BlasBuild);
}

void MeshRenderer::updateBudget()
{
    Allocator& allocator = context_->allocator();
    const VkDeviceSize held = allocator.deviceLocalBytes();
    const std::uint64_t failures = textures_->outOfMemoryCount() + meshOutOfMemory_;
    const bool failed = failures != failuresSeen_;
    if (failed) {
        // The driver's budget was too generous (or another program just
        // took memory): stay below what failed, for now.
        if (failuresSeen_ == 0)
            std::println(stderr, "lunarstone: GPU memory is full at {} MiB; textures and meshes shrink to fit",
                         held >> 20);
        failuresSeen_ = failures;
        ceiling_ = std::min(ceiling_, held - held / 16);
    } else if (ceiling_ < ~VkDeviceSize{0} - (VkDeviceSize{1} << 20)) {
        ceiling_ += VkDeviceSize{1} << 20;
    }
    if (failed || memoryBudget_.heapSize == 0 || frame_ % budgetInterval == 0)
        memoryBudget_ = allocator.budget();

    const VkDeviceSize target = std::min(memoryBudget_.target, ceiling_);
    const VkDeviceSize textures = textures_->residentBytes();
    const VkDeviceSize others = held > textures ? held - textures : 0;
    // GUESS: the textures always get a little, so something shows.
    constexpr VkDeviceSize minimum = VkDeviceSize{16} << 20;
    textures_->setBudget(std::max(target > others ? target - others : 0, minimum));
    currentMeshBudget_ = std::min(meshBudget, target / 2);
    stats_.memoryBudget = target;
}

void MeshRenderer::fillSky(FrameUniforms& uniforms, const Camera& camera, float aspect, const Lighting& lighting)
{
    const float tanHalf = std::tan(camera.fieldOfView * std::numbers::pi_v<float> / 360.0f);
    const Vector3 forward = camera.cframe.lookVector();
    const Vector3 right = camera.cframe.rightVector() * (tanHalf * aspect);
    const Vector3 up = camera.cframe.upVector() * tanHalf;
    std::ranges::copy(std::array{forward.x, forward.y, forward.z, 0.0f}, uniforms.skyForward);
    std::ranges::copy(std::array{right.x, right.y, right.z, 0.0f}, uniforms.skyRight);
    std::ranges::copy(std::array{up.x, up.y, up.z, 0.0f}, uniforms.skyUp);

    const SkyLook& sky = lighting.skybox;
    constexpr float degrees = std::numbers::pi_v<float> / 180.0f;
    const CFrame rotation = CFrame::fromOrientation(sky.orientation.x * degrees, sky.orientation.y * degrees,
                                                    sky.orientation.z * degrees);
    for (std::size_t row = 0; row < 3; ++row) // the transpose: world to sky
        std::ranges::copy(std::array{rotation.r[0][row], rotation.r[1][row], rotation.r[2][row], 0.0f},
                          uniforms.skyRotation[row]);

    for (std::size_t i = 0; i < 6; ++i) {
        const std::uint32_t index = textures_->use(sky.faces[i], true);
        (i < 4 ? uniforms.skyFaces0[i] : uniforms.skyFaces1[i - 4]) = index;
        // Faces still on their way leave the flat colour in the blur too.
        const Color3 mean = index != 0
                                ? skyAverage(sky.faces[i])
                                : Color3{toLinear(lighting.sky.r), toLinear(lighting.sky.g), toLinear(lighting.sky.b)};
        std::ranges::copy(std::array{mean.r, mean.g, mean.b, 0.0f}, uniforms.skyAverage[i]);
    }
    // The client's ambient cube (EnvironmentDiffuseScale): the sky's light
    // along each world axis, tinted by the sky gradient, top for +Y, bottom
    // for -Y and their mean sideways. The client gets each axis's light from
    // its environment map; here it's each face's mean colour weighted the
    // way a cosine lobe sees a cube's faces (about 0.55 of a lobe falls on
    // the face it points at and 0.11 on each side face). With skylight 1
    // everywhere (no light grid) the client's indoor half cancels out.
    const float environment = lighting.environmentDiffuse;
    if (environment > 0.0f) {
        const Color3 top = lighting.skyGradientTop, bottom = lighting.skyGradientBottom;
        const Color3 middle{(top.r + bottom.r) * 0.5f, (top.g + bottom.g) * 0.5f, (top.b + bottom.b) * 0.5f};
        for (std::size_t axis = 0; axis < 6; ++axis) {
            Vector3 d;
            (axis < 2 ? d.x : axis < 4 ? d.y : d.z) = axis % 2 == 0 ? 1.0f : -1.0f;
            Color3 light;
            for (std::size_t face = 0; face < 6; ++face) {
                // The face's outward axis, from sky space to the world.
                Vector3 f;
                (face < 2 ? f.x : face < 4 ? f.y : f.z) = face % 2 == 0 ? 1.0f : -1.0f;
                const Vector3 world{rotation.r[0][0] * f.x + rotation.r[0][1] * f.y + rotation.r[0][2] * f.z,
                                    rotation.r[1][0] * f.x + rotation.r[1][1] * f.y + rotation.r[1][2] * f.z,
                                    rotation.r[2][0] * f.x + rotation.r[2][1] * f.y + rotation.r[2][2] * f.z};
                const float facing = d.dot(world);
                const float weight = std::max(facing, 0.0f) * 0.554f + (1.0f - std::abs(facing)) * 0.1114f;
                light
                    = {light.r + uniforms.skyAverage[face][0] * weight, light.g + uniforms.skyAverage[face][1] * weight,
                       light.b + uniforms.skyAverage[face][2] * weight};
            }
            const Color3 tint = axis == 2 ? top : axis == 3 ? bottom : middle;
            std::ranges::copy(std::array{light.r * tint.r * environment, light.g * tint.g * environment,
                                         light.b * tint.b * environment, 0.0f},
                              uniforms.ambientCube[axis]);
        }
    }
    uniforms.skyFaces1[2] = textures_->use(sky.sun, true);
    uniforms.skyFaces1[3] = textures_->use(sky.moon, true);
    std::ranges::copy(std::array{lighting.skyTint.r, lighting.skyTint.g, lighting.skyTint.b,
                                 sky.celestialBodiesShown ? lighting.starBrightness : 0.0f},
                      uniforms.skyTint);
    auto sprite = [&](float (&out)[4], Vector3 direction, float angularSize) {
        const Vector3 d = direction.unit();
        std::ranges::copy(std::array{d.x, d.y, d.z, std::tan(std::clamp(angularSize, 0.0f, 60.0f) * 0.5f * degrees)},
                          out);
    };
    sprite(uniforms.sunSprite, lighting.sunPosition, sky.sunAngularSize);
    sprite(uniforms.moonSprite, lighting.moonDirection, sky.moonAngularSize);
    std::ranges::copy(std::array{static_cast<float>(sky.starCount), sky.celestialBodiesShown ? 1.0f : 0.0f, 1.0f,
                                 0.6f + 0.4f * lighting.starBrightness},
                      uniforms.skyParams);

    const AtmosphereLook& atmosphere = lighting.atmosphere;
    putLinear(uniforms.atmosphereColor, atmosphere.color, atmosphere.densityPerStud());
    putLinear(uniforms.atmosphereDecay, atmosphere.decay, atmosphere.offset);
    std::ranges::copy(std::array{atmosphere.glare, atmosphere.haze, 0.0f, 0.0f}, uniforms.atmosphereParams);
    const CloudLook& clouds = lighting.clouds;
    std::ranges::copy(std::array{clouds.cover, clouds.density, clouds.enabled ? 1.0f : 0.0f, 0.0f}, uniforms.clouds);
    std::ranges::copy(std::array{toLinear(clouds.color.r), toLinear(clouds.color.g), toLinear(clouds.color.b), 0.0f},
                      uniforms.cloudColor);
}

void MeshRenderer::copySceneDepth(VkCommandBuffer cmd, Slot& s, VkExtent2D extent)
{
    constexpr VkPipelineStageFlags2 depthStages
        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    const VkImageMemoryBarrier2 before[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = depthStages,
            .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image = s.depth.get(),
            .subresourceRange = depthRange,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = s.sceneDepth.get(),
            .subresourceRange = depthRange,
        },
    };
    const VkDependencyInfo beforeCopy{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers = before,
    };
    vkCmdPipelineBarrier2(cmd, &beforeCopy);
    const VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
        .extent = {extent.width, extent.height, 1},
    };
    vkCmdCopyImage(cmd, s.depth.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.sceneDepth.get(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    const VkImageMemoryBarrier2 after[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = depthStages,
            .dstAccessMask
            = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .image = s.depth.get(),
            .subresourceRange = depthRange,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
            .image = s.sceneDepth.get(),
            .subresourceRange = depthRange,
        },
    };
    const VkDependencyInfo afterCopy{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers = after,
    };
    vkCmdPipelineBarrier2(cmd, &afterCopy);
}

void MeshRenderer::recordHighlights(VkCommandBuffer cmd, std::uint32_t slot, Slot& s, const RenderTarget& scene,
                                    std::size_t count, const std::function<void(std::size_t, bool)>& draw,
                                    std::span<const HighlightStyle* const> styles)
{
    const Image& mask = highlights_->mask(slot, scene.extent);
    const VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // The scene's depth, as the main pass left it, tested against.
    imageBarrier(cmd, {
                          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                                          | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                          .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .image = s.depth.get(),
                          .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
                      });
    // The mask, whatever the slot's last frame read of it.
    imageBarrier(cmd,
                 {
                     .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     .srcAccessMask = VK_ACCESS_2_NONE,
                     .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     .image = mask.get(),
                     .subresourceRange = colorRange,
                 });
    const VkRenderingAttachmentInfo color{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = mask.view(),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {.uint32 = {0, 0, 0, 0}}},
    };
    const VkRenderingAttachmentInfo depth{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = s.depth.view(),
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, scene.extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
        .pDepthAttachment = &depth,
    };
    vkCmdBeginRendering(cmd, &rendering);
    const VkViewport viewport{
        0.0f, 0.0f, static_cast<float>(scene.extent.width), static_cast<float>(scene.extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, scene.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &s.descriptors, 0, nullptr);
    const VkBuffer instanceBuffer = s.instances.get();
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 1, 1, &instanceBuffer, &zero);
    // AlwaysOnTop last: over an occluded highlight in front of it.
    for (const bool onTop : {false, true}) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          onTop ? highlightMaskOnTop_.get() : highlightMask_.get());
        for (std::size_t i = 0; i < count; ++i)
            draw(i, onTop);
    }
    vkCmdEndRendering(cmd);

    // The mask to the highlights' tile pass and their draw.
    imageBarrier(cmd,
                 {
                     .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     .image = mask.get(),
                     .subresourceRange = colorRange,
                 });
    // The scene, blended over where the main pass left it.
    imageBarrier(cmd, {
                          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .dstAccessMask
                          = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .image = scene.image,
                          .subresourceRange = colorRange,
                      });
    std::vector<HighlightPass::Style> looks;
    looks.reserve(styles.size());
    for (std::size_t i = 0; i < styles.size(); ++i) {
        const HighlightStyle& style = *styles[i];
        HighlightPass::Style look{.id = static_cast<std::uint32_t>(i + 1)};
        putLinear(look.fill, style.fillColor, 1.0f - std::clamp(style.fillTransparency, 0.0f, 1.0f));
        putLinear(look.outline, style.outlineColor, 1.0f - std::clamp(style.outlineTransparency, 0.0f, 1.0f));
        looks.push_back(look);
    }
    highlights_->record(cmd, slot, scene.view, scene.extent, looks);
}

void MeshRenderer::recordLightGrid(VkCommandBuffer cmd, Slot& slot, FrameUniforms& uniforms)
{
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    std::vector<LightGrid::ChunkTexels> chunks = lightGrid_->takeUploads();
    if (!lightGridCleared_ || !chunks.empty()) {
        const VkImage images[2] = {lightGridImage_.get(), lightMapImage_.get()};
        for (VkImage image : images)
            imageBarrier(cmd, {
                                  .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .oldLayout = lightGridCleared_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                 : VK_IMAGE_LAYOUT_UNDEFINED,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .image = image,
                                  .subresourceRange = range,
                              });
        if (!lightGridCleared_) {
            // Full light and no local light until the grid says otherwise.
            const VkClearColorValue white{.float32 = {1.0f, 1.0f, 1.0f, 1.0f}};
            const VkClearColorValue black{.float32 = {0.0f, 0.0f, 0.0f, 0.0f}};
            vkCmdClearColorImage(cmd, lightGridImage_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
            vkCmdClearColorImage(cmd, lightMapImage_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            lightGridCleared_ = true;
        }
        if (!chunks.empty()) {
            // Each chunk's sky and sun texels, then its local light.
            constexpr VkDeviceSize gridBytes = 4 * LightGrid::chunkVoxels;
            constexpr VkDeviceSize mapBytes = 4 * LightGrid::chunkVoxels;
            constexpr VkDeviceSize chunkBytes = gridBytes + mapBytes;
            Buffer& staging = stagingBuffer(slot, chunkBytes * chunks.size());
            const GridPoint size = lightGrid_->chunks();
            std::vector<VkBufferImageCopy> copies[2];
            for (auto& list : copies)
                list.reserve(chunks.size());
            for (std::size_t i = 0; i < chunks.size(); ++i) {
                std::memcpy(staging.mapped().data() + i * chunkBytes, chunks[i].texels.data(), gridBytes);
                std::memcpy(staging.mapped().data() + i * chunkBytes + gridBytes, chunks[i].light.data(), mapBytes);
                auto wrapped = [](int v, int n) { return ((v % n) + n) % n; };
                const VkOffset3D offset{wrapped(chunks[i].chunk.x, size.x) * LightGrid::chunkX,
                                        wrapped(chunks[i].chunk.z, size.z) * LightGrid::chunkZ,
                                        wrapped(chunks[i].chunk.y, size.y) * LightGrid::chunkY};
                for (std::size_t k = 0; k < 2; ++k)
                    copies[k].push_back({
                        .bufferOffset = i * chunkBytes + (k == 0 ? 0 : gridBytes),
                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .imageOffset = offset,
                        .imageExtent = {LightGrid::chunkX, LightGrid::chunkZ, LightGrid::chunkY},
                    });
            }
            for (std::size_t k = 0; k < 2; ++k)
                vkCmdCopyBufferToImage(cmd, staging.get(), images[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       static_cast<std::uint32_t>(copies[k].size()), copies[k].data());
        }
        for (VkImage image : images)
            imageBarrier(cmd, {
                                  .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  .image = image,
                                  .subresourceRange = range,
                              });
    }
    uniforms.lightMap[0] = lightGrid_->lightReady() ? 1.0f : 0.0f;
    // Where the grid is, half a voxel in from its edges, where the wrapped
    // texture would blend in the far side.
    const GridPoint low = lightGrid_->originVoxel(), size = lightGrid_->voxels();
    const float v = LightGrid::voxelStuds;
    std::ranges::copy(std::array{static_cast<float>(low.x) * v + v * 0.5f, static_cast<float>(low.y) * v + v * 0.5f,
                                 static_cast<float>(low.z) * v + v * 0.5f, lightGrid_->ready() ? 1.0f : 0.0f},
                      uniforms.lightGridMin);
    std::ranges::copy(std::array{static_cast<float>(low.x + size.x) * v - v * 0.5f,
                                 static_cast<float>(low.y + size.y) * v - v * 0.5f,
                                 static_cast<float>(low.z + size.z) * v - v * 0.5f, 0.0f},
                      uniforms.lightGridMax);
    std::ranges::copy(std::array{1.0f / (static_cast<float>(size.x) * v), 1.0f / (static_cast<float>(size.z) * v),
                                 1.0f / (static_cast<float>(size.y) * v), 0.0f},
                      uniforms.lightGridScale);
}

void MeshRenderer::writeLocalShadowDescriptors(Slot& slot)
{
    const VkDescriptorImageInfo atlas{shadowSampler_.get(), slot.localShadows.view(),
                                      VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo matrices{slot.localShadowMatrices.get(), 0, VK_WHOLE_SIZE};
    const VkWriteDescriptorSet writes[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = slot.descriptors,
            .dstBinding = 12,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &atlas,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = slot.descriptors,
            .dstBinding = 13,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &matrices,
        },
    };
    vkUpdateDescriptorSets(context_->device(), 2, writes, 0, nullptr);
}

void MeshRenderer::writeLights(Slot& slot, std::span<const LocalLight> lights, bool traced, std::span<const int> shadowAt)
{
    struct GpuLight { // lights.glsl's LocalLight
        float positionReach[4];
        float colorStart[4];
        float direction[4];
        float axisU[4];
        float axisV[4];
        float flags[4];
        float falloff[4];
    };
    static_assert(sizeof(GpuLight) == 112);
    const auto lightCount = static_cast<std::uint32_t>(std::max<std::size_t>(lights.size(), 1));
    const std::size_t words = std::max<std::size_t>(clusters_.cells.size() + clusters_.indices.size(), 1);
    bool replaced = false;
    if (lightCount > slot.lightCapacity) {
        slot.lightCapacity = std::bit_ceil(std::max(lightCount, 64u));
        slot.lights = Buffer(context_->allocator(), VkDeviceSize{slot.lightCapacity} * sizeof(GpuLight),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryUsage::Upload);
        replaced = true;
    }
    if (words > slot.clusterCapacity) {
        slot.clusterCapacity = std::bit_ceil(std::max(static_cast<std::uint32_t>(words), 4096u));
        slot.clusters = Buffer(context_->allocator(), VkDeviceSize{slot.clusterCapacity} * sizeof(std::uint32_t),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryUsage::Upload);
        replaced = true;
    }
    std::byte* out = slot.lights.mapped().data();
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const LocalLight& light = lights[i];
        const Color3 color = light.pixelColor();
        // SurfaceLights a hundredth of a stud back into their part.
        // [code: 0x1047e675a]
        const Vector3 at
            = light.kind == LocalLight::Kind::Surface ? light.position - light.direction * 0.01f : light.position;
        const float start = LocalLight::falloffStart * light.range;
        // Its reach from `at`, which a SurfaceLight's step back adds to.
        const float reach = light.reach() + (light.kind == LocalLight::Kind::Surface ? 0.02f : 0.0f);
        const GpuLight gpu{
            {at.x, at.y, at.z, reach},
            {color.r, color.g, color.b, start},
            {light.direction.x, light.direction.y, light.direction.z, light.cosHalfAngle},
            {light.axisU.x, light.axisU.y, light.axisU.z, light.halfU},
            {light.axisV.x, light.axisV.y, light.axisV.z, light.halfV},
            {traced && (light.shadows || graphics_.shadowAllLights) ? 1.0f : 0.0f, i < shadowAt.size() ? static_cast<float>(shadowAt[i]) : -1.0f,
             0.0f, std::sqrt(light.halfU * light.halfV)},
            // What the shaders would otherwise divide by, per pixel and light.
            {light.range, 1.0f / std::max(light.range - start, 1e-4f),
             light.cosHalfAngle != 0.0f ? 1.0f / (1.0f - light.cosHalfAngle) : 0.0f, 0.0f},
        };
        std::memcpy(out + i * sizeof(GpuLight), &gpu, sizeof(GpuLight));
    }
    std::byte* words0 = slot.clusters.mapped().data();
    // An empty vector's data() may be null, which memcpy may not be handed even for 0 bytes.
    if (!clusters_.cells.empty())
        std::memcpy(words0, clusters_.cells.data(), clusters_.cells.size() * sizeof(std::uint32_t));
    if (!clusters_.indices.empty())
        std::memcpy(words0 + clusters_.cells.size() * sizeof(std::uint32_t), clusters_.indices.data(),
                    clusters_.indices.size() * sizeof(std::uint32_t));
    if (!replaced)
        return;
    const VkDescriptorBufferInfo infos[2] = {{slot.lights.get(), 0, VK_WHOLE_SIZE},
                                             {slot.clusters.get(), 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t i = 0; i < 2; ++i)
        writes[i] = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = slot.descriptors,
            .dstBinding = 10 + i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &infos[i],
        };
    vkUpdateDescriptorSets(context_->device(), 2, writes, 0, nullptr);
}

namespace {

constexpr VkPipelineStageFlags2 depthStages
    = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
constexpr VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
// Every vertex buffer is bound from its start.
constexpr VkDeviceSize zeroOffset = 0;

// Texture alpha this far up (whole item opacity) cuts holes; below it blends.
constexpr float cutoutOpacity = 0.9f;
// Radius over distance below which a part is left out of the rays: about
// two pixels across at 1080p. GUESS.
constexpr float rayMinAngle = 0.001f;

// Material textures are for plain parts; a texture of its own replaces them.
bool plainPart(const DrawItem& item)
{
    return !item.texture && !item.decal && !item.physical && !item.normalMap && !item.roughnessMap
           && !item.metalnessMap && !item.terrain && !item.terrainWater;
}

struct MaterialMaps {
    std::uint32_t color = 0;
    std::uint32_t normal = 0;
    float studsPerTile = 4.0f;
    bool tintMask = false;
};
// A material and the MaterialVariant name its items carry (into items,
// which outlive the frame's use of them).
struct MaterialKey {
    Enum::Material material;
    std::string_view variant;
    bool operator==(const MaterialKey&) const = default;
};
struct MaterialKeyHash {
    std::size_t operator()(const MaterialKey& key) const
    {
        return std::hash<std::string_view>{}(key.variant) * 31u + static_cast<std::size_t>(key.material);
    }
};

// A slot's stream buffer of `stride`-byte elements made big enough for
// `count` of them, in powers of two from 256 up.
void reserveStream(Allocator& allocator, Buffer& buffer, std::uint32_t& capacity, std::size_t count,
                   std::size_t stride)
{
    if (count <= capacity)
        return;
    capacity = std::bit_ceil(std::max<std::uint32_t>(static_cast<std::uint32_t>(count), 256));
    buffer = Buffer(allocator, VkDeviceSize{capacity} * stride, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, MemoryUsage::Stream);
}

// A local light's cube map faces, in the order of their axes.
const std::array<Vector3, 6> cubeAxes = {{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};

// The faces a light shines through: a spot's, those its cone can reach.
std::uint8_t litFaces(const LocalLight& light)
{
    const float coneReach = light.kind == LocalLight::Kind::Spot
                                ? std::acos(std::clamp(light.cosHalfAngle, -1.0f, 1.0f)) + 0.9553f
                                : 4.0f;
    std::uint8_t mask = 0;
    for (std::size_t f = 0; f < 6; ++f)
        if (coneReach >= std::numbers::pi_v<float> || cubeAxes[f].dot(light.direction) >= std::cos(coneReach))
            mask |= static_cast<std::uint8_t>(1u << f);
    return mask;
}

// A cube face's projection out to `range`: standard depth, 0 at the near
// plane, its frustum a little wider than 90 degrees so the filter's taps at
// its edges stay in its tile.
Mat4 cubeFaceProjection(float range)
{
    const float tanHalf = 1.0f + 4.0f / static_cast<float>(MeshRenderer::localShadowTile);
    const float nearZ = 0.05f, farZ = std::max(range, 0.1f);
    Mat4 projection;
    projection.m[0] = {1.0f / tanHalf, 0.0f, 0.0f, 0.0f};
    projection.m[1] = {0.0f, 1.0f / tanHalf, 0.0f, 0.0f};
    projection.m[2] = {0.0f, 0.0f, farZ / (nearZ - farZ), -1.0f};
    projection.m[3] = {0.0f, 0.0f, nearZ * farZ / (nearZ - farZ), 0.0f};
    return projection;
}

// From a face's clip space to its tile's place in the atlas's texture coordinates.
Mat4 atlasTile(std::size_t tile)
{
    constexpr std::uint32_t perRow = MeshRenderer::localShadowAtlas / MeshRenderer::localShadowTile;
    const float size = static_cast<float>(MeshRenderer::localShadowTile) / static_cast<float>(MeshRenderer::localShadowAtlas);
    const float x = static_cast<float>(tile % perRow) * size, y = static_cast<float>(tile / perRow) * size;
    Mat4 toTile;
    toTile.m[0][0] = 0.5f * size;
    toTile.m[1][1] = 0.5f * size;
    toTile.m[3][0] = x + 0.5f * size;
    toTile.m[3][1] = y + 0.5f * size;
    return toTile;
}

// The faces (of those in faceMask) whose 90 degree pyramids a caster's
// sphere reaches into, `d` from the light to its centre.
std::uint8_t casterFaces(Vector3 d, float radius, std::uint8_t faceMask)
{
    const float components[3] = {d.x, d.y, d.z};
    const float slack = radius * 1.4143f + 0.1f;
    std::uint8_t mask = 0;
    for (std::size_t f = 0; f < 6; ++f) {
        if ((faceMask & (1u << f)) == 0)
            continue;
        const std::size_t a = f / 2;
        const float along = (f % 2 == 0 ? 1.0f : -1.0f) * components[a];
        const float b = components[(a + 1) % 3], c2 = components[(a + 2) % 3];
        if (along + radius > 0.0f && along - std::abs(b) > -slack && along - std::abs(c2) > -slack)
            mask |= static_cast<std::uint8_t>(1u << f);
    }
    return mask;
}

// Whether a point is inside the object-space box at `center` with half size
// `half` that model moves.
bool boxContains(const Mat4& model, Vector3 center, Vector3 half, Vector3 point)
{
    const Vector3 offset = point - Vector3{model.m[3][0], model.m[3][1], model.m[3][2]};
    const float centers[3] = {center.x, center.y, center.z};
    const float halves[3] = {half.x, half.y, half.z};
    for (std::size_t k = 0; k < 3; ++k) {
        const Vector3 column{model.m[k][0], model.m[k][1], model.m[k][2]};
        const float length2 = column.dot(column);
        if (!(length2 > 1e-12f && std::abs(offset.dot(column) / length2 - centers[k]) <= halves[k]))
            return false;
    }
    return true;
}

// The local lights' shadow casters by where they are, in cells of 32 studs;
// the biggest apart, looked at for every light. The cells are a list sorted
// by key, a cell's z the low bits of it, so a row of cells along z is one
// binary search and a run of it.
class CasterGrid {
public:
    // sphereOf(i) is the i-th caster's centre and radius.
    template <class SphereOf>
    void build(std::size_t count, SphereOf&& sphereOf)
    {
        cells_.reserve(count);
        for (std::uint32_t c = 0; c < count; ++c) {
            const auto [center, radius] = sphereOf(c);
            if (radius > cell)
                big_.push_back(c);
            else
                cells_.emplace_back(key(static_cast<int>(std::floor(center.x / cell)),
                                        static_cast<int>(std::floor(center.y / cell)),
                                        static_cast<int>(std::floor(center.z / cell))),
                                    c);
        }
        std::ranges::sort(cells_);
    }

    // Calls take(c) for the big casters, then for those in the cells within
    // `range` (and a cell more) of `position`.
    template <class Take>
    void around(Vector3 position, float range, Take&& take) const
    {
        for (std::uint32_t c : big_)
            take(c);
        const int r = static_cast<int>(std::ceil(range / cell)) + 1;
        const int cx = static_cast<int>(std::floor(position.x / cell));
        const int cy = static_cast<int>(std::floor(position.y / cell));
        const int cz = static_cast<int>(std::floor(position.z / cell));
        for (int x = cx - r; x <= cx + r; ++x)
            for (int y = cy - r; y <= cy + r; ++y) {
                const std::uint64_t last = key(x, y, cz + r);
                for (auto at = std::ranges::lower_bound(cells_, std::pair{key(x, y, cz - r), 0u});
                     at != cells_.end() && at->first <= last; ++at)
                    take(at->second);
            }
    }

private:
    static constexpr float cell = 32.0f;
    static constexpr int cellBias = 1 << 20;
    static std::uint64_t key(int x, int y, int z)
    {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x + cellBias) & 0x1fffff) << 42)
               | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y + cellBias) & 0x1fffff) << 21)
               | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z + cellBias) & 0x1fffff));
    }

    std::vector<std::pair<std::uint64_t, std::uint32_t>> cells_; // (cell, caster), sorted
    std::vector<std::uint32_t> big_;
};

// Takes the target from the colour attachment layout the frame left it in
// to the one the caller asked for.
void toFinalLayout(VkCommandBuffer cmd, const RenderTarget& target)
{
    VkImageMemoryBarrier2 toFinal{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = target.finalLayout,
        .image = target.image,
        .subresourceRange = colorRange,
    };
    if (target.finalLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        toFinal.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toFinal.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    } else if (target.finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        // Presentation waits on a semaphore signalled after this; no access needed.
        toFinal.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        toFinal.dstAccessMask = VK_ACCESS_2_NONE;
    }
    imageBarrier(cmd, toFinal);
}

} // namespace

// Everything record() works out about one frame, handed from each of its
// steps to the next: what's in view and in which pass, the instances'
// ranges, the uniforms, the local lights' shadows, and the attachments the
// scene's scopes draw into. Its own functions are the planning (and the draw
// calls themselves); MeshRenderer's steps record the passes around them.
// It lives on record()'s stack, and never moves (the rendering infos point
// into it).
struct MeshRenderer::Frame {
    // Cull, then sort what's left: opaque and cut-out items grouped by mesh,
    // decals by zIndex, transparent ones farthest first, shadow casters
    // grouped by mesh too.
    struct Draw {
        GpuMesh* mesh;
        std::uint32_t level;
        std::uint32_t count;
        std::uint32_t first;
        bool terrain = false; // terrain or water: drawn by the pipelines with their shading
    };
    struct Visible {
        std::uint32_t item;
        std::uint32_t draw; // opaque and cut out: index into their draws
        float depth;        // transparent: view-space z; decals: zIndex
        GpuMesh* mesh;
        float alpha;
        std::uint32_t level;
    };
    struct RayItem {
        std::uint32_t item;
        GpuMesh* mesh;
        bool cutout;
        bool casts; // in the sun's shadow rays too, not only reflections
    };
    // What can cast local lights' shadows into their cube maps (Future
    // lighting, not ray traced): solid shadow casters, wherever they are.
    struct LocalCaster {
        std::uint32_t item;
        GpuMesh* mesh;
        bool cutout;
        Vector3 center;
        float radius;
        // A light farther than this from the centre (squared) is outside its
        // mesh's box: the box's corners when its axes are square, else
        // everywhere (the exact test is done).
        float boxReach2;
    };
    struct Caster {
        std::uint32_t item;
        std::uint32_t draw;
        float depth = 0.0f; // from the shadow map's eye, along the sun
    };
    // Where an item is from the camera: worked out once, for its textures
    // and for culling.
    struct Placement {
        Vector3 center;
        float scale = 0.0f;
        float radius = 0.0f;
        float squared = 0.0f; // from the eye to the centre
        bool tooFar = false;
        bool seen = false;
    };
    // See-through items whose texture isn't on the GPU yet; looked at again
    // after this frame's uploads.
    struct Deferred {
        std::uint32_t item;
        GpuMesh* mesh;
        float alpha;
        Placement placement;
    };
    // The highlighted items among the visible ones, for the highlights'
    // mask: each highlight (up to maxHighlights) gets an id, in the order seen.
    struct HighlightDraw {
        GpuMesh* mesh;
        std::uint32_t level;
        std::uint32_t instance;
        std::uint32_t id;
        bool onTop;
    };
    // A shadowed local light this frame: its cube faces' view-projections
    // and the local casters each one's map draws.
    struct LocalShadow {
        std::uint32_t place = 0; // in the atlas: its faces are tiles 6 * place on
        std::array<Mat4, 6> faces;
        std::uint8_t faceMask = 0; // the faces it lights through
        bool redraw = false;       // its tiles drawn again this frame (else as the slot's last frame left them)
        // Per face, what's drawn into it (into localCasters), grouped by
        // cut-out and mesh.
        std::array<std::vector<std::uint32_t>, 6> casters;
    };
    // What looking for one light's casters reuses from the light before.
    struct CasterSearch {
        std::vector<std::uint32_t> nearby;
        // Its casters and the faces each is drawn into, until it's known
        // whether its tiles are drawn again.
        struct Hit {
            std::uint32_t caster;
            std::uint8_t faces;
        };
        std::vector<Hit> hits;
        // Each caster's look (mesh, place, texture) hashed once, when a
        // light first has it.
        std::vector<std::uint64_t> looks;
    };

    Frame(MeshRenderer& renderer, VkCommandBuffer commands, std::uint32_t slotIndex, Slot& slotState,
          const RenderTarget& finalTarget, const RenderTarget& sceneTarget, const Camera& eyeCamera,
          std::span<const DrawItem> drawItems, const Lighting& frameLighting, bool postEffects, bool scaledScene);
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    Placement place(const DrawItem& item, const GpuMesh& mesh) const;
    // How many texels across an item's textures show on screen: the item's
    // bounding sphere in pixels, over how often the texture repeats across
    // it. GUESS: twice that for mesh UVs, whose islands share the texture
    // and whose faces turn away.
    float texelsAcross(const DrawItem& item, const Placement& placement) const;
    // Puts a visible (or shadow casting) item into its pass and its draw.
    void classify(std::uint32_t i, GpuMesh* mesh, float alpha, const Placement& placement);
    // Decals by zIndex and transparent items farthest first; with no opaque
    // items, the decal layer's decals drawn as the others are.
    void sortBlended();
    // Opaque and cut-out instances nearest first (and shadow casters nearest
    // the sun), their draws in that order, and every draw its instance range.
    void sortSolid();
    // A plain part's material maps, looked up once a frame.
    MaterialMaps mapsFor(const MaterialKey& material, const MaterialTextureProvider& provider, TextureCache& textures);
    void noteHighlight(std::uint32_t at, const Visible& v);
    // The local lights' shadow maps this frame: each of `shadowed` (an index
    // into near and its place in the atlas), its faces, and whether its
    // tiles are drawn again. shadowAt gets each one's first face matrix.
    void planLocalShadows(std::span<const LocalLight> near, std::span<const std::pair<std::size_t, std::uint32_t>> shadowed,
                          std::span<int> shadowAt);
    void planLocalShadow(LocalShadow& out, const LocalLight& light, const CasterGrid& grid, CasterSearch& search,
                         bool atlasReady);
    // counted: false for the depth pre-pass, whose draws the opaque pass repeats.
    void drawMesh(const GpuMesh& mesh, std::uint32_t level, std::uint32_t count, std::uint32_t first,
                  bool counted = true) const;
    // A sorted list's draws: neighbours that share a mesh (and level) go in one.
    void drawRuns(const std::vector<Visible>& list, std::uint32_t first) const;

    VkCommandBuffer cmd;
    std::uint32_t slot;
    Slot& s;
    const RenderTarget& target;
    // The 3D image: the target itself, or one of the slot's own at
    // renderScale that's scaled up onto it before the overlay, by a blit or
    // by the post effects' last pass, which read it.
    RenderTarget scene;
    const Camera& camera;
    std::span<const DrawItem> items;
    const Lighting& lighting;
    bool post;
    bool scaled;
    FrameStats& stats;
    std::uint64_t number; // the renderer's frame_
    bool traced;          // with ray queries
    float drawDistance;

    float aspect;
    Mat4 viewProjection;
    Frustum frustum;
    Mat4 view;
    // Sun shadows by ray queries when there are any, else the shadow map.
    bool rayShadows;
    bool shadows;
    SunShadow sunShadow;
    LodSelector lods;
    Vector3 eye;
    float pixelsPerStud;

    std::unordered_map<MaterialKey, MaterialMaps, MaterialKeyHash> materialMaps;
    std::optional<std::pair<MaterialKey, MaterialMaps>> lastMaps;
    std::vector<GpuMesh*> uploads;
    std::vector<RayItem> rayItems;
    std::vector<LocalCaster> localCasters;
    std::vector<Deferred> deferred;
    std::vector<Draw> draws[2]; // opaque, cut out
    std::vector<Visible> solid[2];
    std::vector<Visible> decals;
    std::vector<Visible> layerDecals; // on solid parts: into the decal layer, unlit
    std::vector<Visible> transparent;
    std::vector<Draw> shadowDraws[2];
    std::vector<Caster> casters[2];
    // The effect batches, farthest first (those on top last), and the depth
    // each is drawn in among the transparent items at.
    std::vector<std::uint32_t> effectOrder;
    std::vector<float> effectDepth;
    std::uint32_t surfaceTextures[2]{};
    std::array<TerrainLayer, terrainMaterialCount> terrainLayers{};
    std::size_t visibleCount = 0;
    std::size_t casterCount = 0;
    // Where each kind's instances start: opaque and cut out first, then
    // decals, transparent items and the decal layer's.
    std::uint32_t decalFirst = 0;
    std::uint32_t transparentFirst = 0;
    std::uint32_t layerFirst = 0;
    std::vector<HighlightDraw> highlightDraws;
    std::vector<const HighlightStyle*> highlightIds;
    FrameUniforms uniforms;
    std::vector<LocalShadow> localShadowed;

    // The scene's attachments, kept over the scopes that draw into it (which
    // load what the one before stored).
    VkRenderingAttachmentInfo color{};
    VkRenderingAttachmentInfo depth{};
    VkRenderingInfo rendering{};
    // Ray traced, the solid geometry, the sky and the decals draw into the
    // denoiser's inputs too, in a scope of their own: it reads them after.
    VkRenderingAttachmentInfo solidColors[4]{};
    VkRenderingInfo solidRendering{};
    VkViewport viewport{};
    VkRect2D scissor{};
};

MeshRenderer::Frame::Frame(MeshRenderer& renderer, VkCommandBuffer commands, std::uint32_t slotIndex, Slot& slotState,
                           const RenderTarget& finalTarget, const RenderTarget& sceneTarget, const Camera& eyeCamera,
                           std::span<const DrawItem> drawItems, const Lighting& frameLighting, bool postEffects,
                           bool scaledScene)
    : cmd(commands), slot(slotIndex), s(slotState), target(finalTarget), scene(sceneTarget), camera(eyeCamera),
      items(drawItems), lighting(frameLighting), post(postEffects), scaled(scaledScene), stats(renderer.stats_),
      number(renderer.frame_), traced(renderer.rays_ != nullptr), drawDistance(renderer.drawDistance_),
      aspect(static_cast<float>(scene.extent.width) / static_cast<float>(scene.extent.height)),
      viewProjection(camera.viewProjection(aspect)), frustum(viewProjection), view(camera.view()),
      lods(camera, static_cast<float>(scene.extent.height), renderer.graphics_.lodPixelError),
      eye(camera.cframe.position),
      pixelsPerStud(static_cast<float>(scene.extent.height)
                    / (2.0f * std::tan(camera.fieldOfView * std::numbers::pi_v<float> / 360.0f)))
{
    const GraphicsSettings& graphics = renderer.graphics_;
    const bool sunShadows
        = graphics.shadows && lighting.shadows && lighting.sunDirection.dot(lighting.sunDirection) > 0.0f;
    rayShadows = sunShadows && traced;
    shadows = sunShadows && !traced;
    sunShadow.texels = graphics.shadowResolution;
    if (shadows)
        sunShadow = SunShadow::fit(camera, aspect, lighting.sunDirection, graphics.shadowDistance,
                                   graphics.shadowResolution);
    solid[0].reserve(items.size());
}

MeshRenderer::Frame::Placement MeshRenderer::Frame::place(const DrawItem& item, const GpuMesh& mesh) const
{
    Placement p;
    p.center = item.model.transformPoint(mesh.center);
    p.scale = maxScale(item.model);
    p.radius = mesh.radius * p.scale;
    const Vector3 offset = p.center - eye;
    p.squared = offset.dot(offset);
    if (!item.terrain && !item.terrainWater) {
        // GUESS: the client measures the draw distance to the part's
        // bounding sphere.
        const float reach = drawDistance + p.radius;
        p.tooFar = p.squared > reach * reach;
    }
    p.seen = !p.tooFar && frustum.intersects(p.center, p.radius);
    return p;
}

float MeshRenderer::Frame::texelsAcross(const DrawItem& item, const Placement& placement) const
{
    if (!placement.seen)
        return 0.0f; // in use, but not on screen
    const float distance = std::max(std::sqrt(placement.squared) - placement.radius, camera.nearPlane);
    const float repeats = std::max(std::min(std::abs(item.uvScale[0]), std::abs(item.uvScale[1])), 1.0f / 64.0f);
    const float uvSpread = item.clampUv || item.decal ? 1.0f : 2.0f;
    return 2.0f * placement.radius * pixelsPerStud / distance * uvSpread / repeats;
}

void MeshRenderer::Frame::classify(std::uint32_t i, GpuMesh* mesh, float alpha, const Placement& placement)
{
    const DrawItem& item = items[i];
    // Dithered terrain cuts holes too.
    const bool holes = (item.texture && item.alphaMode == TextureAlpha::Transparency)
                       || (item.terrain && (item.dither[0] > 0.0f || item.dither[1] < 1.0f));
    enum class Pass : std::uint8_t { Opaque, Cutout, Decal, Transparent };
    Pass pass = Pass::Opaque;
    if (item.decal)
        pass = Pass::Decal;
    else if (holes)
        pass = alpha >= cutoutOpacity ? Pass::Cutout : Pass::Transparent;
    else if (alpha < 1.0f)
        pass = Pass::Transparent;
    const std::size_t k = pass == Pass::Cutout ? 1 : 0;

    const Vector3 center = placement.center;
    const float scale = placement.scale;
    if (placement.tooFar) {
        ++stats.tooFar;
        return;
    }
    const bool casts = shadows && (pass == Pass::Opaque || pass == Pass::Cutout) && item.castShadow
                       && sunShadow.reaches(center, placement.radius);
    const bool seen = placement.seen;
    // Everything solid is in the rays' scene, however far away (shadow
    // rays have no map to fall off), down to what's under a pixel or so
    // across from the camera: what it would shadow or show in a mirror
    // is as small. Only casters stop sun rays.
    if (traced && (pass == Pass::Opaque || pass == Pass::Cutout)
        && placement.radius * placement.radius > placement.squared * rayMinAngle * rayMinAngle)
        rayItems.push_back({i, mesh, pass == Pass::Cutout, rayShadows && item.castShadow});
    if (!traced && (pass == Pass::Opaque || pass == Pass::Cutout) && item.castShadow)
        localCasters.push_back({i, mesh, pass == Pass::Cutout && item.texture != nullptr, center, placement.radius,
                                boxReach2(item.model, mesh->halfExtent, center)});
    if (!casts && !seen)
        return;
    // Shadows are cast at the level the camera sees.
    std::uint32_t level = 0;
    if (mesh->levelCount > 1 && item.renderFidelity != Enum::RenderFidelity::Precise) {
        const float distance = squaredDistanceToBox(eye, center, boxHalfExtent(item.model, mesh->halfExtent));
        level = mesh->screenSized
                    ? lods.levelForError(distance, scale, std::span(mesh->errors).first(mesh->levelCount),
                                         item.renderFidelity)
                    : lods.levelAt(distance, mesh->levelCount, item.renderFidelity);
    }

    if (casts) {
        if (mesh->shadowFrame[k][level] != number) {
            mesh->shadowFrame[k][level] = number;
            mesh->shadowIndex[k][level] = static_cast<std::uint32_t>(shadowDraws[k].size());
            shadowDraws[k].push_back({mesh, level, 0, 0});
        }
        ++shadowDraws[k][mesh->shadowIndex[k][level]].count;
        casters[k].push_back({i, mesh->shadowIndex[k][level], -sunShadow.view.transformPoint(center).z});
    }

    if (!seen)
        return;
    if (mesh->levelCount > 1)
        ++stats.levels[std::min<std::size_t>(level, stats.levels.size() - 1)];
    Visible visible{i, 0, 0.0f, mesh, alpha, level};
    switch (pass) {
    case Pass::Opaque:
    case Pass::Cutout:
        visible.depth = squaredDistanceToBox(eye, center, boxHalfExtent(item.model, mesh->halfExtent));
        if (mesh->drawFrame[k][level] != number) {
            mesh->drawFrame[k][level] = number;
            mesh->drawIndex[k][level] = static_cast<std::uint32_t>(draws[k].size());
            draws[k].push_back({mesh, level, 0, 0, item.terrain || item.terrainWater});
        }
        ++draws[k][mesh->drawIndex[k][level]].count;
        visible.draw = mesh->drawIndex[k][level];
        solid[k].push_back(visible);
        break;
    case Pass::Decal:
        visible.depth = static_cast<float>(item.zIndex);
        (item.onSolid ? layerDecals : decals).push_back(visible);
        break;
    case Pass::Transparent:
        visible.depth = view.transformPoint(center).z;
        transparent.push_back(visible);
        break;
    }
}

void MeshRenderer::Frame::sortBlended()
{
    // The layer is drawn over the depth pre-pass, which a frame with no
    // opaque items has none of.
    if (solid[0].empty()) {
        decals.insert(decals.end(), layerDecals.begin(), layerDecals.end());
        layerDecals.clear();
    }
    std::ranges::stable_sort(decals, std::less{}, &Visible::depth);
    std::ranges::stable_sort(layerDecals, std::less{}, &Visible::depth);
    std::ranges::stable_sort(transparent, std::less{}, &Visible::depth);
}

void MeshRenderer::Frame::sortSolid()
{
    // Opaque and cut-out instances nearest first, and their draws in the
    // order of their nearest instance: the depth test then turns away much
    // of what's hidden before it's shaded (in scene order, walls, furniture
    // and terrain behind each other were all shaded).
    for (std::size_t k = 0; k < 2; ++k) {
        std::ranges::sort(solid[k], {}, &Visible::depth);
        constexpr auto unseen = static_cast<std::uint32_t>(-1);
        std::vector<std::uint32_t> order(draws[k].size(), unseen);
        std::vector<Draw> sorted;
        sorted.reserve(draws[k].size());
        for (Visible& v : solid[k]) {
            if (order[v.draw] == unseen) {
                order[v.draw] = static_cast<std::uint32_t>(sorted.size());
                sorted.push_back(draws[k][v.draw]);
            }
            v.draw = order[v.draw];
        }
        // The parts first, then the terrain: one pipeline change between them.
        std::vector<std::uint32_t> partsFirst(sorted.size());
        std::uint32_t next = 0;
        for (const bool terrain : {false, true})
            for (std::size_t d = 0; d < sorted.size(); ++d)
                if (sorted[d].terrain == terrain)
                    partsFirst[d] = next++;
        for (Visible& v : solid[k])
            v.draw = partsFirst[v.draw];
        draws[k].assign(sorted.size(), {});
        for (std::size_t d = 0; d < sorted.size(); ++d)
            draws[k][partsFirst[d]] = sorted[d];
    }

    // Shadow casters likewise, nearest the sun first: the shadow map's pass
    // is all depth fill, and what lies behind is then turned away early.
    for (std::size_t k = 0; k < 2; ++k) {
        std::ranges::sort(casters[k], {}, &Caster::depth);
        constexpr auto unseen = static_cast<std::uint32_t>(-1);
        std::vector<std::uint32_t> order(shadowDraws[k].size(), unseen);
        std::vector<Draw> sorted;
        sorted.reserve(shadowDraws[k].size());
        for (Caster& caster : casters[k]) {
            if (order[caster.draw] == unseen) {
                order[caster.draw] = static_cast<std::uint32_t>(sorted.size());
                sorted.push_back(shadowDraws[k][caster.draw]);
            }
            caster.draw = order[caster.draw];
        }
        shadowDraws[k] = std::move(sorted);
    }

    // Instances land grouped by draw (a counting sort): opaque, cut out, then
    // decals and transparent ones in their sorted order.
    auto assignRanges = [](std::vector<Draw>& list, std::uint32_t next) {
        for (Draw& draw : list) {
            draw.first = next;
            next += draw.count;
            draw.count = 0;
        }
        return next;
    };
    decalFirst = assignRanges(draws[1], assignRanges(draws[0], 0));
    transparentFirst = static_cast<std::uint32_t>(decalFirst + decals.size());
    layerFirst = static_cast<std::uint32_t>(transparentFirst + transparent.size());
    assignRanges(shadowDraws[1], assignRanges(shadowDraws[0], 0));
}

MaterialMaps MeshRenderer::Frame::mapsFor(const MaterialKey& material, const MaterialTextureProvider& provider,
                                          TextureCache& textures)
{
    if (!provider)
        return MaterialMaps{};
    if (lastMaps && lastMaps->first == material)
        return lastMaps->second;
    auto [it, added] = materialMaps.try_emplace(material);
    if (added) {
        const MaterialTextures found = provider(material.material, material.variant);
        it->second.color = textures.use(found.color, true);
        it->second.normal = it->second.color != 0 ? textures.use(found.normal, false) : 0;
        it->second.studsPerTile = found.studsPerTile;
        it->second.tintMask = found.tintMask;
    }
    lastMaps.emplace(material, it->second);
    return it->second;
}

void MeshRenderer::Frame::noteHighlight(std::uint32_t at, const Visible& v)
{
    const HighlightStyle* style = items[v.item].highlight.get();
    if (!style)
        return;
    auto it = std::ranges::find(highlightIds, style);
    if (it == highlightIds.end()) {
        if (highlightIds.size() >= maxHighlights)
            return;
        highlightIds.push_back(style);
        it = highlightIds.end() - 1;
    }
    highlightDraws.push_back(
        {v.mesh, v.level, at, static_cast<std::uint32_t>(it - highlightIds.begin()) + 1, style->alwaysOnTop});
}

void MeshRenderer::Frame::planLocalShadows(std::span<const LocalLight> near,
                                           std::span<const std::pair<std::size_t, std::uint32_t>> shadowed,
                                           std::span<int> shadowAt)
{
    CasterGrid grid;
    if (!shadowed.empty())
        grid.build(localCasters.size(), [&](std::uint32_t c) {
            return std::pair{localCasters[c].center, localCasters[c].radius};
        });
    CasterSearch search;
    search.looks.assign(shadowed.empty() ? 0 : localCasters.size(), 0);
    const bool atlasReady = s.localShadowsReady && s.localShadows.extent().width == localShadowAtlas;
    for (const auto& [i, atlasPlace] : shadowed) {
        LocalShadow& out = localShadowed.emplace_back();
        out.place = atlasPlace;
        shadowAt[i] = static_cast<int>(6 * out.place);
        planLocalShadow(out, near[i], grid, search, atlasReady);
    }
}

void MeshRenderer::Frame::planLocalShadow(LocalShadow& out, const LocalLight& light, const CasterGrid& grid,
                                          CasterSearch& search, bool atlasReady)
{
    out.faceMask = litFaces(light);
    const Mat4 projection = cubeFaceProjection(light.range);
    std::byte* matrices = s.localShadowMatrices.mapped().data();
    for (std::size_t f = 0; f < 6; ++f) {
        out.faces[f] = projection * viewMatrix(Camera::lookAt(light.position, light.position + cubeAxes[f]));
        const std::size_t tile = 6 * out.place + f;
        const Mat4 sampled = atlasTile(tile) * out.faces[f];
        std::memcpy(matrices + tile * sizeof(Mat4), &sampled, sizeof(Mat4));
    }
    auto lookOf = [&](std::uint32_t c) {
        if (search.looks[c] == 0) {
            const LocalCaster& caster = localCasters[c];
            const DrawItem& item = items[caster.item];
            std::uint64_t h = mixHash(std::bit_cast<std::uint64_t>(caster.mesh),
                                      caster.cutout ? std::bit_cast<std::uint64_t>(item.texture.get()) : 0);
            for (const auto& column : item.model.m)
                for (float v : column)
                    h = mixHash(h, std::bit_cast<std::uint32_t>(v));
            search.looks[c] = h | 1u;
        }
        return search.looks[c];
    };
    // Its casters: what its range reaches, less any part it's inside (its
    // lamp shines out; GUESS: the client's parts don't shadow their own
    // lights), into the faces they're seen through.
    std::vector<std::uint32_t>& nearby = search.nearby;
    nearby.clear();
    search.hits.clear();
    grid.around(light.position, light.range, [&](std::uint32_t c) {
        const LocalCaster& caster = localCasters[c];
        const Vector3 d = caster.center - light.position;
        const float reach = caster.radius + light.range;
        if (d.dot(d) < reach * reach)
            nearby.push_back(c);
    });
    std::uint64_t signature = mixHash(std::bit_cast<std::uint64_t>(light.key), out.faceMask);
    for (float v : {light.position.x, light.position.y, light.position.z, light.range})
        signature = mixHash(signature, std::bit_cast<std::uint32_t>(v));
    // Which casters it reaches and through which faces follows from the
    // light and their looks (mesh and place), so the same light over the
    // same casters draws the same tiles: in whatever order they were found,
    // which their hashes are summed over.
    std::uint64_t looks = 0;
    for (std::uint32_t c : nearby)
        looks += mixHash(lookOf(c), 0x2545f4914f6cdd1dull);
    const std::uint64_t inputs = mixHash(mixHash(signature, looks), nearby.size());
    if (atlasReady && s.localShadowInputs[out.place] == inputs) {
        out.redraw = false;
        return;
    }
    s.localShadowInputs[out.place] = inputs;
    // Drawn grouped by cut-out and mesh.
    std::ranges::sort(nearby, {}, [&](std::uint32_t c) {
        return std::tuple{localCasters[c].cutout, static_cast<const void*>(localCasters[c].mesh), c};
    });
    for (std::uint32_t c : nearby) {
        const LocalCaster& caster = localCasters[c];
        const Vector3 d = caster.center - light.position;
        // Most are far enough from it to say so without the test.
        if (d.dot(d) <= caster.boxReach2
            && boxContains(items[caster.item].model, caster.mesh->center, caster.mesh->halfExtent, light.position))
            continue;
        const std::uint8_t mask = casterFaces(d, caster.radius, out.faceMask);
        if (mask == 0)
            continue;
        search.hits.push_back({c, mask});
        signature = mixHash(mixHash(signature, lookOf(c)), mask);
    }
    out.redraw = !atlasReady || s.localShadowSignatures[out.place] != signature;
    s.localShadowSignatures[out.place] = signature;
    // Only a light whose tiles are drawn again needs its casters by face.
    if (out.redraw)
        for (const auto& [c, mask] : search.hits)
            for (std::size_t f = 0; f < 6; ++f)
                if (mask & (1u << f))
                    out.casters[f].push_back(c);
}

void MeshRenderer::Frame::drawMesh(const GpuMesh& mesh, std::uint32_t level, std::uint32_t count, std::uint32_t first,
                                   bool counted) const
{
    const GpuMesh::Level& range = mesh.levels[level];
    VkBuffer vertices = mesh.vertices.get();
    if (vertices == VK_NULL_HANDLE)
        return; // didn't fit on the GPU this time
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices, &zeroOffset);
    vkCmdBindIndexBuffer(cmd, mesh.indices.get(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, range.indexCount, count, range.firstIndex, 0, first);
    if (!counted)
        return;
    ++stats.drawCalls;
    stats.triangles += std::uint64_t{range.indexCount / 3} * count;
}

void MeshRenderer::Frame::drawRuns(const std::vector<Visible>& list, std::uint32_t first) const
{
    for (std::uint32_t i = 0; i < list.size();) {
        std::uint32_t end = i + 1;
        while (end < list.size() && list[end].mesh == list[i].mesh && list[end].level == list[i].level)
            ++end;
        drawMesh(*list[i].mesh, list[i].level, end - i, first + i);
        i = end;
    }
}

void MeshRenderer::record(VkCommandBuffer cmd, std::uint32_t slot, const RenderTarget& target, const Camera& camera,
                          std::span<const DrawItem> items, const Lighting& lighting)
{
    // The frame's first stamp, and the reset the rest of the frame's build
    // on: vkCmdResetQueryPool is only legal outside a rendering scope, and
    // nothing of this frame's is recorded yet. Off, this costs one branch.
    if (passTimestamps_) {
        vkCmdResetQueryPool(cmd, passTimestamps_.get(), 0, passQueryCount);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, passTimestamps_.get(), 0);
    }
    const bool post = graphics_.postEffects && lighting.post.any();
    const bool scaled = sceneScale(graphics_) < 0.999f && (target.blitTarget || post);
    const RenderTarget scene = scaled || post ? sizedScene(slot, target, scaled) : target;
    const auto started = std::chrono::steady_clock::now();
    auto mark = started;
    const auto lap = [&mark](double& into) {
        const auto now = std::chrono::steady_clock::now();
        into = std::chrono::duration<double, std::milli>(now - mark).count();
        mark = now;
    };
    FrameStats::RecordPhases phases;
    Slot& s = prepareSlot(slot, scene.extent);
    ++frame_;
    textures_->beginFrame(frame_);
    stats_ = {.items = static_cast<std::uint32_t>(items.size())};
    updateBudget();

    Frame frame(*this, cmd, slot, s, target, scene, camera, items, lighting, post, scaled);
    lights_.clear();
    if (lightSource_)
        lightSource_->collect(lights_);
    stats_.localLights = static_cast<std::uint32_t>(lights_.size());
    lightGrid_->update(items, frame.eye, {.sunDirection = lighting.sunDirection, .shadows = lighting.shadows}, lights_);

    lap(phases.setup);
    // What's drawn where, with the textures it uses on the GPU.
    cull(frame);
    lap(phases.steps[0]);
    collectEffects(frame);
    lap(phases.steps[1]);
    uploadTextures(frame);
    lap(phases.steps[2]);
    frame.sortBlended();
    lap(phases.steps[3]);
    orderEffects(frame);
    lap(phases.steps[4]);
    frame.sortSolid();
    lap(phases.steps[5]);
    writeInstances(frame);
    lap(phases.steps[6]);
    for (const double step : phases.steps)
        phases.build += step;

    // How it's lit.
    fillUniforms(frame);
    recordLightGrid(cmd, s, frame.uniforms);
    // The grid's chunk copies and the barrier that made them readable; the
    // gap to the next stamp is what that took.
    markPass(cmd, GpuPass::LightGrid);
    planLocalLights(frame);
    std::memcpy(s.uniforms.mapped().data(), &frame.uniforms, sizeof(frame.uniforms));

    if (!frame.uploads.empty())
        upload(cmd, s, frame.uploads);
    else {
        // upload() stamps its own two passes when it runs; with nothing new
        // to upload it is not called, so they are stamped here instead.
        markPass(cmd, GpuPass::MeshUpload);
        markPass(cmd, GpuPass::BlasBuild);
    }
    if (rays_)
        buildRayScene(frame);
    // After buildTop() and the barrier that made the new top level readable
    // (or after the last of the frame's uploads, with no ray queries).
    markPass(cmd, GpuPass::TlasBuild);
    stats_.meshesUploaded = static_cast<std::uint32_t>(frame.uploads.size());
    stats_.texturesUploaded = textures_->uploadedLastFrame();

    lap(phases.prepare);
    // The passes.
    recordSunShadow(frame);
    lap(phases.sun);
    recordLocalShadows(frame);
    lap(phases.local);
    recordSolid(frame);
    lap(phases.solid);
    recordDenoise(frame);
    recordTransparent(frame);
    if (!frame.highlightDraws.empty())
        recordHighlights(cmd, slot, s, scene, frame.highlightDraws.size(), [&](std::size_t i, bool onTop) {
            const Frame::HighlightDraw& draw = frame.highlightDraws[i];
            if (draw.onTop != onTop)
                return;
            vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(Mat4), sizeof(std::uint32_t),
                               &draw.id);
            frame.drawMesh(*draw.mesh, draw.level, 1, draw.instance);
        }, frame.highlightIds);
    // Highlighted items are few and none most frames, so the stamp is written
    // whether or not the mask and fill passes above ran.
    markPass(cmd, GpuPass::Highlights);
    recordPost(frame);
    recordUpscale(frame);
    recordOverlay(frame);
    toFinalLayout(cmd, target);

    releaseUnused();
    lap(phases.rest);
    stats_.phases = phases;
    stats_.recordMilliseconds
        = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

RenderTarget MeshRenderer::sizedScene(std::uint32_t slot, const RenderTarget& target, bool scaled)
{
    const float scale = scaled ? sceneScale(graphics_) : 1.0f;
    const VkExtent2D extent{
        std::max(1u, static_cast<std::uint32_t>(static_cast<float>(target.extent.width) * scale)),
        std::max(1u, static_cast<std::uint32_t>(static_cast<float>(target.extent.height) * scale))};
    Slot& sized = slots_.at(slot);
    Image& image = sized.scene;
    // Seen as UNORM too where the target is, for the effects (EffectPass::GammaTarget).
    const VkFormat unorm = target.unormView != VK_NULL_HANDLE ? target.unormFormat : VK_FORMAT_UNDEFINED;
    if (image.get() == VK_NULL_HANDLE || image.extent().width != extent.width
        || image.extent().height != extent.height) {
        sized.sceneUnorm = {};
        image = Image(context_->allocator(), extent, colorFormat_,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, 1, unorm != VK_FORMAT_UNDEFINED ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
        if (unorm != VK_FORMAT_UNDEFINED) {
            const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image.get(),
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = unorm,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            VkImageView view = VK_NULL_HANDLE;
            vkCheck(vkCreateImageView(context_->device(), &viewInfo, nullptr, &view), "vkCreateImageView");
            sized.sceneUnorm = UniqueImageView(context_->device(), view);
        }
    }
    return {.image = image.get(),
            .view = image.view(),
            .extent = extent,
            .finalLayout = target.finalLayout,
            .unormView = sized.sceneUnorm.get(),
            .unormFormat = sized.sceneUnorm ? unorm : VK_FORMAT_UNDEFINED};
}

void MeshRenderer::cull(Frame& frame)
{
    const MeshData* lastKey = nullptr;
    GpuMesh* lastMesh = nullptr;
    std::optional<MaterialKey> lastMaterial;
    for (std::uint32_t i = 0; i < frame.items.size(); ++i) {
        const DrawItem& item = frame.items[i];
        const MeshData* key = item.mesh ? item.mesh.get() : primitiveMesh(Primitive::Block).get();
        GpuMesh* mesh = key == lastKey ? lastMesh : findMesh(item.mesh, frame.uploads);
        lastKey = key;
        lastMesh = mesh;
        if (!mesh)
            continue;
        mesh->lastUsed = frame_;

        const float minTransparency = item.decal ? 0.0f : materialLook(item.material).minTransparency;
        const float alpha = 1.0f - std::max(item.transparency, minTransparency);
        if (alpha <= 0.0f)
            continue;

        // Every texture in use gets queued for upload (or kept), as sharp as
        // the item is big on screen.
        bool waiting = false;
        const Frame::Placement placement = frame.place(item, *mesh);
        const bool maps = graphics_.physicalShading && (item.normalMap || item.roughnessMap || item.metalnessMap);
        if (item.texture || maps) {
            const float texels = frame.texelsAcross(item, placement);
            textures_->use(item.texture, true, &waiting, texels);
            if (maps) {
                textures_->use(item.normalMap, false, nullptr, texels);
                textures_->use(item.roughnessMap, false, nullptr, texels);
                textures_->use(item.metalnessMap, false, nullptr, texels);
            }
        }
        // The material's maps are queued here and looked up for good once
        // they're uploaded (mapsFor).
        if (const MaterialKey material{item.material, item.materialVariant};
            materialTextures_ && material != lastMaterial && plainPart(item)) {
            lastMaterial = material;
            if (frame.materialMaps.try_emplace(material).second) {
                const MaterialTextures found = materialTextures_(item.material, item.materialVariant);
                textures_->use(found.color, true, nullptr, TextureCache::fullSize);
                textures_->use(found.normal, false, nullptr, TextureCache::fullSize);
            }
        }
        // A see-through texture would draw solid until it arrives.
        if (waiting && item.alphaMode == TextureAlpha::Transparency) {
            frame.deferred.push_back({i, mesh, alpha, placement});
            continue;
        }
        frame.classify(i, mesh, alpha, placement);
    }
}

void MeshRenderer::collectEffects(Frame& frame)
{
    // The effect sources' geometry; its textures are queued with the items'.
    effectGeometry_.clear();
    if (effectSources_.empty())
        return;
    const EffectView effectView{frame.camera, frame.viewProjection, frame.lighting.time, drawDistance_,
                                frame.pixelsPerStud};
    for (EffectSource* source : effectSources_)
        source->collect(effectView, effectGeometry_);
    for (const EffectBatch& batch : effectGeometry_.batches) {
        if (!batch.texture)
            continue;
        const float distance = std::max((batch.center - frame.eye).magnitude() - batch.radius, frame.camera.nearPlane);
        textures_->use(batch.texture, true, nullptr, 2.0f * batch.radius * frame.pixelsPerStud / distance);
    }
}

void MeshRenderer::uploadTextures(Frame& frame)
{
    // Tiled everywhere (and the sky all around): kept as sharp as allowed.
    constexpr float full = TextureCache::fullSize;
    for (int i = 0; i < 2 && surfaceTextures_; ++i)
        textures_->use(surfaceTextures_(i == 0 ? Enum::SurfaceType::Studs : Enum::SurfaceType::Inlet), true, nullptr,
                       full);
    const bool anyTerrain
        = std::ranges::any_of(frame.items, [](const DrawItem& item) { return item.terrain || item.terrainWater; });
    if (terrainMaterials_ && anyTerrain) {
        for (std::size_t i = 0; i < frame.terrainLayers.size(); ++i) {
            TerrainLayer& layer = frame.terrainLayers[i];
            layer = terrainMaterials_(i);
            textures_->use(layer.color, true, nullptr, full);
            textures_->use(layer.normal, false, nullptr, full);
            for (const ImagePtr& normalFrame : layer.normalFrames)
                textures_->use(normalFrame, false, nullptr, full);
        }
    }
    for (const ImagePtr& face : frame.lighting.skybox.faces)
        textures_->use(face, true, nullptr, full);
    textures_->use(frame.lighting.skybox.sun, true, nullptr, full);
    textures_->use(frame.lighting.skybox.moon, true, nullptr, full);
    textures_->upload(frame.cmd, [&](VkDeviceSize size) -> Buffer& { return stagingBuffer(frame.s, size); });
    // After upload()'s own barriers: this pass's number then carries the wait
    // on the staging the copies were read from, which is part of its cost.
    markPass(frame.cmd, GpuPass::TextureUpload);
    textures_->updateDescriptors(frame.s.descriptors, 2, frame.slot);
    for (int i = 0; i < 2 && surfaceTextures_; ++i)
        frame.surfaceTextures[i]
            = textures_->use(surfaceTextures_(i == 0 ? Enum::SurfaceType::Studs : Enum::SurfaceType::Inlet), true);
    for (const Frame::Deferred& d : frame.deferred) {
        bool waiting = false;
        textures_->use(frame.items[d.item].texture, true, &waiting);
        if (!waiting)
            frame.classify(d.item, d.mesh, d.alpha, d.placement);
    }
    frame.materialMaps.clear(); // filled again as instances are written, now with what got uploaded
}

void MeshRenderer::orderEffects(Frame& frame)
{
    // Effect batches, like the transparent items, farthest first, to be
    // drawn in among them.
    std::vector<std::uint32_t>& order = frame.effectOrder;
    order.resize(effectGeometry_.batches.size());
    frame.effectDepth.resize(order.size());
    for (std::uint32_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::vector<float> depths(order.size());
    // Those on top after everything else, among themselves farthest first.
    constexpr float onTop = std::numeric_limits<float>::max();
    for (std::uint32_t i = 0; i < depths.size(); ++i) {
        const EffectBatch& batch = effectGeometry_.batches[i];
        const float depth = frame.view.transformPoint(batch.center).z;
        depths[i] = batch.onTop ? onTop / 2.0f + depth : depth;
    }
    std::ranges::stable_sort(order, std::less{}, [&](std::uint32_t i) { return depths[i]; });
    // A batch whose texture is still on its way waits for it rather than
    // drawing a white square.
    std::vector<std::uint32_t> textures(order.size());
    std::uint32_t kept = 0;
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        const ImagePtr& texture = effectGeometry_.batches[order[i]].texture;
        const std::uint32_t index = textures_->use(texture, true);
        if (texture && index == 0)
            continue;
        order[kept] = order[i];
        frame.effectDepth[kept] = depths[order[i]];
        textures[kept++] = index;
    }
    order.resize(kept);
    frame.effectDepth.resize(kept);
    textures.resize(kept);
    effects_->write(frame.slot, effectGeometry_, order, textures);
    stats_.effectBatches = static_cast<std::uint32_t>(order.size());
    stats_.effectTriangles = effects_->triangles(frame.slot);
}

void MeshRenderer::writeInstances(Frame& frame)
{
    Slot& s = frame.s;
    frame.visibleCount = frame.solid[0].size() + frame.solid[1].size() + frame.decals.size()
                         + frame.layerDecals.size() + frame.transparent.size();
    frame.casterCount = frame.casters[0].size() + frame.casters[1].size();
    stats_.visible = static_cast<std::uint32_t>(frame.visibleCount);
    stats_.shadowCasters = static_cast<std::uint32_t>(frame.casterCount);
    reserveInstances(s, frame.visibleCount);
    reserveStream(context_->allocator(), s.shadowInstances, s.shadowCapacity, frame.casterCount,
                  sizeof(ShadowInstance));

    std::byte* instances = frame.visibleCount > 0 ? s.instances.mapped().data() : nullptr;
    std::byte* shades = frame.visibleCount > 0 ? s.shades.mapped().data() : nullptr;
    auto write = [&](std::uint32_t at, const Frame::Visible& v) {
        const DrawItem& item = frame.items[v.item];
        const MaterialLook look = materialLook(item.material);
        // Without physically based shading, its maps are left out too.
        const bool physical
            = graphics_.physicalShading && (item.physical || item.normalMap || item.roughnessMap || item.metalnessMap);
        const bool isPlain = plainPart(item);
        const MaterialMaps material = isPlain ? frame.mapsFor({item.material, item.materialVariant}, materialTextures_,
                                                              *textures_)
                                              : MaterialMaps{};
        const Color3 tint = item.texture ? Color3{toLinear(item.textureColor.r), toLinear(item.textureColor.g),
                                                  toLinear(item.textureColor.b)}
                                         : Color3{1.0f, 1.0f, 1.0f};
        const std::uint32_t flags = static_cast<std::uint32_t>(item.alphaMode) | (item.clampUv ? flagClamp : 0u)
                                    | (physical ? flagPhysical : 0u) | (item.decal ? flagDecal : 0u)
                                    | (material.tintMask ? flagTintMask : 0u) | (item.terrain ? flagTerrain : 0u)
                                    | (item.terrainWater ? flagWater : 0u);
        const Instance instance{
            item.model,
            {toLinear(item.color.r) * look.tint, toLinear(item.color.g) * look.tint, toLinear(item.color.b) * look.tint,
             v.alpha},
        };
        std::memcpy(instances + std::size_t{at} * sizeof(Instance), &instance, sizeof(Instance));
        Shade shade{
            {look.specular, look.shininess, look.emissive, item.reflectance},
            {isPlain ? static_cast<float>(look.pattern) : 0.0f, look.patternScale, look.patternStrength,
             material.studsPerTile},
            {tint.r, tint.g, tint.b, 0.0f},
            {textures_->use(item.texture, true), physical ? textures_->use(item.normalMap, false) : 0u,
             physical ? textures_->use(item.roughnessMap, false) : 0u,
             physical ? textures_->use(item.metalnessMap, false) : 0u},
            {material.color, material.normal, flags, item.surfaces},
            {item.uvScale[0], item.uvScale[1], item.uvOffset[0], item.uvOffset[1]},
        };
        if (material.normal != 0)
            std::ranges::copy(rotationOf(item.model), shade.rotation);
        if (item.terrain) {
            for (std::size_t i = 0; i < item.terrainPalette.size(); ++i)
                shade.maps[i / 4] |= std::uint32_t{item.terrainPalette[i]} << (8 * (i % 4));
            shade.maps[2] = shade.maps[3] = 0;
            shade.uvTransform[0] = item.dither[0];
            shade.uvTransform[1] = item.dither[1];
        }
        std::memcpy(shades + std::size_t{at} * sizeof(Shade), &shade, sizeof(Shade));
    };
    for (std::size_t k = 0; k < 2; ++k) {
        for (const Frame::Visible& v : frame.solid[k]) {
            Frame::Draw& draw = frame.draws[k][v.draw];
            const std::uint32_t at = draw.first + draw.count++;
            write(at, v);
            frame.noteHighlight(at, v);
        }
    }
    for (std::uint32_t i = 0; i < frame.decals.size(); ++i)
        write(frame.decalFirst + i, frame.decals[i]);
    for (std::uint32_t i = 0; i < frame.layerDecals.size(); ++i)
        write(frame.layerFirst + i, frame.layerDecals[i]);
    for (std::uint32_t i = 0; i < frame.transparent.size(); ++i) {
        write(frame.transparentFirst + i, frame.transparent[i]);
        frame.noteHighlight(frame.transparentFirst + i, frame.transparent[i]);
    }

    std::byte* casterInstances = frame.casterCount == 0 ? nullptr : s.shadowInstances.mapped().data();
    for (std::size_t k = 0; k < 2; ++k) {
        for (const Frame::Caster& caster : frame.casters[k]) {
            Frame::Draw& draw = frame.shadowDraws[k][caster.draw];
            const DrawItem& item = frame.items[caster.item];
            const ShadowInstance instance{
                item.model,
                {item.uvScale[0], item.uvScale[1], item.uvOffset[0], item.uvOffset[1]},
                k == 1 ? textures_->use(item.texture, true) : 0,
            };
            std::memcpy(casterInstances + std::size_t{draw.first + draw.count++} * sizeof(ShadowInstance), &instance,
                        sizeof(ShadowInstance));
        }
    }
}

void MeshRenderer::fillUniforms(Frame& frame)
{
    const Camera& camera = frame.camera;
    const Lighting& lighting = frame.lighting;
    const SunShadow& sunShadow = frame.sunShadow;
    FrameUniforms& uniforms = frame.uniforms;
    uniforms = {
        .viewProjection = frame.viewProjection,
        .cameraPosition = {camera.cframe.position.x, camera.cframe.position.y, camera.cframe.position.z, 0.0f},
        .sunDirection = {lighting.sunDirection.x, lighting.sunDirection.y, lighting.sunDirection.z, 0.0f},
        .sunColor = {lighting.sunColor.r, lighting.sunColor.g, lighting.sunColor.b, 0.0f},
        .ambient = {lighting.ambient.r, lighting.ambient.g, lighting.ambient.b, lighting.exposure},
        .skyAmbient = {lighting.skyAmbient.r, lighting.skyAmbient.g, lighting.skyAmbient.b, 0.0f},
        .fogRange = {lighting.fogStart, lighting.fogEnd, 0.0f, 0.0f},
        .shadowViewProjection = sunShadow.viewProjection,
        .shadow = {frame.shadows ? 1.0f : 0.0f, 1.0f / static_cast<float>(sunShadow.texels),
                   1.5f * 2.0f * sunShadow.radius / static_cast<float>(sunShadow.texels), lighting.shadowSoftness},
        .backColor = {lighting.backColor.r, lighting.backColor.g, lighting.backColor.b, 0.0f},
        .skyGradientTop = {lighting.skyGradientTop.r, lighting.skyGradientTop.g, lighting.skyGradientTop.b,
                           lighting.environmentDiffuse},
        .skyGradientBottom = {lighting.skyGradientBottom.r, lighting.skyGradientBottom.g, lighting.skyGradientBottom.b,
                              lighting.environmentSpecular},
    };
    putLinear(uniforms.sky, lighting.sky);
    putLinear(uniforms.fogColor, lighting.fogColor);
    uniforms.surfaceTextures[0] = frame.surfaceTextures[0];
    uniforms.surfaceTextures[1] = frame.surfaceTextures[1];
    fillSky(uniforms, camera, frame.aspect, lighting);
    std::ranges::copy(
        std::array{camera.nearPlane, camera.farPlane, static_cast<float>(std::fmod(lighting.time, 3600.0)), 0.0f},
        uniforms.depthParams);
    // With post effects, ColorCorrection goes on after them (as the client's
    // does), so the scene is drawn without it.
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            uniforms.colorCorrection[row][column]
                = frame.post ? (row == column ? 1.0f : 0.0f) : lighting.colorCorrection[row][column];
    fillTerrainLayers(uniforms, frame.terrainLayers, lighting.time);
    uniforms.quality[0] = graphics_.sharpReflections ? 1.0f : 0.0f;
    uniforms.quality[1] = frame.rayShadows ? 1.0f : 0.0f;
    uniforms.quality[2] = rays_ ? 1.0f : 0.0f;
    uniforms.quality[3] = rays_ && graphics_.reflectEverything ? 1.0f : 0.0f;
    uniforms.occlusion[0] = rays_ && graphics_.ambientOcclusion ? 1.0f : 0.0f;
    uniforms.occlusion[1] = ambientOcclusionStuds;
    uniforms.tone[0] = lighting.retroTone ? 1.0f : 0.0f;
    uniforms.tone[1] = graphics_.potato ? 1.0f : 0.0f;
    // A decal is plastic (DrawItem's default material), lit as such.
    const MaterialLook decalLook = materialLook(Enum::Material::Plastic);
    uniforms.decals[0] = frame.layerDecals.empty() ? 0.0f : 1.0f;
    uniforms.decals[1] = decalLook.specular;
    uniforms.decals[2] = decalLook.shininess;
}

void MeshRenderer::fillTerrainLayers(FrameUniforms& uniforms, std::span<const TerrainLayer> layers, double time)
{
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const TerrainLayer& layer = layers[i];
        FrameUniforms::TerrainLayer& out = uniforms.terrainLayers[i];
        putLinear(out.color, layer.tint, layer.tintMask ? 1.0f : 0.0f);
        out.maps[0] = textures_->use(layer.color, true);
        out.maps[1] = textures_->use(layer.normal, false);
        out.tiling[0] = 1.0f / std::max(layer.studsPerTileTop, 0.01f);
        out.tiling[1] = 1.0f / std::max(layer.studsPerTileSide, 0.01f);
        out.tiling[2] = i == 1 ? layer.waveStrength : layer.dominance; // see waterSurface in mesh.frag
        out.tiling[3] = layer.edgeNoise;
        if (i == 1)
            out.color[3] = layer.reflectance;
        const float range = std::max(layer.blendMax - layer.blendMin, 1e-3f);
        out.blend[0] = 1.0f / range;
        out.blend[1] = -layer.blendMin / range;
        out.blend[2] = layer.blendScale;
        out.blend[3] = 1.0f / std::max(layer.noiseStuds, 0.01f);
        if (i == 1) {
            // Water: the ripple map frame showing and the next, blended by
            // blend.x; the swell's phase and height in blend.yz.
            const std::size_t frames = layer.normalFrames.size();
            const double played = time * static_cast<double>(layer.frameRate);
            if (frames > 1) {
                const auto at = static_cast<std::size_t>(std::fmod(played, static_cast<double>(frames)));
                out.maps[1] = textures_->use(layer.normalFrames[at], false);
                out.maps[2] = textures_->use(layer.normalFrames[(at + 1) % frames], false);
            } else {
                out.maps[2] = out.maps[1];
            }
            out.blend[0] = static_cast<float>(played - std::floor(played));
            out.blend[1]
                = static_cast<float>(std::fmod(time * static_cast<double>(layer.waveSpeed), 2.0 * std::numbers::pi));
            out.blend[2] = layer.waveHeight;
        }
    }
}

void MeshRenderer::planLocalLights(Frame& frame)
{
    Slot& s = frame.s;
    // Local lights per pixel near the camera with Future lighting (the
    // client's DefaultUnified*FS with LightRenderGPU), and ray traced with
    // any technology, its shadows traced; elsewhere they light through the
    // light map alone, as they do with Future on a preset without
    // pixelLights. Only those that reach closer than where the per-pixel
    // light has faded into the light map are kept.
    const bool future = frame.lighting.technology == Enum::Technology::Future
                        || frame.lighting.technology == Enum::Technology::Unified;
    const bool perPixel = ((future && graphics_.pixelLights) || rays_) && !lights_.empty();
    std::vector<LocalLight> near;
    if (perPixel) {
        const float faded = graphics_.shadowDistance + localLightBlend;
        for (const LocalLight& light : lights_)
            if ((light.position - frame.eye).magnitude() - light.reach() < faded)
                near.push_back(light);
        // The nearest first, when there are more than the buffer takes.
        if (near.size() > maxPixelLights) {
            std::ranges::nth_element(near, near.begin() + maxPixelLights, {}, [&](const LocalLight& light) {
                return (light.position - frame.eye).magnitude() - light.reach();
            });
            near.resize(maxPixelLights);
        }
    }
    clusters_.build(near, frame.camera, frame.scene.extent.width, frame.scene.extent.height);

    std::vector<int> shadowAt(near.size(), -1);
    const auto shadowed = placeLocalShadows(near, frame.eye, future && !rays_ && graphics_.shadows);
    if (s.localShadowSignatures.size() != atlasPlaces) {
        s.localShadowSignatures.assign(atlasPlaces, 0);
        s.localShadowInputs.assign(atlasPlaces, 0);
    }
    frame.planLocalShadows(near, shadowed, shadowAt);
    stats_.shadowedLights = static_cast<std::uint32_t>(frame.localShadowed.size());
    // The atlas at full size the first time it's needed, before anything
    // this frame binds the descriptors that point at it.
    if (!frame.localShadowed.empty() && s.localShadows.extent().width != localShadowAtlas) {
        s.localShadows = Image(context_->allocator(), {localShadowAtlas, localShadowAtlas}, shadowFormat,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT);
        writeLocalShadowDescriptors(s);
        s.localShadowsReady = false;
    }
    // Ray traced, the shadows of lights with Shadows are traced (the
    // client's shadow maps and light grid stop only theirs).
    writeLights(s, near, rays_ != nullptr, shadowAt);
    stats_.pixelLights = static_cast<std::uint32_t>(near.size());
    stats_.clusteredLights = static_cast<std::uint32_t>(clusters_.indices.size());
    FrameUniforms& uniforms = frame.uniforms;
    std::ranges::copy(std::array{static_cast<float>(near.size()), near.empty() ? 0.0f : 1.0f,
                                 graphics_.shadowDistance, 1.0f / localLightBlend},
                      uniforms.localLights);
    std::ranges::copy(std::array{static_cast<float>(clusters_.tilesX), static_cast<float>(clusters_.tilesY),
                                 static_cast<float>(LightClusters::tilePixels),
                                 static_cast<float>(LightClusters::slices)},
                      uniforms.lightClusters);
    uniforms.lightMap[1] = localLightCap;
    uniforms.lightMap[2] = rays_ && !near.empty() ? 1.0f : 0.0f;
}

std::vector<std::pair<std::size_t, std::uint32_t>>
MeshRenderer::placeLocalShadows(std::span<const LocalLight> near, Vector3 eye, bool shadowing)
{
    // Future lighting's shadows, not ray traced: the nearest point and spot
    // lights with Shadows get cube maps, as many as the atlas holds (the
    // client's LightShadowGPU and its atlas). GUESS: SurfaceLights get none,
    // as the client gives them no shadow map object [code: 0x10486df4f]. A
    // light keeps its place in the atlas while it stays shadowed, and its
    // tiles are drawn again only when it or what it shadows changed.
    std::vector<std::size_t> shadowed;
    if (shadowing) {
        for (std::size_t i = 0; i < near.size(); ++i)
            if (near[i].shadows && near[i].kind != LocalLight::Kind::Surface)
                shadowed.push_back(i);
        std::ranges::sort(shadowed, {},
                          [&](std::size_t i) { return (near[i].position - eye).magnitude() - near[i].reach(); });
        shadowed.resize(std::min<std::size_t>(shadowed.size(), atlasPlaces));
    }
    std::unordered_map<const lunar::Instance*, std::uint32_t> kept;
    std::vector<bool> taken(atlasPlaces, false);
    for (std::size_t i : shadowed)
        if (auto found = localShadowPlaces_.find(near[i].key); found != localShadowPlaces_.end()) {
            kept.emplace(near[i].key, found->second);
            taken[found->second] = true;
        }
    std::uint32_t free = 0;
    for (std::size_t i : shadowed)
        if (!kept.contains(near[i].key)) {
            while (taken[free])
                ++free;
            taken[free] = true;
            kept.emplace(near[i].key, free);
        }
    localShadowPlaces_ = std::move(kept);
    std::vector<std::pair<std::size_t, std::uint32_t>> placed;
    placed.reserve(shadowed.size());
    for (std::size_t i : shadowed)
        placed.emplace_back(i, localShadowPlaces_.at(near[i].key));
    return placed;
}

void MeshRenderer::buildRayScene(Frame& frame)
{
    Slot& s = frame.s;
    // The frame's top level: everything solid whose mesh has a bottom level,
    // and what mesh.frag needs of each (RayInstance): its mesh, for the
    // alpha test of cut-outs and the UVs and normals of reflection hits, and
    // its surface.
    struct RayInstance {
        VkDeviceAddress vertices = 0; // Vertex[]
        VkDeviceAddress indices = 0;  // the full level's
        std::uint32_t texture = 0;
        std::uint32_t flags = 0;    // rayAlphaTested, rayTerrain
        float uvTransform[4]{};     // scale, offset
        float color[4]{};           // linear, times the material's tint; w: emissive
        std::uint32_t palette[2]{}; // terrain: material ids, a byte each
    };
    static_assert(sizeof(RayInstance) == 64);
    constexpr std::uint32_t rayAlphaTested = 1;
    constexpr std::uint32_t rayTerrain = 2;
    // Ray masks: sun shadow rays see casters, reflection rays everything.
    constexpr std::uint8_t maskCasts = 1;
    constexpr std::uint8_t maskReflected = 2;
    constexpr std::uint8_t maskLocalCasts = 4; // local lights' shadow rays, with or without the sun's
    // What can close a room in for the sky's rays (mesh.frag's skyRayOut):
    // solid shadow casters 4 studs across or more, so a character's own limbs
    // or a prop don't (GUESS: the size).
    constexpr std::uint8_t maskEncloses = 8;
    constexpr float enclosingStuds = 4.0f;
    std::vector<RayScene::Instance> topInstances;
    std::vector<RayInstance> described;
    topInstances.reserve(frame.rayItems.size());
    described.reserve(frame.rayItems.size());
    for (const Frame::RayItem& ray : frame.rayItems) {
        const VkDeviceAddress bottom = ray.mesh->rayBottom;
        if (bottom == 0)
            continue;
        const DrawItem& item = frame.items[ray.item];
        // Dithered terrain cuts holes only to fade levels of detail: solid to rays.
        const bool alphaTested = ray.cutout && item.texture;
        const Vector3 half = boxHalfExtent(item.model, ray.mesh->halfExtent);
        const bool encloses
            = !ray.cutout && item.castShadow && std::max({half.x, half.y, half.z}) * 2.0f >= enclosingStuds;
        // Terrain is a surface with no inside: local lights' rays see both
        // its sides.
        topInstances.push_back(
            {item.model, bottom, static_cast<std::uint32_t>(described.size()), !alphaTested,
             static_cast<std::uint8_t>(maskReflected | (ray.casts ? maskCasts : 0)
                                       | (item.castShadow ? maskLocalCasts : 0) | (encloses ? maskEncloses : 0)),
             !item.terrain});
        RayInstance& out = described.emplace_back();
        out.vertices = ray.mesh->rayVertices;
        out.indices = ray.mesh->rayIndices;
        out.texture = item.texture ? textures_->use(item.texture, true) : 0;
        out.flags = (alphaTested ? rayAlphaTested : 0u) | (item.terrain ? rayTerrain : 0u);
        out.uvTransform[0] = item.uvScale[0];
        out.uvTransform[1] = item.uvScale[1];
        out.uvTransform[2] = item.uvOffset[0];
        out.uvTransform[3] = item.uvOffset[1];
        const MaterialLook look = materialLook(item.material);
        out.color[0] = toLinearApprox(item.color.r) * look.tint;
        out.color[1] = toLinearApprox(item.color.g) * look.tint;
        out.color[2] = toLinearApprox(item.color.b) * look.tint;
        out.color[3] = look.emissive;
        if (item.terrain)
            for (std::size_t i = 0; i < item.terrainPalette.size(); ++i)
                out.palette[i / 4] |= std::uint32_t{item.terrainPalette[i]} << (8 * (i % 4));
    }
    const auto needed = static_cast<std::uint32_t>(std::max<std::size_t>(described.size(), 1));
    if (needed > s.rayCapacity) {
        s.rayCapacity = std::bit_ceil(std::max(needed, 1024u));
        s.rayInstances = Buffer(context_->allocator(), std::size_t{s.rayCapacity} * sizeof(RayInstance),
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryUsage::Upload);
    }
    if (!described.empty())
        std::memcpy(s.rayInstances.mapped().data(), described.data(), described.size() * sizeof(RayInstance));
    rays_->buildTop(frame.cmd, frame.slot, topInstances, s.rayScratch);

    VkAccelerationStructureKHR top = rays_->top(frame.slot);
    const VkWriteDescriptorSetAccelerationStructureKHR topInfo{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &top,
    };
    const VkDescriptorBufferInfo instanceInfo{s.rayInstances.get(), 0, VK_WHOLE_SIZE};
    const VkWriteDescriptorSet writes[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = &topInfo,
            .dstSet = s.descriptors,
            .dstBinding = 7,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = s.descriptors,
            .dstBinding = 8,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &instanceInfo,
        },
    };
    vkUpdateDescriptorSets(context_->device(), 2, writes, 0, nullptr);
}

void MeshRenderer::recordSunShadow(Frame& frame)
{
    const VkCommandBuffer cmd = frame.cmd;
    Slot& s = frame.s;
    // The shadow map always gets cleared, so the main pass can sample it
    // whether or not anything casts.
    imageBarrier(cmd, {
                          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .srcAccessMask = VK_ACCESS_2_NONE,
                          .dstStageMask = depthStages,
                          .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                           | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .image = s.shadowMap.get(),
                          .subresourceRange = depthRange,
                      });
    const VkRenderingAttachmentInfo shadowDepth{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = s.shadowMap.view(),
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.depthStencil = {1.0f, 0}},
    };
    const VkExtent2D shadowExtent{frame.sunShadow.texels, frame.sunShadow.texels};
    const VkRenderingInfo shadowRendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, shadowExtent},
        .layerCount = 1,
        .pDepthAttachment = &shadowDepth,
    };
    vkCmdBeginRendering(cmd, &shadowRendering);
    if (frame.casterCount > 0) {
        const VkViewport viewport{
            0.0f, 0.0f, static_cast<float>(shadowExtent.width), static_cast<float>(shadowExtent.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, shadowExtent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &s.descriptors, 0, nullptr);
        // Unused by the sun's casters, but their shader declares it.
        const Mat4 unused;
        vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &unused);
        VkBuffer casterBuffer = s.shadowInstances.get();
        vkCmdBindVertexBuffers(cmd, 1, 1, &casterBuffer, &zeroOffset);
        const VkPipeline shadowPipelines[2] = {shadow_.get(), shadowCutout_.get()};
        for (std::size_t k = 0; k < 2; ++k) {
            if (frame.shadowDraws[k].empty())
                continue;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelines[k]);
            for (const Frame::Draw& draw : frame.shadowDraws[k])
                frame.drawMesh(*draw.mesh, draw.level, draw.count, draw.first);
        }
    }
    vkCmdEndRendering(cmd);
    imageBarrier(cmd, {
                          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                          .srcStageMask = depthStages,
                          .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                          .image = s.shadowMap.get(),
                          .subresourceRange = depthRange,
                      });
    // The sun's map is cleared and recorded every frame whatever the lights
    // ask for, so this pass always runs and always gets a stamp: after the
    // barrier the main pass's shaders will sample it through.
    markPass(cmd, GpuPass::SunShadow);
}

void MeshRenderer::recordLocalShadows(Frame& frame)
{
    const VkCommandBuffer cmd = frame.cmd;
    Slot& s = frame.s;
    const std::vector<Frame::LocalShadow>& localShadowed = frame.localShadowed;
    // Shadowed local lights' cube maps: the casters each face sees into its
    // tile of the atlas, at full detail, for the lights whose tiles aren't
    // as they should be already.
    const bool redrawing = std::ranges::any_of(localShadowed, &Frame::LocalShadow::redraw);
    if (redrawing) {
        struct FaceDraw {
            GpuMesh* mesh;
            std::uint32_t first;
            std::uint32_t count;
            bool cutout;
        };
        std::vector<std::array<std::vector<FaceDraw>, 6>> faceDraws(localShadowed.size());
        std::size_t instanceCount = 0;
        for (const Frame::LocalShadow& light : localShadowed)
            if (light.redraw)
                for (const auto& face : light.casters)
                    instanceCount += face.size();
        reserveStream(context_->allocator(), s.localShadowInstances, s.localShadowInstanceCapacity,
                      std::max<std::size_t>(instanceCount, 1), sizeof(ShadowInstance));
        std::byte* out = s.localShadowInstances.mapped().data();
        std::uint32_t next = 0;
        for (std::size_t k = 0; k < localShadowed.size(); ++k) {
            if (!localShadowed[k].redraw)
                continue;
            for (std::size_t f = 0; f < 6; ++f) {
                std::vector<FaceDraw>& draws = faceDraws[k][f];
                for (std::uint32_t c : localShadowed[k].casters[f]) {
                    const Frame::LocalCaster& caster = frame.localCasters[c];
                    const DrawItem& item = frame.items[caster.item];
                    if (draws.empty() || draws.back().mesh != caster.mesh || draws.back().cutout != caster.cutout)
                        draws.push_back({caster.mesh, next, 0, caster.cutout});
                    ++draws.back().count;
                    const ShadowInstance instance{
                        item.model,
                        {item.uvScale[0], item.uvScale[1], item.uvOffset[0], item.uvOffset[1]},
                        caster.cutout ? textures_->use(item.texture, true) : 0,
                    };
                    std::memcpy(out + std::size_t{next++} * sizeof(ShadowInstance), &instance,
                                sizeof(ShadowInstance));
                }
            }
        }
        stats_.localShadowCasters = next;
        // What's there is kept for the lights that didn't change.
        const bool keep = s.localShadowsReady;
        imageBarrier(cmd, {
                              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                              .dstStageMask = depthStages,
                              .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                               | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                              .oldLayout = keep ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                              .image = s.localShadows.get(),
                              .subresourceRange = depthRange,
                          });
        const VkRenderingAttachmentInfo atlasDepth{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = s.localShadows.view(),
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .loadOp = keep ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.depthStencil = {1.0f, 0}},
        };
        const VkRenderingInfo atlasRendering{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, {localShadowAtlas, localShadowAtlas}},
            .layerCount = 1,
            .pDepthAttachment = &atlasDepth,
        };
        vkCmdBeginRendering(cmd, &atlasRendering);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &s.descriptors, 0, nullptr);
        VkBuffer instanceBuffer = s.localShadowInstances.get();
        vkCmdBindVertexBuffers(cmd, 1, 1, &instanceBuffer, &zeroOffset);
        const std::uint32_t perRow = localShadowAtlas / localShadowTile;
        for (std::size_t k = 0; k < localShadowed.size(); ++k) {
            if (!localShadowed[k].redraw)
                continue;
            for (std::uint32_t f = 0; f < 6; ++f) {
                const std::uint32_t tile = 6 * localShadowed[k].place + f;
                const VkRect2D area{{static_cast<std::int32_t>((tile % perRow) * localShadowTile),
                                     static_cast<std::int32_t>((tile / perRow) * localShadowTile)},
                                    {localShadowTile, localShadowTile}};
                if (keep) {
                    const VkClearAttachment clear{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                                  .clearValue = {.depthStencil = {1.0f, 0}}};
                    const VkClearRect rect{area, 0, 1};
                    vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
                }
                if (faceDraws[k][f].empty())
                    continue;
                const VkViewport viewport{static_cast<float>(area.offset.x), static_cast<float>(area.offset.y),
                                          static_cast<float>(localShadowTile), static_cast<float>(localShadowTile),
                                          0.0f, 1.0f};
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &area);
                vkCmdPushConstants(cmd, layout_.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4),
                                   &localShadowed[k].faces[f]);
                std::optional<bool> bound;
                for (const FaceDraw& draw : faceDraws[k][f]) {
                    if (bound != draw.cutout) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          draw.cutout ? localShadowCutout_.get() : localShadow_.get());
                        bound = draw.cutout;
                    }
                    frame.drawMesh(*draw.mesh, 0, draw.count, draw.first, false);
                }
            }
        }
        vkCmdEndRendering(cmd);
        imageBarrier(cmd, {
                              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                              .srcStageMask = depthStages,
                              .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                              .newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                              .image = s.localShadows.get(),
                              .subresourceRange = depthRange,
                          });
        s.localShadowsReady = true;
    } else if (!s.localShadowsReady) {
        // Bound but unused: it only needs the layout the descriptor says.
        imageBarrier(cmd, {
                              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                              .srcAccessMask = VK_ACCESS_2_NONE,
                              .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                              .image = s.localShadows.get(),
                              .subresourceRange = depthRange,
                          });
        s.localShadowsReady = true;
    }
    // Both ways through above end with the atlas in the layout the main pass
    // samples it in, so the stamp is the same either side of the branch.
    markPass(cmd, GpuPass::LocalShadows);
}

void MeshRenderer::bindScene(const Frame& frame)
{
    vkCmdSetViewport(frame.cmd, 0, 1, &frame.viewport);
    vkCmdSetScissor(frame.cmd, 0, 1, &frame.scissor);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &frame.s.descriptors, 0,
                            nullptr);
    if (frame.visibleCount > 0) {
        VkBuffer instanceBuffer = frame.s.instances.get();
        vkCmdBindVertexBuffers(frame.cmd, 1, 1, &instanceBuffer, &zeroOffset);
    }
}

void MeshRenderer::recordSolid(Frame& frame)
{
    const VkCommandBuffer cmd = frame.cmd;
    Slot& s = frame.s;
    const RenderTarget& scene = frame.scene;
    imageBarrier(cmd,
                 {
                     .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                     .srcAccessMask = VK_ACCESS_2_NONE,
                     .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     .image = scene.image,
                     .subresourceRange = colorRange,
                 });
    imageBarrier(cmd, {
                          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                          .srcStageMask = depthStages,
                          .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstStageMask = depthStages,
                          .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                           | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .image = s.depth.get(),
                          .subresourceRange = depthRange,
                      });
    // Water reads the scene's depth from its own copy, which every draw's
    // descriptors name, so it's always in the layout they say.
    imageBarrier(cmd, {
                          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .srcAccessMask = VK_ACCESS_2_NONE,
                          .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                          .image = s.sceneDepth.get(),
                          .subresourceRange = depthRange,
                      });
    // Ray traced, the opaque pass and the decals also hand the denoiser what
    // the local lights' shared shadow ray found (mesh.frag's DENOISE outputs).
    if (rays_)
        for (const Image* image : {&s.rest, &s.restSeen, &s.restGuide})
            imageBarrier(cmd, {
                                  .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                                  | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_NONE,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  .image = image->get(),
                                  .subresourceRange = colorRange,
                              });

    frame.color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = scene.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {{frame.uniforms.sky[0], frame.uniforms.sky[1], frame.uniforms.sky[2], 1.0f}}},
    };
    if (frame.lighting.transparentBackground)
        frame.color.clearValue = {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}};
    frame.depth = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = s.depth.view(),
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, // water copies it mid-frame
        .clearValue = {.depthStencil = {0.0f, 0}},
    };
    frame.rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, scene.extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &frame.color,
        .pDepthAttachment = &frame.depth,
    };
    frame.solidColors[0] = frame.color;
    frame.solidRendering = frame.rendering;
    if (rays_) {
        const VkImageView views[3] = {s.rest.view(), s.restSeen.view(), s.restGuide.view()};
        for (std::size_t i = 0; i < 3; ++i)
            frame.solidColors[i + 1] = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = views[i],
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}},
            };
        frame.solidRendering.colorAttachmentCount = 4;
        frame.solidRendering.pColorAttachments = frame.solidColors;
    }
    frame.viewport = {
        0.0f, 0.0f, static_cast<float>(scene.extent.width), static_cast<float>(scene.extent.height), 0.0f, 1.0f};
    frame.scissor = {{0, 0}, scene.extent};

    // With decals on solid parts, the opaque items' depth goes first in a
    // scope of its own, the decal layer is drawn over it in another, and the
    // solid scope goes on from that depth with the layer to read. Without,
    // the pre-pass opens the solid scope and the layer is only bound.
    if (!frame.layerDecals.empty())
        recordDecalLayer(frame);
    else {
        // Bound but unused: it only needs the layout the descriptor says.
        imageBarrier(cmd, {
                              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              .srcAccessMask = VK_ACCESS_2_NONE,
                              .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              .image = s.decalLayer.get(),
                              .subresourceRange = colorRange,
                          });
        vkCmdBeginRendering(cmd, &frame.solidRendering);
        bindScene(frame);
        // The opaque items' depth first; their pass below shades each pixel once.
        if (!frame.solid[0].empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depthPrepass_.get());
            for (const Frame::Draw& draw : frame.draws[0])
                frame.drawMesh(*draw.mesh, draw.level, draw.count, draw.first, false);
        }
        // After the depth pre-pass, and after the layer that wasn't drawn.
        // The stamps are outside the branches above (which drew nothing when
        // there was nothing to draw) so the indices stay in step with
        // GpuPass whatever the frame had in it.
        markPass(cmd, GpuPass::Depth);
        markPass(cmd, GpuPass::DecalLayer);
    }
    if (frame.visibleCount > 0) {
        const VkPipeline solidPipelines[2][2]
            = {{opaque_.get(), opaqueTerrain_.get()}, {cutout_.get(), cutoutTerrain_.get()}};
        for (std::size_t k = 0; k < 2; ++k) {
            if (frame.solid[k].empty())
                continue;
            int bound = -1;
            for (const Frame::Draw& draw : frame.draws[k]) {
                if (int{draw.terrain} != bound) {
                    bound = int{draw.terrain};
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, solidPipelines[k][bound]);
                }
                frame.drawMesh(*draw.mesh, draw.level, draw.count, draw.first);
            }
        }
    }
    // After the opaque and cut-out items have shaded what survived the depth.
    markPass(cmd, GpuPass::Opaque);
    // The sky goes wherever the solid geometry left the far plane, before
    // anything see-through is blended over it. It is a fullscreen draw every
    // frame, so it is only skipped where nothing is to be behind the scene.
    if (!frame.lighting.transparentBackground) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rays_ ? skyDenoise_.get() : sky_.get());
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    markPass(cmd, GpuPass::Sky);
    if (frame.visibleCount > 0 && !frame.decals.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, decal_.get());
        frame.drawRuns(frame.decals, frame.decalFirst);
    }
    // After the decals, which this scene may have none of.
    markPass(cmd, GpuPass::Decals);
}

void MeshRenderer::recordDecalLayer(Frame& frame)
{
    const VkCommandBuffer cmd = frame.cmd;
    Slot& s = frame.s;
    const VkRenderingInfo depthRendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, frame.scene.extent},
        .layerCount = 1,
        .pDepthAttachment = &frame.depth,
    };
    vkCmdBeginRendering(cmd, &depthRendering);
    bindScene(frame);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depthOnly_.get());
    for (const Frame::Draw& draw : frame.draws[0])
        frame.drawMesh(*draw.mesh, draw.level, draw.count, draw.first, false);
    vkCmdEndRendering(cmd);
    markPass(cmd, GpuPass::Depth);

    const VkImageMemoryBarrier2 toLayer[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = depthStages,
            .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = depthStages,
            .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .image = s.depth.get(),
            .subresourceRange = depthRange,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = s.decalLayer.get(),
            .subresourceRange = colorRange,
        },
    };
    const VkDependencyInfo toLayerInfo{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers = toLayer,
    };
    vkCmdPipelineBarrier2(cmd, &toLayerInfo);

    const VkRenderingAttachmentInfo layerColor{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = s.decalLayer.view(),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}},
    };
    frame.depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    const VkRenderingInfo layerRendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, frame.scene.extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &layerColor,
        .pDepthAttachment = &frame.depth,
    };
    vkCmdBeginRendering(cmd, &layerRendering);
    bindScene(frame);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, decalLayer_.get());
    // Neighbours in zIndex order that share a mesh (and level) go in one draw.
    frame.drawRuns(frame.layerDecals, frame.layerFirst);
    vkCmdEndRendering(cmd);

    // The layer to the opaque pass's shader; the depth to the passes after
    // it, the cut-outs' writes among them.
    const VkImageMemoryBarrier2 fromLayer[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = depthStages,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = depthStages,
            .dstAccessMask
            = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .image = s.depth.get(),
            .subresourceRange = depthRange,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = s.decalLayer.get(),
            .subresourceRange = colorRange,
        },
    };
    const VkDependencyInfo fromLayerInfo{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers = fromLayer,
    };
    vkCmdPipelineBarrier2(cmd, &fromLayerInfo);
    markPass(cmd, GpuPass::DecalLayer);
    vkCmdBeginRendering(cmd, &frame.solidRendering);
    bindScene(frame);
}

void MeshRenderer::recordDenoise(Frame& frame)
{
    if (rays_) {
        const VkCommandBuffer cmd = frame.cmd;
        Slot& s = frame.s;
        // The local lights' shared shadow ray, denoised: the solid scope
        // ends, the blur runs its passes over what it left, and the rest of
        // the frame goes on in the same colour and depth with the composite
        // first, before anything see-through is blended over it.
        vkCmdEndRendering(cmd);
        std::vector<VkImageMemoryBarrier2> between;
        for (const Image* image : {&s.rest, &s.restSeen, &s.restGuide})
            between.push_back({
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = image->get(),
                .subresourceRange = colorRange,
            });
        for (const Image& image : s.restVisibility)
            between.push_back({
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .image = image.get(),
                .subresourceRange = colorRange,
            });
        between.push_back({
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = frame.scene.image,
            .subresourceRange = colorRange,
        });
        between.push_back({
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = depthStages,
            .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = depthStages,
            .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .image = s.depth.get(),
            .subresourceRange = depthRange,
        });
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = static_cast<std::uint32_t>(between.size()),
            .pImageMemoryBarriers = between.data(),
        };
        vkCmdPipelineBarrier2(cmd, &dependency);
        if (denoise_) {
            // What each pixel's ray found, blurred with steps 1, 2, 4 and 8
            // pixels back and forth (the first reading the rays themselves)
            // into [1], [0], [1] and [0]: the last lands in [0], which the
            // composite reads through the other set.
            for (std::size_t pass = 0; pass < denoiseBlur_.size(); ++pass) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoiseBlur_[pass].get());
                const VkDescriptorSet set = s.denoiseSets[1 - pass % 2];
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoiseLayout_.get(), 0, 1, &set, 0,
                                        nullptr);
                vkCmdDispatch(cmd, (frame.scene.extent.width + 15) / 16, (frame.scene.extent.height + 15) / 16, 1);
                const VkMemoryBarrier2 written{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                };
                const VkDependencyInfo afterPass{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .memoryBarrierCount = 1,
                    .pMemoryBarriers = &written,
                };
                vkCmdPipelineBarrier2(cmd, &afterPass);
            }
        }
        frame.color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        frame.depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        vkCmdBeginRendering(cmd, &frame.rendering);
        vkCmdSetViewport(cmd, 0, 1, &frame.viewport);
        vkCmdSetScissor(cmd, 0, 1, &frame.scissor);
        if (denoise_) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, denoiseLayout_.get(), 0, 1,
                                    &s.denoiseSets[1], 0, nullptr);
            for (const UniquePipeline& composite : denoiseComposite_) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite.get());
                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_.get(), 0, 1, &s.descriptors, 0,
                                nullptr);
        if (frame.visibleCount > 0) {
            VkBuffer instanceBuffer = s.instances.get();
            vkCmdBindVertexBuffers(cmd, 1, 1, &instanceBuffer, &zeroOffset);
        }
    }
    // After the denoiser, which only a ray traced frame has.
    markPass(frame.cmd, GpuPass::Denoise);
}

void MeshRenderer::recordTransparent(Frame& frame)
{
    const VkCommandBuffer cmd = frame.cmd;
    const std::vector<Frame::Visible>& transparent = frame.transparent;
    const std::uint32_t transparentFirst = frame.transparentFirst;
    if (frame.visibleCount > 0) {
        std::vector<std::uint32_t> water; // indices into transparent
        for (std::uint32_t i = 0; i < transparent.size(); ++i)
            if (frame.items[transparent[i].item].terrainWater)
                water.push_back(i);
        if (!water.empty()) {
            // Water hides what's behind it by the studs of water in front,
            // so it needs the depth so far: end the pass, copy the depth,
            // and go on drawing into the same attachments.
            vkCmdEndRendering(cmd);
            copySceneDepth(cmd, frame.s, frame.scene.extent);
            frame.color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            frame.depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            vkCmdBeginRendering(cmd, &frame.rendering);
            bindScene(frame);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterDepth_.get());
            for (std::uint32_t i : water)
                frame.drawMesh(*transparent[i].mesh, transparent[i].level, 1, transparentFirst + i);
            // Then the water itself, which hides whatever see-through
            // things are below it; those above blend over it after.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, water_.get());
            for (std::uint32_t i : water)
                frame.drawMesh(*transparent[i].mesh, transparent[i].level, 1, transparentFirst + i);
        }
    }
    // After the water (and the depth copy it needs, which stays adjacent to
    // the end and begin of the rendering scope around it).
    markPass(cmd, GpuPass::Water);
    // The rest of the transparent items, farthest first, with the effect
    // batches in among them: neighbours that share a mesh (and level) go in
    // one draw, and so do runs of effect batches. Water's done.
    const std::vector<std::uint32_t>& effectOrder = frame.effectOrder;
    const std::vector<float>& effectDepth = frame.effectDepth;
    std::uint32_t nextEffect = 0;
    bool transparentBound = false;
    // Effects blend in sRGB space through the scene's UNORM view, like the
    // client's; going back to the scene picks up what's there.
    frame.color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    frame.depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    const EffectPass::GammaTarget gammaTarget{frame.scene.unormView, frame.scene.unormFormat, &frame.rendering};
    auto drawEffectsBehind = [&](float behind) {
        std::uint32_t end = nextEffect;
        while (end < effectOrder.size() && effectDepth[end] < behind)
            ++end;
        if (end == nextEffect)
            return;
        effects_->draw(cmd, frame.slot, nextEffect, end - nextEffect, &gammaTarget);
        nextEffect = end;
        transparentBound = false;
    };
    for (std::uint32_t i = 0; i < transparent.size();) {
        if (frame.items[transparent[i].item].terrainWater) {
            ++i;
            continue;
        }
        drawEffectsBehind(transparent[i].depth);
        if (!transparentBound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, transparent_.get());
            transparentBound = true;
        }
        std::uint32_t end = i + 1;
        while (end < transparent.size() && transparent[end].mesh == transparent[i].mesh
               && transparent[end].level == transparent[i].level && !frame.items[transparent[end].item].terrainWater
               && (nextEffect == effectOrder.size() || effectDepth[nextEffect] >= transparent[end].depth))
            ++end;
        frame.drawMesh(*transparent[i].mesh, transparent[i].level, end - i, transparentFirst + i);
        i = end;
    }
    // After the transparent items, whose draws are mixed with the effect
    // batches in among them.
    markPass(cmd, GpuPass::Transparent);
    drawEffectsBehind(std::numeric_limits<float>::infinity());
    // After the effect batches in front of everything.
    markPass(cmd, GpuPass::Effects);

    vkCmdEndRendering(cmd);
}

void MeshRenderer::recordPost(Frame& frame)
{
    const VkCommandBuffer cmd = frame.cmd;
    stats_.postEffects = frame.post;
    if (frame.post) {
        // The scene (and its depth, for sun rays and depth of field) read
        // by the post effects, which draw onto the target.
        imageBarrier(cmd, {
                              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              .image = frame.scene.image,
                              .subresourceRange = colorRange,
                          });
        const bool depthRead = PostPass::needsDepth(frame.lighting.post);
        if (depthRead)
            copySceneDepth(cmd, frame.s, frame.scene.extent);
        post_->record(cmd, frame.slot,
                      {
                          .scene = frame.scene.view,
                          .sceneExtent = frame.scene.extent,
                          .depth = depthRead ? frame.s.sceneDepth.view() : VK_NULL_HANDLE,
                          .target = frame.target.image,
                          .targetView = frame.target.view,
                          .targetFormat = colorFormat_,
                          .targetExtent = frame.target.extent,
                          .camera = &frame.camera,
                          .lighting = &frame.lighting,
                          .rayScene = rays_ ? rays_->top(frame.slot) : VK_NULL_HANDLE,
                      });
    }
    // After the effects and the barriers that fed them, or after the last of
    // the scene's draws when the frame asked for none.
    markPass(cmd, GpuPass::Post);
}

void MeshRenderer::recordUpscale(Frame& frame)
{
    const VkCommandBuffer cmd = frame.cmd;
    const RenderTarget& scene = frame.scene;
    const RenderTarget& target = frame.target;
    if (!frame.post && frame.scaled) {
        // The scene onto the target, filtered up; the overlay then draws at full size.
        const VkImageMemoryBarrier2 toBlit[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .image = scene.image,
                .subresourceRange = colorRange,
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                .srcAccessMask = VK_ACCESS_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .image = target.image,
                .subresourceRange = colorRange,
            },
        };
        const VkDependencyInfo blitDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = toBlit};
        vkCmdPipelineBarrier2(cmd, &blitDependency);
        const VkImageBlit region{
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets
            = {{0, 0, 0},
               {static_cast<std::int32_t>(scene.extent.width), static_cast<std::int32_t>(scene.extent.height), 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets
            = {{0, 0, 0},
               {static_cast<std::int32_t>(target.extent.width), static_cast<std::int32_t>(target.extent.height), 1}},
        };
        // Potato's pixels stay blocks.
        vkCmdBlitImage(cmd, scene.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                       graphics_.potato ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        imageBarrier(
            cmd, {
                     .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     .srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                     .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     .image = target.image,
                     .subresourceRange = colorRange,
                 });
    }
    // The upscale blit, or nothing at all (with the effects on, or with the
    // scene drawn at the target's own size). Never between the barriers above
    // and the blit they feed.
    markPass(cmd, GpuPass::Upscale);
}

void MeshRenderer::recordOverlay(Frame& frame)
{
    const VkCommandBuffer cmd = frame.cmd;
    Slot& s = frame.s;
    const RenderTarget& target = frame.target;
    if (overlay_) {
        // The overlay draws over what the scene wrote.
        imageBarrier(
            cmd, {
                     .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                     .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     .image = target.image,
                     .subresourceRange = colorRange,
                 });
        // It may test against the scene's depth.
        imageBarrier(cmd, {
                              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                              .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                                              | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                              .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                              .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                              .image = s.depth.get(),
                              .subresourceRange = depthRange,
                          });
        RenderTarget world = target;
        if (!frame.scaled) {
            world.depthView = s.depth.view();
            world.depthFormat = depthFormat;
        } else if (depthBlit_) {
            world.depthView = blitOverlayDepth(frame);
            world.depthFormat = depthFormat;
        }
        world.viewProjection = frame.viewProjection;
        overlay_->record(cmd, frame.slot, world);
    }
    // After the 2D UI, which most frames have none of. The frame's last stamp
    // inside record() is the readback's, which the caller writes after it.
    markPass(cmd, GpuPass::Overlay);
}

VkImageView MeshRenderer::blitOverlayDepth(Frame& frame)
{
    // A scaled scene's depth is smaller than the target: blitted up to its
    // size, a texel to a block of pixels, so billboards and surface GUIs are
    // still hidden by what's in front of them (they drew through every wall
    // without it).
    const VkCommandBuffer cmd = frame.cmd;
    Slot& s = frame.s;
    const VkExtent2D sceneExtent = frame.scene.extent;
    const VkExtent2D targetExtent = frame.target.extent;
    Image& depthUp = s.overlayDepth;
    if (depthUp.get() == VK_NULL_HANDLE || depthUp.extent().width != targetExtent.width
        || depthUp.extent().height != targetExtent.height)
        depthUp = Image(context_->allocator(), targetExtent, depthFormat,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT);
    const VkImageMemoryBarrier2 toCopy[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = depthStages,
            .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image = s.depth.get(),
            .subresourceRange = depthRange,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = depthStages,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = depthUp.get(),
            .subresourceRange = depthRange,
        },
    };
    const VkDependencyInfo copyDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers = toCopy,
    };
    vkCmdPipelineBarrier2(cmd, &copyDependency);
    const VkImageBlit region{
        .srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0},
                       {static_cast<std::int32_t>(sceneExtent.width), static_cast<std::int32_t>(sceneExtent.height), 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0},
                       {static_cast<std::int32_t>(targetExtent.width), static_cast<std::int32_t>(targetExtent.height),
                        1}},
    };
    vkCmdBlitImage(cmd, s.depth.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depthUp.get(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    imageBarrier(cmd, {
                          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dstStageMask = depthStages,
                          .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .image = depthUp.get(),
                          .subresourceRange = depthRange,
                      });
    return depthUp.view();
}

void MeshRenderer::releaseUnused()
{
    // Meshes nothing drew for a while go; every frame that could still be
    // reading them finished long ago.
    std::erase_if(meshes_, [&](const auto& entry) {
        if (entry.second.lastUsed + keepUnusedFrames >= frame_)
            return false;
        if (rays_)
            rays_->forget(entry.first);
        return true;
    });
    VkDeviceSize meshBytes = 0;
    for (const auto& [key, mesh] : meshes_)
        meshBytes += mesh.bytes;
    if (meshBytes > currentMeshBudget_) {
        // Over budget (a camera flying over streamed terrain): the longest
        // unused go first, as soon as no frame in flight can read them.
        std::vector<std::tuple<std::uint64_t, VkDeviceSize, const MeshData*>> unused; // last used, bytes
        for (const auto& [key, mesh] : meshes_)
            if (mesh.lastUsed + slots_.size() < frame_)
                unused.emplace_back(mesh.lastUsed, mesh.bytes, key);
        std::ranges::sort(unused);
        for (const auto& [lastUsed, bytes, key] : unused) {
            if (meshBytes <= currentMeshBudget_)
                break;
            meshBytes -= bytes;
            if (rays_)
                rays_->forget(key);
            meshes_.erase(key);
        }
    }
    stats_.meshBytes = meshBytes;
    textures_->collect();
    stats_.deviceBytes = context_->allocator().allocatedBytes();
    stats_.deviceLocalBytes = context_->allocator().deviceLocalBytes();
    stats_.textureBytes = textures_->residentBytes();
    stats_.textureBudget = textures_->budget();
    stats_.texturesResized = textures_->resizedLastFrame();
    stats_.texturesPending = static_cast<std::uint32_t>(textures_->pending());
    stats_.textureQuality = textures_->quality();
    stats_.outOfMemory = textures_->outOfMemoryCount() + meshOutOfMemory_;
    std::erase_if(skyAverages_, [&](const auto& entry) { return entry.second.lastUsed + keepUnusedFrames < frame_; });
    stats_.meshesResident = static_cast<std::uint32_t>(meshes_.size());
    stats_.texturesResident = static_cast<std::uint32_t>(textures_->resident());
    if (rays_) {
        // What this frame's upload() and buildTop() above asked the driver
        // to build, taken now so they count once each.
        const RayScene::Stats rays = rays_->takeStats();
        stats_.blasBuilds = rays.bottomBuilds;
        stats_.tlasBuilds = rays.topBuilds;
        stats_.tlasRefits = rays.topRefits;
        stats_.rayInstances = rays.topInstances;
    }
}

} // namespace lunar::render

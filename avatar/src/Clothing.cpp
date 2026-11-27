#include <lunar/avatar/Clothing.hpp>

#include <lunar/assets/MeshFile.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <thread>
#include <utility>
#include <vector>

namespace lunar::avatar {

namespace {

constexpr std::string_view compositePrefix = "rbxlunar-composite://";

struct Group {
    std::string_view name;
    std::uint32_t width;
    std::uint32_t height;
    std::string_view layout;
    bool shirt;
    bool pants;
    std::initializer_list<std::string_view> parts;
};

const std::array<Group, 5>& groups()
{
    static const std::array<Group, 5> table{{
        {"torso", 388, 272, "R15CompositTorsoBase", true, true, {"UpperTorso", "LowerTorso"}},
        {"leftarm", 264, 284, "R15CompositLeftArmBase", true, false, {"LeftUpperArm", "LeftLowerArm", "LeftHand"}},
        {"rightarm", 264, 284, "R15CompositRightArmBase", true, false, {"RightUpperArm", "RightLowerArm", "RightHand"}},
        {"leftleg", 264, 284, "R15CompositLeftArmBase", false, true, {"LeftUpperLeg", "LeftLowerLeg", "LeftFoot"}},
        {"rightleg", 264, 284, "R15CompositRightArmBase", false, true, {"RightUpperLeg", "RightLowerLeg", "RightFoot"}},
    }};
    return table;
}

std::array<float, 4> sample(const Image& image, float u, float v)
{
    // Nearest texel, clamped.
    const auto x = static_cast<std::uint32_t>(
        std::clamp(u * static_cast<float>(image.width), 0.0f, static_cast<float>(image.width - 1)));
    const auto y = static_cast<std::uint32_t>(
        std::clamp(v * static_cast<float>(image.height), 0.0f, static_cast<float>(image.height - 1)));
    const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 4;
    return {static_cast<float>(image.rgba[at]), static_cast<float>(image.rgba[at + 1]),
            static_cast<float>(image.rgba[at + 2]), static_cast<float>(image.rgba[at + 3]) / 255.0f};
}

void blend(Image& target, std::uint32_t x, std::uint32_t y, const std::array<float, 4>& color)
{
    const float a = color[3];
    if (a <= 0.0f)
        return;
    // std::lround of what's never negative here, without the libm call: a
    // float plus a half is exact in a double, so this rounds the same way.
    auto round = [](float v) { return static_cast<long>(static_cast<double>(v) + 0.5); };
    const std::size_t at = (static_cast<std::size_t>(y) * target.width + x) * 4;
    const float below = static_cast<float>(target.rgba[at + 3]) / 255.0f;
    const float out = a + below * (1.0f - a);
    for (std::size_t c = 0; c < 3; ++c) {
        const float mixed
            = (color[c] * a + static_cast<float>(target.rgba[at + c]) * below * (1.0f - a)) / std::max(out, 1e-6f);
        target.rgba[at + c] = static_cast<std::uint8_t>(std::clamp(round(mixed), 0L, 255L));
    }
    target.rgba[at + 3] = static_cast<std::uint8_t>(std::clamp(round(out * 255.0f), 0L, 255L));
}

// The layout's triangles drawn into the layout rows [rowBegin, rowEnd)
// (layout Y, which runs up the image), in the layout's order.
void drawRows(Image& target, const MeshData& layout, const Image& source, std::uint32_t rowBegin, std::uint32_t rowEnd)
{
    for (std::size_t i = 0; i + 2 < layout.indices.size(); i += 3) {
        const MeshVertex& a = layout.vertices[layout.indices[i]];
        const MeshVertex& b = layout.vertices[layout.indices[i + 1]];
        const MeshVertex& c = layout.vertices[layout.indices[i + 2]];
        const float area = (b.position.x - a.position.x) * (c.position.y - a.position.y)
                           - (c.position.x - a.position.x) * (b.position.y - a.position.y);
        if (std::abs(area) < 1e-9f)
            continue;
        const float minX = std::max(0.0f, std::floor(std::min({a.position.x, b.position.x, c.position.x})));
        const float maxX = std::min(static_cast<float>(target.width) - 1.0f,
                                    std::ceil(std::max({a.position.x, b.position.x, c.position.x})));
        const float minY = std::max(0.0f, std::floor(std::min({a.position.y, b.position.y, c.position.y})));
        const float maxY = std::min(static_cast<float>(target.height) - 1.0f,
                                    std::ceil(std::max({a.position.y, b.position.y, c.position.y})));
        if (maxX < minX || maxY < minY)
            continue;
        const auto x0 = static_cast<std::uint32_t>(minX);
        const auto x1 = static_cast<std::uint32_t>(maxX);
        const auto y0 = std::max(static_cast<std::uint32_t>(minY), rowBegin);
        const auto y1 = std::min(static_cast<std::uint32_t>(maxY) + 1, rowEnd);
        for (std::uint32_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = x0; x <= x1; ++x) {
                // Pixel centres, barycentric weights.
                const float px = static_cast<float>(x) + 0.5f;
                const float py = static_cast<float>(y) + 0.5f;
                const float wa
                    = ((b.position.x - px) * (c.position.y - py) - (c.position.x - px) * (b.position.y - py)) / area;
                const float wb
                    = ((c.position.x - px) * (a.position.y - py) - (a.position.x - px) * (c.position.y - py)) / area;
                const float wc = 1.0f - wa - wb;
                constexpr float edge = -1e-4f;
                if (wa < edge || wb < edge || wc < edge)
                    continue;
                const float u = wa * a.u + wb * b.u + wc * c.u;
                const float v = wa * a.v + wb * b.v + wc * c.v;
                // Layout Y runs up the texture; image rows run down.
                blend(target, x, target.height - 1 - y, sample(source, u, v));
            }
        }
    }
}

} // namespace

// A pixel only ever blends the triangles over it in the layout's order, so
// bands of rows go to threads of their own and come out as they would on
// one: a character's 1024x512 R6 atlas took 14 ms on the frame that
// dressed it.
void drawComposite(Image& target, const MeshData& layout, const Image& source)
{
    if (target.empty() || source.empty())
        return;
    constexpr std::uint32_t rowsPerBand = 32;
    const std::uint32_t bands = (target.height + rowsPerBand - 1) / rowsPerBand;
    const std::uint32_t threads = std::min({std::max(std::thread::hardware_concurrency() / 2, 1u), 8u, bands});
    if (threads <= 1 || layout.indices.size() < 64) {
        drawRows(target, layout, source, 0, target.height);
        return;
    }
    std::atomic<std::uint32_t> next{0};
    auto run = [&] {
        for (std::uint32_t band = next.fetch_add(1); band < bands; band = next.fetch_add(1))
            drawRows(target, layout, source, band * rowsPerBand, std::min((band + 1) * rowsPerBand, target.height));
    };
    std::vector<std::jthread> pool;
    pool.reserve(threads - 1);
    for (std::uint32_t t = 1; t < threads; ++t)
        pool.emplace_back(run);
    run();
}

ClothingCompositor::ClothingCompositor(std::shared_ptr<assets::AssetCache> cache)
    : cache_(std::move(cache)), textures_(cache_, 2)
{
}

std::shared_ptr<const MeshData> ClothingCompositor::layout(std::string_view name)
{
    std::scoped_lock lock(mutex_);
    auto& slot = layouts_[std::string(name)];
    if (!slot) {
        auto bytes = cache_->fetch(std::format("rbxasset://avatar/compositing/{}.mesh", name));
        if (!bytes)
            return nullptr;
        auto file = assets::parseMesh(**bytes);
        if (!file)
            return nullptr;
        slot = std::make_shared<const MeshData>(std::move(file->mesh));
    }
    return slot;
}

bool ClothingCompositor::apply(Model& character)
{
    std::optional<Dressing> dressing = prepare(character);
    if (!dressing)
        return false;
    composite(*dressing);
    return finish(character, *dressing);
}

std::optional<ClothingCompositor::Dressing> ClothingCompositor::prepare(const Model& character)
{
    const auto* humanoid = dynamic_cast<const Humanoid*>(character.findFirstChildOfClass("Humanoid"));
    if (!humanoid)
        return std::nullopt;
    const auto* shirt = dynamic_cast<const Shirt*>(character.findFirstChildOfClass("Shirt"));
    const auto* pants = dynamic_cast<const Pants*>(character.findFirstChildOfClass("Pants"));
    Dressing dressing;
    dressing.r6 = humanoid->rigType() == Enum::HumanoidRigType::R6;
    if (dressing.r6) {
        const auto* tshirt = dynamic_cast<const ShirtGraphic*>(character.findFirstChildOfClass("ShirtGraphic"));
        dressing.templates
            = {pants ? pants->pantsTemplate() : std::string(), shirt ? shirt->shirtTemplate() : std::string(),
               tshirt ? tshirt->graphic() : std::string()};
    } else {
        dressing.templates = {shirt ? shirt->shirtTemplate() : std::string(),
                              pants ? pants->pantsTemplate() : std::string(), std::string()};
    }
    return dressing;
}

void ClothingCompositor::composite(Dressing& dressing)
{
    dressing.loaded = false;
    dressing.images.clear();
    if (dressing.r6) {
        // Bottom to top, the order of the layouts' depths: pants, shirt, t-shirt.
        static constexpr std::array<std::string_view, 3> layouts{"CompositPantsTemplate", "CompositShirtTemplate",
                                                                 "CompositTShirt"};
        auto atlas = std::make_shared<Image>();
        atlas->width = 1024;
        atlas->height = 512;
        atlas->rgba.assign(atlas->byteSize(), 0);
        bool drew = false;
        for (std::size_t i = 0; i < layouts.size(); ++i) {
            const std::string& templateId = dressing.templates[i];
            if (templateId.empty())
                continue;
            auto loaded = textures_.load(templateId);
            std::shared_ptr<const MeshData> mesh = layout(layouts[i]);
            if (!loaded || !mesh)
                return;
            drawComposite(*atlas, *mesh, **loaded);
            (*loaded)->rgba.evict(); // a template mapped from a file needn't stay in memory once drawn
            drew = true;
        }
        if (!drew)
            return;
        dressing.loaded = true;
        dressing.images.push_back(std::move(atlas));
        return;
    }
    std::shared_ptr<const Image> shirtImage;
    std::shared_ptr<const Image> pantsImage;
    if (!dressing.templates[0].empty()) {
        auto loaded = textures_.load(dressing.templates[0]);
        if (!loaded)
            return;
        shirtImage = *loaded;
    }
    if (!dressing.templates[1].empty()) {
        auto loaded = textures_.load(dressing.templates[1]);
        if (!loaded)
            return;
        pantsImage = *loaded;
    }
    if (!shirtImage && !pantsImage)
        return;
    dressing.loaded = true;
    // Templates mapped from a file needn't stay in memory once drawn.
    auto evict = [&] {
        for (const auto& drawn : {shirtImage, pantsImage})
            if (drawn)
                drawn->rgba.evict();
    };
    for (const Group& group : groups()) {
        std::shared_ptr<const MeshData> mesh = layout(group.layout);
        if (!mesh) {
            evict();
            return;
        }
        auto image = std::make_shared<Image>();
        image->width = group.width;
        image->height = group.height;
        image->rgba.assign(image->byteSize(), 0);
        if (group.pants && pantsImage)
            drawComposite(*image, *mesh, *pantsImage);
        if (group.shirt && shirtImage)
            drawComposite(*image, *mesh, *shirtImage);
        dressing.images.push_back(std::move(image));
    }
    evict();
}

bool ClothingCompositor::finish(Model& character, Dressing& dressing)
{
    if (!dressing.loaded)
        return false;
    if (dressing.r6) {
        auto* humanoid = dynamic_cast<Humanoid*>(character.findFirstChildOfClass("Humanoid"));
        if (!humanoid || dressing.images.empty())
            return false;
        std::string id;
        {
            std::scoped_lock lock(mutex_);
            id = std::format("{}{}/r6", compositePrefix, next_++);
            images_[id] = std::move(dressing.images.front());
        }
        humanoid->assignBodyTexture(std::move(id));
        return true;
    }
    std::uint64_t serial = 0;
    {
        std::scoped_lock lock(mutex_);
        serial = next_++;
    }
    // The groups drawn, in order; a layout that didn't load stops there.
    for (std::size_t g = 0; g < dressing.images.size(); ++g) {
        const Group& group = groups()[g];
        const std::string id = std::format("{}{}/{}", compositePrefix, serial, group.name);
        {
            std::scoped_lock lock(mutex_);
            images_[id] = std::move(dressing.images[g]);
        }
        for (std::string_view name : group.parts) {
            auto* part = dynamic_cast<MeshPart*>(character.findFirstChild(name));
            if (part && (part->textureId().empty() || part->textureId().starts_with(compositePrefix)))
                part->setTextureID(id);
        }
    }
    return dressing.images.size() == groups().size();
}

bool ClothingCompositor::ready(const Model& character)
{
    const auto* shirt = dynamic_cast<const Shirt*>(character.findFirstChildOfClass("Shirt"));
    const auto* pants = dynamic_cast<const Pants*>(character.findFirstChildOfClass("Pants"));
    const auto* tshirt = dynamic_cast<const ShirtGraphic*>(character.findFirstChildOfClass("ShirtGraphic"));
    bool ready = true;
    for (const std::string_view id : {shirt ? std::string_view(shirt->shirtTemplate()) : std::string_view(),
                                      pants ? std::string_view(pants->pantsTemplate()) : std::string_view(),
                                      tshirt ? std::string_view(tshirt->graphic()) : std::string_view()}) {
        if (id.empty())
            continue;
        textures_.request(id);
        const assets::TextureState state = textures_.state(id);
        ready = ready && (state == assets::TextureState::Ready || state == assets::TextureState::Failed);
    }
    return ready;
}

std::shared_ptr<const Image> ClothingCompositor::image(std::string_view id) const
{
    std::scoped_lock lock(mutex_);
    auto found = images_.find(std::string(id));
    return found == images_.end() ? nullptr : found->second;
}

assets::TextureProvider ClothingCompositor::provider(assets::TextureProvider next)
{
    return [this, next = std::move(next)](std::string_view id) -> std::shared_ptr<const Image> {
        if (id.starts_with(compositePrefix))
            return image(id);
        return next ? next(id) : nullptr;
    };
}

} // namespace lunar::avatar

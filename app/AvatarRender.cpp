#include "AvatarRender.hpp"

#include "PlaceScene.hpp"

#include <lunar/Classes.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/TextureLoader.hpp>
#include <lunar/avatar/AnimationSystem.hpp>
#include <lunar/avatar/Character.hpp>
#include <lunar/avatar/Clothing.hpp>
#include <lunar/render/Context.hpp>
#include <lunar/render/Image.hpp>
#include <lunar/render/Lighting.hpp>
#include <lunar/render/Offscreen.hpp>
#include <lunar/render/Scene.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <numbers>
#include <print>
#include <string>
#include <vector>

int renderAvatar(const std::filesystem::path& outPath, const AvatarRenderOptions& options)
{
    auto cache = std::make_shared<lunar::assets::AssetCache>();
    lunar::avatar::CharacterOptions characterOptions;
    characterOptions.assets = cache;
    std::vector<std::string> problems;
    if (options.avatar) {
        characterOptions.rigType = options.avatar->rigType;
        characterOptions.description = options.avatar->description.get();
        characterOptions.problems = &problems;
    }
    if (options.rig)
        characterOptions.rigType = *options.rig;
    auto character = lunar::avatar::createCharacter(characterOptions);
    if (!character) {
        std::println(stderr, "character: {}", character.error());
        return 1;
    }
    for (const std::string& problem : problems)
        std::println(stderr, "avatar: {}", problem);

    auto game = lunar::DataModel::create();
    (*character)->setParent(game->workspace());
    auto ground = std::make_shared<lunar::Part>();
    ground->setName("Baseplate");
    ground->setSize({24.0f, 1.0f, 24.0f});
    ground->setPosition({0.0f, -0.5f, 0.0f});
    ground->setColor(lunar::Color3::fromRGB(91, 154, 76));
    ground->setAnchored(true);
    ground->setParent(game->workspace());

    lunar::avatar::AnimationSystem animation(cache);
    lunar::avatar::CharacterAnimation& entry = animation.add(*character, options.anim.empty());
    if (!options.anim.empty()) {
        // One default animation, straight onto the Animator.
        const auto& sets = lunar::avatar::defaultAnimations(characterOptions.rigType);
        auto set = sets.find(options.anim);
        if (set == sets.end()) {
            std::println(stderr, "no default animation called {}", options.anim);
            return 1;
        }
        // The most likely choice (idle's weight-9 animation).
        const lunar::avatar::AnimationChoice* choice = &set->second.front();
        for (const auto& candidate : set->second)
            if (candidate.weight > choice->weight)
                choice = &candidate;
        auto clip = animation.clips().load(choice->animationId);
        if (!clip) {
            std::println(stderr, "{}", clip.error());
            return 1;
        }
        auto track = entry.animator->loadAnimation(*clip, options.anim);
        track->play(0.0f);
    } else if (options.speed) {
        entry.animate->onRunning(*options.speed);
    }
    animation.clips().wait();

    // Animate at 60 Hz up to the requested time, then once more at it.
    constexpr float frame = 1.0f / 60.0f;
    float elapsed = 0.0f;
    while (elapsed + frame <= options.time) {
        if (options.speed && entry.animate)
            entry.animate->onRunning(*options.speed);
        animation.step(frame);
        elapsed += frame;
    }
    animation.step(options.time - elapsed);

    // Shirt and pants onto the body, then draw with the composited textures.
    lunar::avatar::ClothingCompositor clothing(cache);
    clothing.apply(**character);
    lunar::assets::TextureLoader textures(cache);
    PlaceScene scene(*game);
    scene.setTextures(clothing.provider(textures.provider()), [&textures] { return textures.generation(); });
    scene.waitForMeshes();
    textures.wait();
    scene.refresh();
    const auto& items = scene.items();

    auto context = lunar::render::Context::create();
    if (!context) {
        std::println(stderr, "Vulkan: {}", context.error());
        return 1;
    }
    auto offscreen = lunar::render::OffscreenRenderer::create(**context, options.width, options.height);
    if (!offscreen) {
        std::println(stderr, "Vulkan: {}", offscreen.error());
        return 1;
    }

    // In front of the character (it faces -Z), turned by yaw, looking at the
    // middle of its bounding box.
    const auto [box, extent] = (*character)->getBoundingBox();
    const lunar::Vector3 target = box.position;
    const float yaw = options.yaw * std::numbers::pi_v<float> / 180.0f;
    const float distance = std::max(extent.y, 4.0f) * 3.2f;
    const lunar::Vector3 eye
        = target + lunar::Vector3{std::sin(yaw) * distance, distance * 0.15f, -std::cos(yaw) * distance};
    lunar::render::Camera camera;
    camera.cframe = lunar::render::Camera::lookAt(eye, target);
    camera.fieldOfView = 24.0f;
    const lunar::render::Lighting lighting = lunar::render::lightingFrom(*game);

    lunar::render::PixelImage image = (*offscreen)->render(camera, items, lighting);
    for (int i = 0; i < 64 && (*offscreen)->renderer().stats().texturesPending > 0; ++i)
        image = (*offscreen)->render(camera, items, lighting);

    const std::string encoded
        = outPath.extension() == ".ppm" ? lunar::render::encodePpm(image) : lunar::render::encodePng(image);
    std::ofstream out(outPath, std::ios::binary);
    out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!out) {
        std::println(stderr, "{}: can't write", outPath.string());
        return 1;
    }
    std::println("{} items, {}x{} -> {}", items.size(), options.width, options.height, outPath.string());
    return 0;
}

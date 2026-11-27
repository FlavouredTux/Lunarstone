#pragma once

// Classic clothing (Shirt and Pants templates) on an R15 body, composited
// the way the client does it: the content folder's avatar/compositing
// meshes are 2D layouts whose positions are pixels of a body part group's
// texture and whose UVs point into the 585x559 clothing template. Drawing
// them with the template gives a texture the body meshes' own UVs fit.
//
// Source: the composit meshes in the 0.742 content folder; how the client
// sizes its targets wasn't traced. GUESSES: the torso texture is 388x272
// and each limb's 264x284 (the extent of R15CompositTorsoBase and
// R15CompositLeftArmBase), legs reuse the arm layout with the pants
// template (the two templates share it), and the shirt goes over the pants.
//
// R6 composites one 1024x512 atlas for the torso and limbs from the
// CompositPantsTemplate, CompositShirtTemplate and CompositTShirt layouts
// (that target size is the extent of CompositFullAtlasBaseTexture); the
// R6 limb meshes in avatar/meshes map onto it. Where no clothing covers the
// atlas it stays clear, so the parts' body colours show through instead of
// being drawn in by the Composit*Base layouts.

#include <lunar/Classes.hpp>
#include <lunar/Image.hpp>
#include <lunar/Mesh.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/assets/TextureLoader.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lunar::avatar {

// Draws a composit mesh into `target`: each triangle's positions are target
// pixels, its UVs (top-down) sample `source`, blended over by alpha.
void drawComposite(Image& target, const MeshData& layout, const Image& source);

class ClothingCompositor {
public:
    explicit ClothingCompositor(std::shared_ptr<assets::AssetCache> cache);

    // Composites the character's Shirt and Pants for each R15 limb group
    // and points the group's untextured MeshParts' TextureID at the result
    // (a "rbxlunar-composite://" id this compositor resolves); for R6 the
    // Humanoid's bodyTexture gets the atlas instead. Blocks while templates
    // load. False when there's nothing to do (no Humanoid or clothing) or a
    // template failed to load.
    bool apply(Model& character);

    // apply() in three steps, for a client that can't spend a frame on it:
    // prepare reads what the character wears (on the thread that owns the
    // instances; nullopt without a Humanoid), composite loads the templates
    // and draws them (any thread, a Dressing on one at a time), and finish
    // names the images and puts them on the character (the owning thread
    // again, in the order the dressings were prepared, for the same ids).
    // apply() is the three in a row.
    struct Dressing {
        bool r6 = false;
        // R15: the shirt's and the pants' templates; R6: the layers, bottom
        // to top (pants, shirt, t-shirt).
        std::array<std::string, 3> templates;
        // composite's: whether every template (and, R6, every layout) it
        // needed loaded, and the images it drew: R15 a group's each, as far
        // as their layouts loaded; R6 the atlas.
        bool loaded = false;
        std::vector<std::shared_ptr<Image>> images;
    };
    std::optional<Dressing> prepare(const Model& character);
    void composite(Dressing& dressing);
    bool finish(Model& character, Dressing& dressing);
    // Asks for the character's templates without waiting: true once each
    // has loaded or failed, when apply() won't wait for a download. For a
    // client that dresses characters as they arrive and can't stall a frame.
    bool ready(const Model& character);

    // A texture provider that resolves the composite ids and passes every
    // other id on to `next`.
    assets::TextureProvider provider(assets::TextureProvider next);

    std::shared_ptr<const Image> image(std::string_view id) const;

private:
    std::shared_ptr<const MeshData> layout(std::string_view name);

    std::shared_ptr<assets::AssetCache> cache_;
    assets::TextureLoader textures_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const MeshData>> layouts_;
    std::unordered_map<std::string, std::shared_ptr<const Image>> images_;
    std::uint64_t next_ = 1;
};

} // namespace lunar::avatar

#pragma once

// The 3D adornments (SelectionBox, the HandleAdornments, SelectionSphere,
// SurfaceSelection) as effect geometry: flat coloured, see-through by their
// Transparency, and through everything with AlwaysOnTop, which is how ESP
// scripts box players with BoxHandleAdornments.
//
// One shows while it's Visible, has an Adornee in the Workspace, and is in
// the Workspace, a PlayerGui or CoreGui itself (Infinite Yield keeps its ESP
// in CoreGui). Handles, ArcHandles and WireframeHandleAdornment draw nothing
// yet.

#include <lunar/Instance.hpp>
#include <lunar/Math.hpp>
#include <lunar/Signal.hpp>
#include <lunar/render/EffectGeometry.hpp>
#include <lunar/render/Texture.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lunar {
class DataModel;
class GuiBase3d;
} // namespace lunar

namespace lunar::render {

// What an adornment is drawn against: where the eye is (only the faces that
// look at it are drawn, and images and lines turn to it) and the frame's
// pixels across a stud a stud away (lines' Thickness is in pixels).
struct AdornView {
    CFrame camera;
    float pixelsPerStud = 0.0f;
};

// One adornment's geometry, appended to `out` as one batch; false when
// nothing shows (not Visible, no Adornee, fully transparent). `offset` moves
// it, as an Adornee drawn ahead of physics is (PlaceScene::followParts).
// Doesn't check where the adornment is kept; Adornments does. Exposed for tests.
bool appendAdornment(const GuiBase3d& adornment, const AdornView& view, Vector3 offset, const ImagePtr& image,
                     EffectGeometry& out);

// The game's adornments, now and as they come and go (its DescendantAdded
// and DescendantRemoving), as effect geometry for MeshRenderer (addEffects).
// textures resolves ImageHandleAdornment's Image.
class Adornments : public EffectSource {
public:
    Adornments(DataModel& game, TextureProvider textures);
    ~Adornments() override;
    Adornments(const Adornments&) = delete;
    Adornments& operator=(const Adornments&) = delete;

    void setTextures(TextureProvider textures) { textures_ = std::move(textures); }
    // Adornees below `moving` are drawn shifted by `offset`, as Ribbons::follow.
    void follow(const Instance* moving, Vector3 offset)
    {
        moving_ = moving;
        offset_ = offset;
    }

    void collect(const EffectView& view, EffectGeometry& out) override;

    std::size_t count() const { return entries_.size(); }
    // What the last collect drew.
    std::size_t drawn() const { return drawn_; }

private:
    struct Entry {
        const Instance* key = nullptr;
        std::weak_ptr<const Instance> instance;
        std::string imageId; // what `image` was looked up for (ImageHandleAdornment)
        ImagePtr image;
    };
    void add(Instance* instance);
    void remove(Instance* instance);
    void drop(std::size_t i);

    DataModel& game_;
    TextureProvider textures_;
    Connection added_;
    Connection removing_;
    std::vector<Entry> entries_;
    std::unordered_map<const Instance*, std::size_t> index_;
    const Instance* moving_ = nullptr;
    Vector3 offset_;
    std::size_t drawn_ = 0;
};

} // namespace lunar::render

#pragma once

#include <lunar/Instance.hpp>
#include <lunar/Math.hpp>
#include <lunar/Signal.hpp>
#include <lunar/render/EffectGeometry.hpp>
#include <lunar/render/Texture.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace lunar {
class Attachment;
class Beam;
class Trail;
} // namespace lunar

namespace lunar::render {

// Where a Trail's two attachments have been: the ends of the segments laid so
// far, oldest first, and while it's Enabled a last point that follows the
// attachments until it's MinLength from the one before and gets laid too.
class TrailHistory {
public:
    struct Point {
        Vector3 p0;            // Attachment0's position
        Vector3 p1;            // Attachment1's
        double born = 0.0;     // seconds of game time it was laid
        float traveled = 0.0f; // studs the trail's middle had come by then (Static textures)
        bool start = false;    // no segment joins it to the point before
    };
    struct Settings {
        float lifetime = 2.0f;  // Trail.Lifetime
        float minLength = 0.1f; // Trail.MinLength
        float maxLength = 0.0f; // Trail.MaxLength, 0 for none
        bool enabled = true;
    };

    // A frame with the attachments at p0 and p1, `time` seconds in.
    void update(Vector3 p0, Vector3 p1, double time, const Settings& settings);
    // Trail:Clear, and what changing the attachments does.
    void clear();

    std::span<const Point> points() const { return points_; }
    // Whether the last point follows the attachments rather than being laid.
    bool following() const { return following_; }

private:
    // Points older than the lifetime go; the last of them is pulled towards
    // the next so the tail shrinks smoothly.
    void expire(double time, float lifetime);
    // MaxLength: the oldest go past it, the last of them cut to fit.
    void shorten(float maxLength);

    std::vector<Point> points_;
    bool following_ = false;
    bool wasEnabled_ = false;
};

// Beam's and Trail's geometry, appended to `out` as one batch; false when
// nothing shows. Exposed for tests; Ribbons calls them every frame.
struct RibbonView {
    Vector3 eye;     // FaceCamera turns ribbons towards it
    Vector3 forward; // the camera's LookVector: ZOffset moves beams against it
};
// a0 and a1 are the attachments' world CFrames; scroll is how far the texture
// has moved along (TextureSpeed's cycles so far, less SetTextureOffset's).
bool appendBeam(const Beam& beam, const CFrame& a0, const CFrame& a1, const RibbonView& view, float scroll,
                const ImagePtr& texture, EffectGeometry& out);
bool appendTrail(const Trail& trail, const TrailHistory& history, double time, const RibbonView& view,
                 const ImagePtr& texture, EffectGeometry& out);

// The Beams and Trails in the Workspace, as effect geometry for MeshRenderer
// (addEffects). Trails remember where their attachments were each frame
// they're collected in, at the frame's time (EffectView::time).
class Ribbons : public EffectSource {
public:
    // Every Beam and Trail below the workspace, now and as they come and go
    // (its DescendantAdded and DescendantRemoving). textures resolves their
    // Texture ids; until one loads the ribbon draws untextured, as the
    // client's do.
    Ribbons(Instance& workspace, TextureProvider textures);
    ~Ribbons() override;
    Ribbons(const Ribbons&) = delete;
    Ribbons& operator=(const Ribbons&) = delete;

    void setTextures(TextureProvider textures) { textures_ = std::move(textures); }
    // Attachments below `moving` are drawn shifted by `offset`, the way
    // PlaceScene::followParts draws a character between physics steps.
    void follow(const Instance* moving, Vector3 offset)
    {
        moving_ = moving;
        offset_ = offset;
    }

    void collect(const EffectView& view, EffectGeometry& out) override;

    std::size_t beamCount() const { return beams_; }
    std::size_t trailCount() const { return trails_; }
    // What the last collect drew.
    std::size_t beamsDrawn() const { return beamsDrawn_; }
    std::size_t trailsDrawn() const { return trailsDrawn_; }

private:
    struct Entry {
        const Instance* key = nullptr;
        std::weak_ptr<const Instance> instance;
        bool trail = false;
        std::string textureId; // what `texture` was looked up for
        ImagePtr texture;
        // Beams: the texture's scrolling.
        double scrolledAt = 0.0;
        float scroll = 0.0f;
        std::uint64_t offsetSets = 0;
        // Trails.
        TrailHistory history;
        const Attachment* attachment0 = nullptr;
        const Attachment* attachment1 = nullptr;
        std::uint64_t clears = 0;
        double updatedAt = 0.0;
        bool seen = false; // collected before: the times above mean something
    };
    void add(Instance* instance);
    void remove(Instance* instance);
    // Drops entries_[i], the last entry taking its place.
    void drop(std::size_t i);
    // An attachment's world CFrame as drawn: shifted along with `moving`.
    CFrame placed(const Attachment& attachment) const;
    const ImagePtr& textureOf(Entry& entry, const std::string& id);

    Instance& workspace_;
    TextureProvider textures_;
    Connection added_;
    Connection removing_;
    // In a vector for the walk every frame; found by instance through index_.
    std::vector<Entry> entries_;
    std::unordered_map<const Instance*, std::size_t> index_;
    std::size_t beams_ = 0;
    std::size_t trails_ = 0;
    std::size_t beamsDrawn_ = 0;
    std::size_t trailsDrawn_ = 0;
    const Instance* moving_ = nullptr;
    Vector3 offset_;
};

} // namespace lunar::render

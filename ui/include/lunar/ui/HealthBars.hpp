#pragma once

#include <lunar/Classes.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>
#include <lunar/ui/TextLayout.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace lunar::ui {

// What a camera sees of a box, on a screen this many pixels big: the
// rectangle its eight corners cover. `box` is the box's frame, `min` and
// `max` its corners in that frame. None when a corner is behind the camera
// (or closer than its near plane), where a rectangle would mean nothing.
// The projection is the one BillboardGuis are placed with (vertical field
// of view in degrees).
std::optional<RectF> projectBox(const CFrame& view, float fieldOfView, Vector2 screen, const CFrame& box, Vector3 min,
                                Vector3 max);

// A health bar beside a character's rectangle on the screen: the track (its
// dark outline included) just left of it and as tall as it, and the fill
// inside it from the bottom up, as tall as the fraction of health left.
struct HealthBarRects {
    RectF track;
    RectF fill; // zero height at no health
};
HealthBarRects healthBarRects(const RectF& character, float fraction, float pixelScale);

// Health over MaxHealth, 0 to 1 (0 for a MaxHealth of 0 or nonsense).
float healthFraction(float health, float maxHealth);

// Lunarstone's health bars (Settings > Health Bars, or F7): beside every
// other player's character in the world, a thin vertical bar that fills
// from the bottom by Health / MaxHealth, green through yellow to red
// (healthColor), and tracks the character as it moves, as the bar Meteor
// draws beside a player's box in Minecraft. Up close the HP shows beside
// it too.
//
// Each bar (and its number) is a world layer at the character's depth, as
// the game's own overhead BillboardGuis are: the scene's depth hides it
// behind walls. Our own character is no wall: a bar behind it on the
// screen is drawn at its front instead.
// None for our own character, a dead one (until the respawn), one behind
// the camera, off the screen, or too far to be more than a speck.
//
// The character's rectangle is its body parts' (Head, Torso, the limbs; R15's
// fifteen) as they're animated, not a box around its root part: a fighting
// game's moves throw the body well away from the root.
//
// Cheap: update() walks Players' children (never the Workspace), keeps
// each player's character, Humanoid, root and body parts by weak reference
// and looks them up again only when the character or its children change,
// projects eight corners a part and lays the HP text out again only when it
// changes.
class HealthBars {
public:
    // The HP number shows beside the bar once the character is this tall
    // on the screen (logical pixels): about 35 studs away at 1080p.
    static constexpr float numberHeight = 110.0f;
    // Characters shorter than this (logical pixels) get no bar.
    static constexpr float minHeight = 8.0f;

    HealthBars(FontLibrary& fonts, GlyphAtlas& atlas) : fonts_(&fonts), atlas_(&atlas) {}

    // This frame's bars, seen from the game's current camera on a screen
    // this big; a null game forgets them all.
    void update(DataModel* game, Vector2 screen, float pixelScale);
    // Appends them as world layers, behind whatever the scene drew in front.
    void draw(DrawList& list);

    struct Bar {
        const Instance* player = nullptr;
        RectF character; // the character's rectangle on the screen
        HealthBarRects rects;
        Color3 color;
        float fraction = 0.0f;
        Vector3 origin; // the world point whose depth it's drawn at
        bool number = false; // the HP shows too, in `label`
        RectF label;
    };
    std::span<const Bar> bars() const { return bars_; }
    // Text layouts made since it was created: the HP text is laid out
    // again only when what it says changes.
    std::uint64_t layouts() const { return layouts_; }

private:
    struct Tracked {
        Ref<Player> player;
        Ref<Model> character;
        Ref<Humanoid> humanoid;
        Ref<BasePart> root;
        // The body's parts (and, for ours, its accessories' handles), as
        // they were when the character had this many children.
        std::vector<Ref<BasePart>> parts;
        std::size_t children = static_cast<std::size_t>(-1);
        std::uint64_t seen = 0;
        // The HP text as last laid out.
        int hp = -1;
        float scale = 0.0f;
        TextLayout text;
    };

    void refresh(Tracked& tracked, Model* character, bool accessories);
    // Where the character is on the screen, from its body's animated parts
    // (from its root part when it has none); its centre in the world too.
    std::optional<RectF> characterRect(const Tracked& tracked, const Humanoid& humanoid, const BasePart& root,
                                       const CFrame& view, float fieldOfView, Vector2 screen, Vector3& centre) const;

    FontLibrary* fonts_;
    GlyphAtlas* atlas_;
    std::unordered_map<const Instance*, Tracked> tracked_;
    std::vector<Bar> bars_;
    std::uint64_t frame_ = 0;
    std::uint64_t layouts_ = 0;
};

} // namespace lunar::ui

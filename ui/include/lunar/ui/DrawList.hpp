#pragma once

#include <lunar/DataTypes.hpp>
#include <lunar/Image.hpp>
#include <lunar/Math.hpp>
#include <lunar/ui/Font.hpp>
#include <lunar/ui/TextLayout.hpp>

#include <array>
#include <cstdint>
#include <utility>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace lunar::ui {

// Straight-alpha colour in sRGB space, the space Roblox GUI colours live in.
struct Color4 {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

    static Color4 from(Color3 c, float transparency = 0.0f) { return {c.r, c.g, c.b, 1.0f - transparency}; }
    friend bool operator==(const Color4&, const Color4&) = default;
};

struct RectF {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    RectF intersect(const RectF& o) const;
    friend bool operator==(const RectF&, const RectF&) = default;
};

enum class QuadKind : std::uint8_t {
    Solid = 0,
    Image = 1, // RGBA texture times the colour
    Glyph = 2, // coverage texture as alpha
    // The rounded box, its edge blurred across uv[0] pixels (the rect is
    // that much bigger than the box, half on each side): UIShadow.
    Shadow = 3,
};

// One drawable rectangle. Everything the UI draws is a list of these,
// drawn in order with alpha blending: rounded corners, strokes, gradients,
// textures and rotation are worked out per pixel by the shader.
struct Quad {
    RectF rect;
    std::array<float, 4> uv{0.0f, 0.0f, 1.0f, 1.0f}; // u0 v0 u1 v1
    Color4 color;
    std::array<float, 4> radii{}; // corners: top-left, top-right, bottom-right, bottom-left
    float stroke = 0.0f;          // > 0: only a ring this wide inside the edge
    RectF clip{-1e9f, -1e9f, 2e9f, 2e9f};
    float rotation = 0.0f; // radians, clockwise on screen, about pivot
    float pivotX = 0.0f, pivotY = 0.0f;
    std::uint32_t texture = 0; // DrawList::textures index; 0 is the glyph atlas
    QuadKind kind = QuadKind::Solid;
    std::uint32_t gradient = 0; // DrawList::gradients index + 1, 0 for none
    float gradientRotation = 0.0f;
    float gradientOffsetX = 0.0f, gradientOffsetY = 0.0f;
    bool pixelated = false;
    bool premultiplied = false; // an image whose colour is already times its alpha, in linear light
    std::uint32_t layer = 0; // DrawList::layers index + 1; 0: on the screen
};

// Where a group of quads (Quad::layer) sits in the world instead of on the
// screen. Billboard: they're in screen pixels, as deep as `origin` (a card
// facing the camera). Surface: they're in canvas pixels, at origin + x *
// axisX + y * axisY. The renderer projects both with the scene's view.
struct WorldLayer {
    enum class Kind : std::uint8_t { Billboard, Surface };
    Kind kind = Kind::Billboard;
    Vector3 origin, axisX, axisY;
    bool depthTested = true; // false: drawn over the world (AlwaysOnTop)
    Color3 tint{1.0f, 1.0f, 1.0f};
};

// A UIGradient sampled into a ramp: colour and transparency over 0..1.
struct GradientRamp {
    static constexpr std::uint32_t width = 256;
    std::array<std::uint8_t, std::size_t{width} * 4> rgba{};

    static GradientRamp from(const ColorSequence& color, const NumberSequence& transparency);
};

// Glyph bitmaps packed into one coverage texture, rasterized on first use.
// Each glyph is keyed by face, glyph, pixel size and stroke radius. When it
// fills up, the glyphs that don't fit are left out of that frame and the
// next beginFrame() packs it again with only the glyphs the last frame drew:
// starting over in the middle of a frame would leave the text drawn before
// pointing at someone else's pixels. (BillboardGuis ask for a new text size
// whenever their distance changes; a crowd of them at 4K fills it.)
class GlyphAtlas {
public:
    static constexpr int size = 4096;

    struct Entry {
        int x = 0, y = 0, width = 0, height = 0;
        int left = 0, top = 0; // placement relative to the pen, like GlyphBitmap
    };

    GlyphAtlas();

    // Null for glyphs without pixels (spaces). strokeRadius > 0 gives the
    // glyph grown by that many pixels, for text strokes. subpixel picks one
    // of subpixelSteps horizontal offsets (glyphs are rasterized shifted by
    // subpixel / subpixelSteps of a pixel, so text spacing stays even).
    static constexpr int subpixelSteps = 4;
    const Entry* get(const FontFace* face, std::uint16_t glyph, float pixelsPerUnit, int strokeRadius = 0,
                     int subpixel = 0);

    std::span<const std::uint8_t> pixels() const { return pixels_; }
    // Rows changed since the last takeDirty(), [begin, end); empty when clean.
    std::pair<int, int> takeDirty();
    // A new frame of text: after a frame that didn't fit, keeps only what
    // the last frame drew, packed again.
    void beginFrame();
    // Bumped when the atlas was packed again; entries from before moved or are gone.
    std::uint64_t generation() const { return generation_; }
    // Glyph bitmaps rasterized since the start (the misses of get()), for LUNAR_PROFILE.
    std::uint64_t rasterized() const { return rasterized_; }
    // The time those took, and the slowest one, since the last call (LUNAR_PROFILE).
    struct RasterStats {
        double milliseconds = 0, worstMilliseconds = 0;
        float worstPixelsPerUnit = 0;
        int worstStroke = 0, worstWidth = 0, worstHeight = 0;
    };
    RasterStats takeRasterStats() { return std::exchange(rasterStats_, {}); }
    std::size_t entryCount() const { return entries_.size(); }

private:
    struct Key {
        const FontFace* face;
        std::uint16_t glyph;
        std::uint32_t size; // pixelsPerUnit * 2^20
        int stroke;
        int subpixel;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const;
    };

    struct Slot {
        std::optional<Entry> entry; // nothing for glyphs without pixels
        std::uint64_t used = 0;     // the frame that last drew it
    };

    bool allocate(int w, int h, int& x, int& y);
    void repack();

    std::vector<std::uint8_t> pixels_;
    std::unordered_map<Key, Slot, KeyHash> entries_;
    int shelfX_ = 0, shelfY_ = 0, shelfHeight_ = 0;
    int dirtyBegin_ = size, dirtyEnd_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t rasterized_ = 0;
    RasterStats rasterStats_;
    std::uint64_t frame_ = 0;
    bool full_ = false; // a glyph didn't fit this frame
};

// What a UI frame draws: quads plus the textures and gradients they use.
class DrawList {
public:
    void clear();

    void rect(const RectF& r, Color4 color, const RectF& clip);
    Quad& push(const Quad& quad)
    {
        quads.push_back(quad);
        return quads.back();
    }
    // Texture slot for an image (1-based; 0 is the glyph atlas). The same
    // image gets the same slot within a frame.
    std::uint32_t texture(const std::shared_ptr<const Image>& image);
    std::uint32_t gradient(const GradientRamp& ramp);

    // Text: strokes first (stroke is TextStrokeColor3/Transparency, alpha 0
    // for none, strokeWidth pixels wide; rich text <stroke> overrides it),
    // then the glyphs, then underlines. origin is the box's top-left.
    void text(GlyphAtlas& atlas, const TextLayout& layout, float originX, float originY, const RectF& clip,
              Color4 stroke = {0, 0, 0, 0}, float rotation = 0.0f, float pivotX = 0.0f, float pivotY = 0.0f,
              float strokeWidth = 1.0f);

    std::vector<Quad> quads;
    std::vector<std::shared_ptr<const Image>> textures; // slot i + 1
    std::vector<GradientRamp> gradients;
    std::vector<WorldLayer> layers;

private:
    std::unordered_map<const Image*, std::uint32_t> textureSlots_;
    std::unordered_multimap<std::size_t, std::uint32_t> gradientSlots_; // ramp hash -> gradients slot
};

} // namespace lunar::ui

#pragma once

#include <lunar/Classes.hpp>
#include <lunar/Tools.hpp>
#include <lunar/platform/Events.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>
#include <lunar/ui/Gui.hpp>

#include <array>
#include <memory>
#include <optional>

namespace lunar::ui {

// The classic backpack hotbar (CoreScripts BackpackScript, "Backpack Version
// 5.1"): the local player's tools in up to ten slots centred at the bottom of
// the screen (lunar::Hotbar keeps them), the held one framed in blue. Keys
// 1-9 and 0 and clicks on a slot equip and unequip;
// StarterGui:SetCoreGuiEnabled(Backpack, false) hides it and turns the keys
// off. The inventory panel (`) isn't drawn.
//
// Layout and colours are the script's constants: 60 px slots 5 px apart,
// Color3(31, 31, 31) at half transparency, a 10% edge of (90, 142, 233) round
// the equipped slot, SourceSans 14 names and numbers, tooltips 25 px above.
class BackpackGui {
public:
    static constexpr int slotCount = Hotbar::slotCount;
    static constexpr float iconSize = 60.0f;
    static constexpr float iconBuffer = 5.0f;
    static constexpr Color3 background = Color3::fromRGB(31, 31, 31);
    static constexpr float backgroundFade = 0.5f; // SLOT_FADE_LOCKED with PreferredTransparency 1
    static constexpr Color3 equipColor = Color3::fromRGB(90, 142, 233);
    static constexpr float equipThickness = 0.1f; // of the slot
    static constexpr Color3 tooltipColor{0.4f, 0.4f, 0.4f};

    BackpackGui(FontLibrary& fonts, GlyphAtlas& atlas);

    void setGame(DataModel* game);
    void setImageProvider(ImageProvider images) { images_ = std::move(images); }

    // Hotkeys and clicks on slots. Keys pass on (true only for clicks on a slot).
    bool handle(const platform::Event& event);
    void update(Vector2 screen, float pixelScale);
    void draw(DrawList& list);

    // False while StarterGui:SetCoreGuiEnabled turned the backpack off.
    bool enabled() const;
    // The tool in a slot (0-based), if any.
    Tool* slot(int index) const;
    // Where a slot is drawn this frame; nothing for empty (hidden) slots.
    std::optional<RectF> slotRect(int index) const;
    // What clicking slot `index` or pressing its key does: equip its tool,
    // or put it away when it's the one in hand.
    void select(int index);

private:
    void sync();
    Player* localPlayer() const;
    void textAt(DrawList& list, std::string_view text, RectF box, Enum::TextXAlignment x, Enum::TextYAlignment y,
                bool wrapped, const RectF& clip);

    FontLibrary* fonts_;
    GlyphAtlas* atlas_;
    ImageProvider images_;
    std::weak_ptr<Instance> game_;
    Hotbar hotbar_;
    std::array<std::optional<RectF>, slotCount> rects_{};
    std::optional<int> pressed_;
    float scale_ = 1.0f;
    float mouseX_ = -1.0f, mouseY_ = -1.0f;
};

} // namespace lunar::ui

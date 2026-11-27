#pragma once

#include <lunar/Classes.hpp>
#include <lunar/platform/Events.hpp>
#include <lunar/render/Graphics.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>
#include <lunar/ui/Gui.hpp>
#include <lunar/ui/TextEditor.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::ui {

class CoreGui;

// What the menu's Settings tab shows; the app fills it in (and keeps it
// current: F11 changes fullscreen too) and hears changes through CoreGui's
// callbacks. A setting whose callback is null shows greyed out.
struct MenuSettings {
    std::optional<render::GraphicsPreset> graphics; // none: settings no preset makes (flags), shown as Custom
    bool fullscreen = false;
    float cameraSensitivity = 1.0f; // times the mouse's turning, like UserGameSettings.MouseSensitivity
    float volume = 1.0f;            // times every sound, 0 to 1, like UserGameSettings.MasterVolume
    bool shiftLock = false;         // the Shift Lock Switch: Shift locks the mouse to the camera
    // Ray tracing is chosen for a window as it opens: the menu shows the
    // choice for the next game against what this one does.
    bool rayTracingAvailable = false; // this GPU can ray trace
    bool rayTracing = false;          // the choice
    bool rayTracingActive = false;    // this window ray traces
    bool performanceStats = false;  // the core GUI's frame rate overlay
    bool healthBars = false;        // the core GUI's bar beside every other player's character (F7)
    bool beatRing = false;          // the Music tab's ring that dances to what plays
    std::string musicStatus;        // what the app says of it (listening, or why not)
};

// What the performance overlay shows, from the app once a second or so.
struct PerformanceStats {
    float fps = 0.0f;
    float frameMilliseconds = 0.0f;
    std::uint32_t drawCalls = 0;
    std::uint64_t triangles = 0;
};

// The Escape menu: a panel that slides in from the right with Roblox's
// in-experience tabs (Home, Players, Settings, Report, Help), Lunarstone's
// Luau tab (the F9 console's stand-in, CoreGui::console) and the reset and
// leave confirmations. Modal while open: it takes every key,
// click, wheel and text event. Owned by CoreGui, whose callbacks and
// settings it works.
//
// Keys while open: Esc closes a dialog, then the menu; R and L ask to reset
// and to leave; Tab and Shift+Tab switch tabs; Up and Down move the focus,
// Left and Right change the focused setting, Enter and Space press it. In
// the Luau tab's editor Enter is a new line, Ctrl+Enter runs the buffer, Tab
// indents, the arrows walk its lines before the history, and Ctrl+L clears
// the log.
//
// The layout is its own (Roblox's current menu for the structure and feel);
// the reasons on the Report tab are the 2016 ReportAbuseMenu's, from memory
// (GUESS). Nothing on the Report tab goes anywhere.
class GameMenu {
public:
    enum class Tab : std::uint8_t { Home, Players, Settings, Report, Help, Music, Luau };
    static constexpr int tabCount = 7;
    enum class Dialog : std::uint8_t { None, Reset, Leave };

    static constexpr float openSeconds = 0.24f;
    static constexpr float closeSeconds = 0.18f;
    // Logical pixels, less on narrow screens: room for eight tabs as wide as
    // the first six had (the bar is the panel less 24), seven of them used.
    static constexpr float panelWidth = 24.0f + 8 * (416.0f / 6);
    static constexpr float minSensitivity = 0.2f;
    static constexpr float maxSensitivity = 4.0f;
    // How many lines the Luau editor shows before it starts scrolling.
    static constexpr int luauMaxLines = 10;

    GameMenu(CoreGui& owner, FontLibrary& fonts, GlyphAtlas& atlas);

    void setGame(DataModel* game);
    void setImageProvider(ImageProvider images) { images_ = std::move(images); }
    // The experience's name for the header; empty for "Menu".
    void setTitle(std::string title) { title_ = std::move(title); }
    // The full map's key for the Help tab; none when there's no map.
    void setMapKey(std::optional<platform::Key> key) { mapKey_ = key; }

    // Open: takes input. animate slides it (Esc); otherwise it's there at once.
    bool open() const { return open_; }
    void setOpen(bool open, bool animate = false);
    // How far it has slid in, 0 to 1 (eased); drawn while above 0.
    float shown() const { return shown_; }
    bool visible() const { return shown_ > 0.0f; }

    Tab tab() const { return tab_; }
    void selectTab(Tab tab);
    Dialog dialog() const { return dialog_; }
    void openDialog(Dialog dialog);
    // Typing in the report's description or the Luau tab's command line.
    bool typing() const { return typing_; }
    // The Luau tab's command line, and which lines its log shows.
    const TextEditor& luauInput() const { return luauInput_; }
    // Where the editor is scrolled to: the first of its lines shown,
    // counting from 0, and how far the caret's own line is shifted left, both
    // to keep the caret in view.
    float luauScroll() const { return luauScroll_; }
    float luauShift() const { return luauShift_; }
    // Where its line numbers are drawn, from the last time it was laid out.
    RectF luauGutter() const { return luauGutter_; }
    enum class LogFilter : std::uint8_t { All, Output, Warnings, Errors };

    // The report form, as filled in. submitted counts presses of Submit
    // that passed; nothing is ever sent.
    struct Report {
        bool aboutPlayer = false;   // else the experience
        std::string player;         // Player.Name
        int reason = -1;            // into reasons(), -1 for none chosen
        TextEditor description;
        int submitted = 0;
        bool notice = false; // the "not sent" note is up
    };
    const Report& report() const { return report_; }
    static std::span<const std::string_view> reasons(bool aboutPlayer);

    // Only while open (or for the Escape that opens it).
    bool handle(const platform::Event& event);
    void update(double time, Vector2 screen, float topInset, float pixelScale);
    void draw(DrawList& list);

    // Where a control was laid out this frame, by id ("tab.home",
    // "home.resume", "dialog.confirm", "settings.fullscreen", ...), for tests.
    std::optional<RectF> controlRect(std::string_view id) const;
    std::optional<std::string> focused() const;

private:
    struct Control {
        std::string id;
        RectF rect;
        RectF clip;
        bool enabled = true;
        bool focusable = true;
        bool content = false; // in the scrolled tab content (rect.y is on screen)
        std::function<void()> press;
        std::function<void(int)> step;   // Left/Right
        std::function<void(float)> drag; // a slider: the fraction under the pointer
    };
    struct Colors;

    void layout(DrawList* list);
    void drawHeader(DrawList* list, const RectF& panel);
    void drawTabs(DrawList* list, const RectF& bar);
    float drawHome(DrawList* list, RectF area);
    float drawPlayers(DrawList* list, RectF area);
    float drawSettings(DrawList* list, RectF area);
    float drawReport(DrawList* list, RectF area);
    float drawHelp(DrawList* list, RectF area);
    float drawMusic(DrawList* list, RectF area);
    float drawLuau(DrawList* list, RectF area);
    // Lunarstone's own tab icon: a terminal window with a prompt.
    void luauIcon(DrawList* list, RectF box, Color4 tint);
    void musicIcon(DrawList* list, RectF box, Color4 tint);
    void toggleControl(DrawList* list, std::string id, const RectF& r, bool on, bool enabled,
                       std::function<void(bool)> set);
    // A one-line text field typed into while it's clicked; long text scrolls
    // to keep the caret in view.
    Control& field(DrawList* list, std::string id, RectF rect, TextEditor& editor, std::string_view placeholder,
                   Enum::Font font, bool enabled = true);
    // A multi-line Luau editor: a gutter of line numbers, the code coloured,
    // as many lines as it holds up to luauMaxLines and then scrolling inside
    // itself. Its height is codeHeight(); clicking puts the caret where the
    // pointer was.
    Control& codeField(DrawList* list, std::string id, RectF rect, TextEditor& editor, std::string_view placeholder,
                       bool enabled = true);
    float codeLine() const { return 20.0f * k_; }  // one line of Luau, tall enough for the font
    float codePad() const { return 10.0f * k_; }   // above and below them
    float codeHeight() const;                      // for what's in luauInput_
    float codeWidth(std::size_t lines);            // the gutter, for the biggest number
    // Where its lines start inside it, how far into a line a byte offset is,
    // and the caret a click in it left.
    float codeTop(RectF rect, std::size_t shown) const;
    float codeX(std::string_view line, std::size_t column) const;
    void caretInCode(TextEditor& editor, RectF rect);
    TextEditor& editing() { return tab_ == Tab::Luau ? luauInput_ : report_.description; }
    std::string_view editingId() const { return tab_ == Tab::Luau ? "luau.input" : "report.description"; }
    void runLuau();
    void browseHistory(int by);
    void drawDialog(DrawList* list, const RectF& panel);

    // Drawing helpers; all of them only lay out when list is null.
    void text(DrawList* list, std::string_view text, RectF box, float size, Enum::Font font, Color4 color,
              Enum::TextXAlignment x = Enum::TextXAlignment::Left,
              Enum::TextYAlignment y = Enum::TextYAlignment::Center, bool wrapped = false);
    float textWidth(std::string_view text, float size, Enum::Font font);
    float wrappedHeight(std::string_view text, float size, Enum::Font font, float width);
    void box(DrawList* list, RectF rect, Color4 color, float radius, float stroke = 0.0f);
    void image(DrawList* list, std::string_view id, RectF box, Color4 tint);
    void chevron(DrawList* list, float cx, float cy, float size, int direction, Color4 color);
    void keyChip(DrawList* list, std::string_view key, float x, float centerY, Color4 color, float* width = nullptr);
    void focusRing(DrawList* list, const Control& control, float radius);
    enum class ButtonStyle : std::uint8_t { Primary, Secondary };
    Control& button(DrawList* list, std::string id, RectF rect, std::string_view label, std::string_view key,
                    ButtonStyle style, std::function<void()> press, bool enabled = true);
    Control& add(Control control);
    bool hovered(const Control& control) const;
    bool isFocused(const Control& control) const;

    void moveFocus(int by);
    void pressFocused();
    void stepFocused(int by);
    void closeDialogOrMenu();
    void resetReportNotice() { report_.notice = false; }
    void submitReport();
    std::vector<Player*> sortedPlayers(Player** local) const;
    std::vector<Player*> otherPlayers() const;
    float& scroll() { return scroll_[static_cast<std::size_t>(tab_)]; }

    CoreGui* owner_;
    FontLibrary* fonts_;
    GlyphAtlas* atlas_;
    ImageProvider images_;
    std::weak_ptr<Instance> game_;
    std::string title_;
    std::optional<platform::Key> mapKey_ = platform::Key::M;

    bool open_ = false;
    float shown_ = 0.0f;   // eased
    float slideFrom_ = 0.0f;
    float slideT_ = 1.0f;  // seconds into the slide, past its length when done
    double lastTime_ = -1.0;
    double time_ = 0.0;
    double openedAt_ = 0.0;

    Tab tab_ = Tab::Home;
    Dialog dialog_ = Dialog::None;
    std::array<float, tabCount> scroll_{};
    float contentHeight_ = 0.0f;
    RectF content_{};
    RectF panel_{};
    RectF clip_{};          // what the helpers draw within
    bool inContent_ = false; // controls added now scroll with the tab

    std::vector<Control> controls_;
    std::optional<std::string> focus_;
    bool focusVisible_ = false; // the ring shows after keyboard use, like :focus-visible
    bool scrollToEnd_ = false; // the tab's content, at the next layout
    std::optional<std::string> pressed_;  // the control the left button went down on
    std::optional<std::string> dragging_; // a slider being dragged

    Report report_;
    TextEditor luauInput_;
    float luauScroll_ = 0.0f; // the editor's first line shown, to keep the caret in view
    float luauShift_ = 0.0f;  // and how far its lines are shifted left, to keep the caret in view
    RectF luauGutter_{};      // where the line numbers are
    int historyAt_ = -1; // into the console's history while Up and Down browse it; -1 for the draft
    LogFilter logFilter_ = LogFilter::All;
    std::uint64_t logSeen_ = 0; // DevConsole::added() when last drawn
    bool typing_ = false;
    double caretTime_ = 0.0;

    Vector2 screen_;
    float top_ = 36.0f;
    float k_ = 1.0f;
    float mouseX_ = -1.0f, mouseY_ = -1.0f;
};

} // namespace lunar::ui

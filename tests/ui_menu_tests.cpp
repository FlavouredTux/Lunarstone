#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/assets/AssetCache.hpp>
#include <lunar/ui/CoreGui.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/GameMenu.hpp>
#include <lunar/ui/LuauHighlight.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace lunar;
using namespace lunar::ui;
using platform::Key;

namespace {

// The core GUI on a 1280x720 screen over a game with a local player (with a
// character) and two others, and counters on every callback the menu has.
struct Menu {
    std::shared_ptr<DataModel> game = DataModel::create();
    FontLibrary fonts{assets::AssetConfig::fromEnvironment().contentDir};
    GlyphAtlas atlas;
    ui::CoreGui core{fonts, atlas};
    std::shared_ptr<Humanoid> humanoid = std::make_shared<Humanoid>();
    double time = 1.0;
    int resets = 0, leaves = 0, calls = 0;
    std::optional<render::GraphicsPreset> preset;
    std::optional<bool> fullscreen;
    std::optional<float> sensitivity;

    Menu()
    {
        auto* players = game->getService<Players>();
        for (const char* name : {"builderman", "Shedletsky", "Telamon"}) {
            auto p = std::make_shared<Player>();
            p->setName(name);
            p->setParent(players);
            if (std::string_view(name) == "builderman") {
                auto character = std::make_shared<Model>();
                humanoid->setParent(character.get());
                character->setParent(game->workspace());
                p->setCharacter(character.get());
                players->assignLocalPlayer(p.get());
            }
        }
        core.setGame(game.get());
        core.onReset = [this] { ++resets; };
        core.onLeave = [this] { ++leaves; };
        core.onGraphicsQuality = [this](render::GraphicsPreset p) {
            preset = p;
            ++calls;
        };
        core.onFullscreen = [this](bool on) {
            fullscreen = on;
            ++calls;
        };
        core.onCameraSensitivity = [this](float value) {
            sensitivity = value;
            ++calls;
        };
        frame(0.0);
    }
    GameMenu& menu() { return core.menu(); }
    void frame(double dt = 1.0 / 60.0)
    {
        time += dt;
        core.update(time, {1280, 720}, 36.0f, 1.0f);
    }
    bool press(Key key, bool shift = false, bool ctrl = false)
    {
        platform::KeyEvent down{.key = key, .pressed = true};
        down.modifiers.shift = shift;
        down.modifiers.ctrl = ctrl;
        const bool used = core.handle(down);
        core.handle(platform::KeyEvent{.key = key, .pressed = false});
        frame();
        return used;
    }
    // A left click in the middle of a menu control laid out this frame.
    void click(std::string_view id)
    {
        frame();
        const auto r = menu().controlRect(id);
        if (!r)
            throw lt::Failure{"no control " + std::string(id)};
        const double x = r->x + r->w / 2, y = r->y + r->h / 2;
        core.handle(platform::MouseMoveEvent{x, y});
        core.handle(platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = true, .x = x, .y = y});
        core.handle(platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = false, .x = x, .y = y});
        frame();
    }
    // Opened (and slid all the way in).
    void open()
    {
        press(Key::Escape);
        for (int i = 0; i < 20; ++i)
            frame();
    }
};

} // namespace

TEST(menu_escape_slides_it_in_and_out)
{
    Menu m;
    CHECK(!m.core.menuOpen());
    CHECK(!m.core.menuVisible());
    CHECK(m.press(Key::Escape));
    CHECK(m.core.menuOpen());
    // On its way in: part of the way after a frame, all of it after the slide.
    CHECK(m.menu().shown() > 0.0f);
    CHECK(m.menu().shown() < 1.0f);
    for (int i = 0; i < 20; ++i)
        m.frame();
    CHECK_EQ(m.menu().shown(), 1.0f);
    CHECK(m.menu().tab() == GameMenu::Tab::Home);

    // Esc again: input goes back to the game at once, the panel slides out.
    CHECK(m.press(Key::Escape));
    CHECK(!m.core.menuOpen());
    CHECK(m.core.menuVisible());
    for (int i = 0; i < 20; ++i)
        m.frame();
    CHECK(!m.core.menuVisible());

    // setMenuOpen skips the slide.
    m.core.setMenuOpen(true);
    CHECK_EQ(m.menu().shown(), 1.0f);
}

TEST(menu_is_modal)
{
    Menu m;
    // Closed: the game's keys pass, Tab toggles the player list.
    CHECK(!m.core.handle(platform::KeyEvent{.key = Key::W, .pressed = true}));
    CHECK(!m.core.handle(platform::KeyEvent{.key = Key::W, .pressed = false}));
    CHECK(!m.core.handle(platform::TextEvent{"w"}));
    CHECK(m.core.playerListOpen());
    m.press(Key::Tab);
    CHECK(!m.core.playerListOpen());
    m.press(Key::Tab);

    m.open();
    // Open: everything is the menu's; Tab switches its tabs, not the list.
    CHECK(m.core.handle(platform::KeyEvent{.key = Key::W, .pressed = true}));
    CHECK(m.core.handle(platform::KeyEvent{.key = Key::W, .pressed = false}));
    CHECK(m.core.handle(platform::KeyEvent{.key = Key::Num1, .pressed = true}));
    CHECK(m.core.handle(platform::TextEvent{"w"}));
    CHECK(m.core.handle(platform::ScrollEvent{.dy = 1}));
    CHECK(m.core.handle(platform::MouseButtonEvent{.button = platform::MouseButton::Right, .pressed = true, .x = 10, .y = 10}));
    CHECK(m.core.handle(platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = true, .x = 900, .y = 300}));
    CHECK(m.core.handle(platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = false, .x = 900, .y = 300}));
    // Moving the pointer is never only the menu's.
    CHECK(!m.core.handle(platform::MouseMoveEvent{100, 100}));
    m.press(Key::Tab);
    CHECK(m.core.playerListOpen());
    CHECK(!m.core.pointerOverPlayerList());
    CHECK_EQ(m.resets + m.leaves + m.calls, 0);
}

TEST(menu_tabs_switch_by_click_and_key)
{
    Menu m;
    m.open();
    m.click("tab.settings");
    CHECK(m.menu().tab() == GameMenu::Tab::Settings);
    m.press(Key::Tab);
    CHECK(m.menu().tab() == GameMenu::Tab::Report);
    m.press(Key::Tab);
    m.press(Key::Tab);
    CHECK(m.menu().tab() == GameMenu::Tab::Music);
    m.press(Key::Tab);
    CHECK(m.menu().tab() == GameMenu::Tab::Luau);
    m.press(Key::Tab);
    CHECK(m.menu().tab() == GameMenu::Tab::Home); // round from Luau
    m.press(Key::Tab, true);
    CHECK(m.menu().tab() == GameMenu::Tab::Luau);
    m.click("tab.players");
    CHECK(m.menu().tab() == GameMenu::Tab::Players);
    // Everyone but us can be reported from the list; we're first.
    CHECK(m.menu().controlRect("players.report.Shedletsky").has_value());
    CHECK(!m.menu().controlRect("players.report.builderman").has_value());
    m.click("players.report.Telamon");
    CHECK(m.menu().tab() == GameMenu::Tab::Report);
    CHECK(m.menu().report().aboutPlayer);
    CHECK_EQ(m.menu().report().player, std::string("Telamon"));

    // Each opening starts at Home.
    m.press(Key::Escape);
    m.open();
    CHECK(m.menu().tab() == GameMenu::Tab::Home);
}

TEST(menu_reset_asks_first)
{
    Menu m;
    m.open();
    // R asks; Esc closes the question first, then the menu.
    m.press(Key::R);
    CHECK(m.menu().dialog() == GameMenu::Dialog::Reset);
    m.press(Key::Escape);
    CHECK(m.menu().dialog() == GameMenu::Dialog::None);
    CHECK(m.core.menuOpen());
    m.press(Key::Escape);
    CHECK(!m.core.menuOpen());

    // Don't Reset does nothing.
    m.open();
    m.click("home.reset");
    CHECK(m.menu().dialog() == GameMenu::Dialog::Reset);
    // Under the question only its buttons answer.
    m.click("home.leave");
    CHECK(m.menu().dialog() == GameMenu::Dialog::Reset);
    m.click("dialog.cancel");
    CHECK(m.menu().dialog() == GameMenu::Dialog::None);
    CHECK_EQ(m.resets, 0);

    // Reset resets and closes the menu; so does R then Enter.
    m.click("home.reset");
    m.click("dialog.confirm");
    CHECK_EQ(m.resets, 1);
    CHECK(!m.core.menuOpen());
    m.open();
    m.press(Key::R);
    m.press(Key::Enter);
    CHECK_EQ(m.resets, 2);
    CHECK(!m.core.menuOpen());
    CHECK_EQ(m.leaves, 0);
}

TEST(menu_leave_asks_first)
{
    Menu m;
    m.open();
    m.press(Key::L);
    CHECK(m.menu().dialog() == GameMenu::Dialog::Leave);
    // The arrows move between Leave and Don't Leave.
    m.press(Key::Right);
    CHECK_EQ(m.menu().focused().value_or(""), std::string("dialog.cancel"));
    m.press(Key::Enter);
    CHECK(m.menu().dialog() == GameMenu::Dialog::None);
    CHECK_EQ(m.leaves, 0);
    m.click("home.leave");
    m.click("dialog.confirm");
    CHECK_EQ(m.leaves, 1);
    CHECK_EQ(m.resets, 0);
}

TEST(menu_resume_closes_it)
{
    Menu m;
    m.open();
    m.click("home.resume");
    CHECK(!m.core.menuOpen());
    m.open();
    m.click("header.close");
    CHECK(!m.core.menuOpen());
    // A click on the dimmed game beside the panel closes it too.
    m.open();
    m.core.handle(platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = true, .x = 100, .y = 400});
    CHECK(!m.core.menuOpen());
}

TEST(menu_settings_call_back)
{
    Menu m;
    m.core.menuSettings.graphics = render::GraphicsPreset::Average;
    m.open();
    m.click("tab.settings");
    m.click("settings.graphics.3"); // Potato, Minimal, Average, Max
    CHECK(m.preset == render::GraphicsPreset::Max);
    CHECK(m.core.menuSettings.graphics == render::GraphicsPreset::Max);
    m.click("settings.fullscreen");
    CHECK(m.fullscreen == true);
    CHECK(m.core.menuSettings.fullscreen);
    m.click("settings.fullscreen");
    CHECK(m.fullscreen == false);

    // The keyboard: from the switch just clicked up to the graphics, Left
    // and Right change it.
    CHECK_EQ(m.menu().focused().value_or(""), std::string("settings.fullscreen"));
    m.press(Key::Up);
    CHECK_EQ(m.menu().focused().value_or(""), std::string("settings.graphics"));
    m.press(Key::Left);
    CHECK(m.preset == render::GraphicsPreset::Average);
    m.press(Key::Down); // fullscreen
    m.press(Key::Down); // sensitivity
    CHECK_EQ(m.menu().focused().value_or(""), std::string("settings.sensitivity"));
    m.press(Key::Right);
    CHECK(m.sensitivity.has_value());
    CHECK(std::abs(m.sensitivity.value_or(0) - 1.1f) < 1e-4f);

    // Shift lock has no callback here: greyed out, clicks do nothing.
    const int before = m.calls;
    m.click("settings.shiftlock");
    CHECK(!m.core.menuSettings.shiftLock);
    // The performance overlay is the core GUI's own.
    m.click("settings.stats");
    CHECK(m.core.menuSettings.performanceStats);
    m.click("settings.healthbars");
    CHECK(m.core.menuSettings.healthBars);
    CHECK_EQ(m.calls, before);
    // What Lunarstone can't do never changes.
    m.click("settings.cameramode.next");
    m.press(Key::Down);
    CHECK_EQ(m.calls, before);
}

TEST(menu_report_sends_nothing)
{
    Menu m;
    m.open();
    m.click("tab.report");
    // Nothing to submit before a reason.
    m.click("report.submit");
    CHECK_EQ(m.menu().report().submitted, 0);
    m.click("report.kind.1");
    CHECK(m.menu().report().aboutPlayer);
    CHECK_EQ(m.menu().report().player, std::string("Shedletsky")); // the first one else, by name
    m.click("report.player.next");
    CHECK_EQ(m.menu().report().player, std::string("Telamon"));
    m.click("report.reason.next");
    CHECK_EQ(m.menu().report().reason, 0);

    // Typing: R and L are letters here, Esc only stops typing.
    m.click("report.description");
    CHECK(m.menu().typing());
    m.core.handle(platform::KeyEvent{.key = Key::R, .pressed = true});
    m.core.handle(platform::TextEvent{"rude in chat"});
    m.press(Key::L);
    CHECK(m.menu().dialog() == GameMenu::Dialog::None);
    CHECK_EQ(m.menu().report().description.text, std::string("rude in chat"));
    m.press(Key::Escape);
    CHECK(!m.menu().typing());
    CHECK(m.core.menuOpen());

    m.click("report.submit");
    CHECK_EQ(m.menu().report().submitted, 1);
    CHECK(m.menu().report().notice);
    // No callback heard of it (and the menu has none it could tell).
    CHECK_EQ(m.resets + m.leaves + m.calls, 0);
    CHECK(m.core.menuOpen());
}

TEST(menu_luau_tab_logs_and_runs_code)
{
    Menu m;
    DevConsole& console = m.core.console();
    std::vector<std::string> ran;
    console.run = [&](const std::string& code) -> std::string {
        ran.push_back(code);
        return code == "error()" ? "Console:1: boom" : std::string();
    };
    console.add(DevConsole::Kind::Output, "hello");
    console.add(DevConsole::Kind::Warning, "careful");
    m.open();
    m.click("tab.luau");
    CHECK(m.menu().tab() == GameMenu::Tab::Luau);
    CHECK(m.menu().controlRect("luau.filter.2").has_value());

    // Typed and run with Ctrl+Enter: echoed, kept for Up, and it keeps typing.
    // Enter on its own is a new line now.
    m.click("luau.input");
    CHECK(m.menu().typing());
    m.core.handle(platform::TextEvent{"print(1)"});
    m.press(Key::Enter);
    CHECK_EQ(ran.size(), std::size_t{0});
    m.press(Key::Backspace);
    m.press(Key::Enter, false, true);
    CHECK_EQ(ran, (std::vector<std::string>{"print(1)"}));
    CHECK(m.menu().luauInput().text.empty());
    CHECK(m.menu().typing());
    m.core.handle(platform::TextEvent{"error()"});
    m.press(Key::Enter, false, true);
    const auto lines = console.lines();
    CHECK_EQ(lines.size(), 5u);
    CHECK(lines[2].kind == DevConsole::Kind::Command);
    CHECK_EQ(lines[2].text, std::string("> print(1)"));
    CHECK(lines[4].kind == DevConsole::Kind::Error);
    m.press(Key::Up);
    CHECK_EQ(m.menu().luauInput().text, std::string("error()"));
    m.press(Key::Up);
    CHECK_EQ(m.menu().luauInput().text, std::string("print(1)"));
    m.press(Key::Down);
    m.press(Key::Down);
    CHECK(m.menu().luauInput().text.empty());

    // L is a letter while typing; Esc stops.
    m.press(Key::L);
    CHECK(m.menu().dialog() == GameMenu::Dialog::None);
    m.press(Key::Escape);
    CHECK(!m.menu().typing());
    m.click("luau.clear");
    CHECK(console.lines().empty());

    // Nothing to run in: the line's greyed out.
    console.run = nullptr;
    m.frame();
    m.click("luau.input");
    CHECK(!m.menu().typing());
}

TEST(menu_luau_tab_edits_a_buffer_of_lines)
{
    Menu m;
    DevConsole& console = m.core.console();
    std::vector<std::string> ran;
    console.run = [&](const std::string& code) -> std::string {
        ran.push_back(code);
        return std::string();
    };
    m.open();
    m.click("tab.luau");
    m.click("luau.input");
    const float oneLine = m.menu().controlRect("luau.input")->h;

    // Enter, and Shift+Enter, are new lines; Ctrl+Enter runs.
    m.core.handle(platform::TextEvent{"local x = 1"});
    m.press(Key::Enter);
    m.press(Key::Enter, true);
    m.core.handle(platform::TextEvent{"print(x)"});
    CHECK_EQ(m.menu().luauInput().text, std::string("local x = 1\n\nprint(x)"));
    CHECK_EQ(m.menu().luauInput().lineCount(), std::size_t{3});
    CHECK_EQ(m.menu().luauInput().caretLine(), std::size_t{2});
    CHECK_EQ(ran.size(), std::size_t{0});
    // The editor grows with the lines in it.
    CHECK(m.menu().controlRect("luau.input")->h > oneLine);

    // Home and End stay on the caret's line.
    m.press(Key::Home);
    CHECK_EQ(m.menu().luauInput().caret, m.menu().luauInput().lineStart());
    m.press(Key::End);
    CHECK_EQ(m.menu().luauInput().caret, m.menu().luauInput().lineEnd());
    // Up and Down walk the lines; there being none above the first, and none
    // below the last, is what leaves them to the history.
    m.press(Key::Up);
    m.press(Key::Up);
    CHECK_EQ(m.menu().luauInput().caretLine(), std::size_t{0});
    m.press(Key::Down);
    CHECK_EQ(m.menu().luauInput().caretLine(), std::size_t{1});
    m.press(Key::Down);
    m.press(Key::Down);
    CHECK_EQ(m.menu().luauInput().caretLine(), std::size_t{2});

    // The whole buffer runs as one thing, and the log echoes it with its line
    // breaks, each under the prompt. The history keeps it whole too.
    const std::string buffer = m.menu().luauInput().text;
    m.press(Key::Enter, false, true);
    CHECK_EQ(ran, (std::vector<std::string>{buffer}));
    CHECK(m.menu().luauInput().text.empty());
    const auto lines = console.lines();
    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK(lines[0].kind == DevConsole::Kind::Command);
    CHECK_EQ(lines[0].text, std::string("> local x = 1"));
    CHECK_EQ(lines[1].text, std::string("  "));
    CHECK_EQ(lines[2].text, std::string("  print(x)"));
    CHECK_EQ(console.history().back(), buffer);

    // Up from the first line brings the whole entry back, caret on its last.
    m.press(Key::Up);
    CHECK_EQ(m.menu().luauInput().text, buffer);
    CHECK_EQ(m.menu().luauInput().caretLine(), std::size_t{2});
    // Down off the last one goes forward through the history, to the draft.
    m.press(Key::Down);
    CHECK(m.menu().luauInput().text.empty());

    // Ten lines fill the editor; past that it scrolls to keep the caret in view.
    m.core.handle(platform::TextEvent{"a\nb\nc\nd\ne\nf\ng\nh\ni\nj"});
    m.frame();
    CHECK_EQ(m.menu().luauInput().lineCount(), std::size_t{10});
    const float tenLines = m.menu().controlRect("luau.input")->h;
    CHECK_EQ(m.menu().luauScroll(), 0.0f);
    m.core.handle(platform::TextEvent{"\nk\nl\nm"});
    m.frame();
    CHECK_EQ(m.menu().luauInput().lineCount(), std::size_t{13});
    CHECK_EQ(m.menu().controlRect("luau.input")->h, tenLines); // no taller past ten
    CHECK_EQ(m.menu().luauScroll(), 3.0f);                     // and three lines gone off the top

    // Ctrl+L clears the log, as it does in the console.
    CHECK(!console.lines().empty());
    m.press(Key::L, false, true);
    CHECK(console.lines().empty());
    CHECK(m.core.menuOpen());
}

TEST(menu_luau_tab_indents_and_keeps_tab_switching)
{
    Menu m;
    m.core.console().run = [](const std::string&) { return std::string(); };
    m.open();
    m.click("tab.luau");
    // An empty editor's Tab moves tabs, as it does over the rest of the menu.
    m.click("luau.input");
    CHECK(m.menu().typing());
    m.press(Key::Tab);
    CHECK(m.menu().tab() == GameMenu::Tab::Home);
    CHECK(!m.menu().typing());

    m.click("tab.luau");
    m.click("luau.input");
    m.core.handle(platform::TextEvent{"print(1)"});
    m.press(Key::Home);
    m.press(Key::Tab);
    CHECK_EQ(m.menu().luauInput().text, std::string("    print(1)"));
    m.press(Key::End);
    m.press(Key::Tab);
    CHECK_EQ(m.menu().luauInput().text, std::string("    print(1)    "));
    // Shift+Tab changes tab whether there's code in it or not: the one before.
    m.press(Key::Tab, true);
    CHECK(m.menu().tab() == GameMenu::Tab::Music);
    CHECK(!m.menu().typing());

    // A click puts the caret where the pointer was: past the end of its line.
    m.click("tab.luau");
    m.click("luau.input");
    m.core.handle(platform::TextEvent{"one\ntwo\nthree"});
    m.frame();
    const RectF field = *m.menu().controlRect("luau.input");
    const double x = field.right() - 40, y = field.y + 40; // the second line
    m.core.handle(platform::MouseMoveEvent{x, y});
    m.core.handle(platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = true, .x = x, .y = y});
    m.core.handle(platform::MouseButtonEvent{.button = platform::MouseButton::Left, .pressed = false, .x = x, .y = y});
    CHECK(m.menu().typing());
    CHECK_EQ(m.menu().luauInput().caretLine(), std::size_t{1});
    CHECK_EQ(m.menu().luauInput().caret, m.menu().luauInput().lineEnd());
}

TEST(menu_luau_tab_numbers_every_line_it_shows)
{
    if (assets::AssetConfig::fromEnvironment().contentDir.empty())
        SKIP("set LUNAR_CONTENT_DIR to run font-dependent tests");
    Menu m;
    m.core.console().run = [](const std::string&) { return std::string(); };
    m.open();
    m.click("tab.luau");
    m.click("luau.input");
    m.core.handle(platform::TextEvent{"one\ntwo\nthree"});
    m.frame();

    // Drawn: a number in the gutter beside every line, not just the first.
    DrawList list;
    m.core.draw(list);
    const RectF gutter = m.menu().luauGutter();
    CHECK(gutter.h > 0.0f);
    const float row = gutter.h / 3.0f;
    for (int i = 0; i < 3; ++i) {
        bool numbered = false;
        for (const Quad& q : list.quads) {
            if (q.kind != QuadKind::Glyph)
                continue;
            const float cx = q.rect.x + q.rect.w / 2, cy = q.rect.y + q.rect.h / 2;
            // In the gutter's row, and inside the clip it was drawn with.
            if (gutter.contains(cx, cy) && q.clip.contains(cx, cy) && cy >= gutter.y + static_cast<float>(i) * row)
                numbered = true;
        }
        CHECK(numbered);
    }
}

TEST(luau_highlight_reads_a_line)
{
    using Token = LuauToken;
    // The spans cover the line: how long each run is, and what it is.
    auto kinds = [](std::string_view code) {
        std::vector<std::pair<std::size_t, Token>> out;
        for (const LuauSpan& span : highlightLuau(code))
            out.emplace_back(span.end - span.begin, span.token);
        return out;
    };
    // What the character at an offset is, and where its run begins and ends.
    auto runAt = [](std::string_view code, std::size_t at) {
        for (const LuauSpan& span : highlightLuau(code))
            if (at >= span.begin && at < span.end)
                return span;
        return LuauSpan{};
    };

    CHECK_EQ(kinds("local x = 1").size(), std::size_t{3});
    CHECK(runAt("local x = 1", 0).token == Token::Keyword);
    CHECK(runAt("local x = 1", 6).token == Token::Code);
    CHECK(runAt("local x = 1", 10).token == Token::Number);
    CHECK_EQ(runAt("local x = 1", 6).begin, std::size_t{5});
    CHECK_EQ(runAt("local x = 1", 10).end, std::size_t{11});
    // A name is not a keyword, and a keyword is only one on its own.
    CHECK(runAt("android = nil", 0).token == Token::Code);
    CHECK(runAt("android = nil", 10).token == Token::Keyword);
    // Strings run to the quote that closes them, escapes and all.
    CHECK(runAt("print(\"hi\\\" there\")", 6).token == Token::String);
    CHECK_EQ(runAt("print(\"hi\\\" there\")", 6).end, std::size_t{18});
    CHECK(runAt("print('hi')", 6).token == Token::String);
    CHECK_EQ(runAt("print('hi')", 6).end, std::size_t{10});
    CHECK(runAt("local s = [[a\nb]]", 10).token == Token::String); // the long form
    CHECK_EQ(runAt("local s = [[a\nb]]", 10).end, std::size_t{17});
    // Comments run to the end of their line, the long form too.
    CHECK(runAt("x = 1 -- a note", 6).token == Token::Comment);
    CHECK_EQ(runAt("x = 1 -- a note", 6).end, std::size_t{15});
    CHECK(runAt("--[[ a note ]] x = 1", 2).token == Token::Comment);
    CHECK_EQ(runAt("--[[ a note ]] x = 1", 2).end, std::size_t{14});
    // Numbers: decimals, exponents and hex.
    CHECK(runAt("x = 1.5e-3", 4).token == Token::Number);
    CHECK_EQ(runAt("x = 1.5e-3", 4).end, std::size_t{10});
    CHECK(runAt("x = 0xFF", 4).token == Token::Number);
    CHECK(runAt("x = .5", 4).token == Token::Number);
    // Nothing at all is no spans; a line with nothing to tell is one.
    CHECK(highlightLuau("").empty());
    CHECK_EQ(kinds("a.b(c)").size(), std::size_t{1});
    CHECK(kinds("a.b(c)")[0].second == Token::Code);
}

TEST(menu_f7_toggles_the_health_bars)
{
    Menu m;
    CHECK(!m.core.menuSettings.healthBars); // off by default
    CHECK(m.press(Key::F7));
    CHECK(m.core.menuSettings.healthBars);
    // Ctrl+F7 and Alt+F7 are left alone.
    platform::KeyEvent ctrl{.key = Key::F7, .pressed = true};
    ctrl.modifiers.ctrl = true;
    CHECK(!m.core.handle(ctrl));
    CHECK(m.core.menuSettings.healthBars);
    // While the menu is open the key is the menu's.
    m.open();
    m.press(Key::F7);
    CHECK(m.core.menuSettings.healthBars);
    m.press(Key::Escape);
    for (int i = 0; i < 20; ++i)
        m.frame();
    CHECK(m.press(Key::F7));
    CHECK(!m.core.menuSettings.healthBars);
}

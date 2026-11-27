// StyleEngine: StyleSheets applied through StyleLinks, the way Roblox's
// Foundation library styles its components by tag.

#include "framework.hpp"

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/StyleEngine.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace lunar;

TEST(style_selectors_parse)
{
    auto plain = styles::parseSelector(".bg-surface-100");
    CHECK(plain.has_value());
    CHECK_EQ(plain->size(), 1u);
    CHECK_EQ((*plain)[0].compounds[0].tags, (std::vector<std::string>{"bg-surface-100"}));

    // Foundation's pseudo rule with its name, and the "> Class" alternative.
    auto pseudo = styles::parseSelector(".radius-small ::UICorner #Corner, .radius-small > UICorner");
    CHECK(pseudo.has_value());
    CHECK_EQ(pseudo->size(), 2u);
    CHECK_EQ((*pseudo)[0].pseudoClass, std::string("UICorner"));
    CHECK_EQ((*pseudo)[0].pseudoName, std::string("Corner"));
    CHECK_EQ((*pseudo)[1].compounds.size(), 2u);
    CHECK_EQ((*pseudo)[1].compounds[1].className, std::string("UICorner"));
    CHECK((*pseudo)[1].direct[0]);

    auto compound = styles::parseSelector("Frame.a.b#Name:Hover TextLabel");
    CHECK(compound.has_value());
    CHECK_EQ((*compound)[0].compounds[0].className, std::string("Frame"));
    CHECK_EQ((*compound)[0].compounds[0].tags.size(), 2u);
    CHECK_EQ((*compound)[0].compounds[0].name, std::string("Name"));
    CHECK_EQ((*compound)[0].compounds[0].states, (std::vector<std::string>{"Hover"}));
    CHECK(!(*compound)[0].direct[0]);

    // The Lua app's own forms: ">>" for descendants, "@Query", "::" right after.
    auto app = styles::parseSelector(".Detail >> .Body::StyleQuery #AtMax, @AtMax >> .Clip");
    CHECK(app.has_value());
    CHECK_EQ(app->size(), 2u);
    CHECK(!(*app)[0].direct[0]);
    CHECK_EQ((*app)[0].pseudoClass, std::string("StyleQuery"));
    CHECK_EQ((*app)[0].pseudoName, std::string("AtMax"));
    CHECK_EQ((*app)[1].compounds[0].queries, (std::vector<std::string>{"AtMax"}));

    for (const char* bad : {"", ".", "> .a", ".a >", ".a >>", ".a ::", ".a ::UICorner .b", "::UICorner"}) {
        CHECK(!styles::parseSelector(bad).has_value());
    }
}

TEST(style_engine_applies_tag_rules_tokens_and_pseudo_instances)
{
    auto gui = std::make_shared<ScreenGui>();
    auto sheet = std::make_shared<StyleSheet>();
    sheet->setAttribute("Surface", Value(Color3{0.25f, 0.5f, 0.75f}));
    auto fill = std::make_shared<StyleRule>();
    fill->setSelector(".bg-surface");
    fill->setStyleProperty("BackgroundColor3", Value(std::string("$Surface")));
    fill->setStyleProperty("BackgroundTransparency", Value(0.0));
    fill->setParent(sheet);
    auto corner = std::make_shared<StyleRule>();
    corner->setSelector(".rounded ::UICorner");
    corner->setStyleProperty("CornerRadius", Value(UDim{0.0f, 8}));
    corner->setParent(sheet);
    // A higher priority rule wins over the first one for the same property.
    auto strong = std::make_shared<StyleRule>();
    strong->setSelector(".bg-surface.clear");
    strong->setPriority(2);
    strong->setStyleProperty("BackgroundTransparency", Value(1.0));
    strong->setParent(sheet);
    auto link = std::make_shared<StyleLink>();
    link->setStyleSheet(sheet.get());
    link->setParent(gui);

    // One frame sets its transparency itself, which no rule overrides (as in
    // Roblox: Foundation's icons set TextSize over a default-size tag).
    auto own = std::make_shared<Frame>();
    own->setBackgroundTransparency(0.5f);
    own->addTag("bg-surface");
    own->addTag("rounded");
    own->setParent(gui);
    auto plain = std::make_shared<Frame>();
    plain->addTag("bg-surface");
    plain->setParent(gui);

    StyleEngine engine;
    Instance* const roots[] = {gui.get()};
    engine.apply(roots);
    CHECK(own->backgroundColor3() == (Color3{0.25f, 0.5f, 0.75f}));
    CHECK_EQ(own->backgroundTransparency(), 0.5f);
    CHECK_EQ(plain->backgroundTransparency(), 0.0f);
    auto* made = dynamic_cast<UICorner*>(own->findFirstChild("UICorner"));
    CHECK(made != nullptr);
    CHECK(made->cornerRadius() == (UDim{0.0f, 8}));

    own->addTag("clear");
    plain->addTag("clear");
    engine.apply(roots);
    CHECK_EQ(own->backgroundTransparency(), 0.5f);
    CHECK_EQ(plain->backgroundTransparency(), 1.0f);

    // Set after styling: it's the frame's own from then on.
    plain->setBackgroundColor3(Color3{1.0f, 0.0f, 0.0f});
    engine.apply(roots);
    CHECK(plain->backgroundColor3() == (Color3{1.0f, 0.0f, 0.0f}));

    // Tags gone: styled values go back to the defaults and the corner goes.
    for (Frame* frame : {own.get(), plain.get()})
        for (const char* tag : {"clear", "bg-surface", "rounded"})
            frame->removeTag(tag);
    engine.apply(roots);
    CHECK(own->backgroundColor3() == Frame().backgroundColor3());
    CHECK_EQ(plain->backgroundTransparency(), 0.0f);
    CHECK(plain->backgroundColor3() == (Color3{1.0f, 0.0f, 0.0f}));
    CHECK(own->findFirstChild("UICorner") == nullptr);
    CHECK_EQ(engine.styledCount(), 0u);
}

// ResetPropertyToDefault gives a property back to styling: React resets a
// prop that goes nil (Foundation's buttons leave their background to the
// "gui-object-defaults" style), and the write it makes doesn't count.
TEST(style_engine_styles_a_property_reset_to_default)
{
    auto gui = std::make_shared<ScreenGui>();
    auto sheet = std::make_shared<StyleSheet>();
    auto defaults = std::make_shared<StyleRule>();
    defaults->setSelector(".gui-object-defaults");
    defaults->setStyleProperty("BackgroundTransparency", Value(1.0));
    defaults->setParent(sheet);
    auto link = std::make_shared<StyleLink>();
    link->setStyleSheet(sheet.get());
    link->setParent(gui);
    auto button = std::make_shared<ImageButton>();
    button->addTag("gui-object-defaults");
    button->setParent(gui);

    StyleEngine engine;
    Instance* const roots[] = {gui.get()};
    engine.apply(roots);
    CHECK_EQ(button->backgroundTransparency(), 1.0f);

    // Written by someone else, even to the default: the button's own.
    button->setBackgroundTransparency(0.0f);
    engine.apply(roots);
    CHECK_EQ(button->backgroundTransparency(), 0.0f);

    const std::array<Value, 1> property{Value(std::string("BackgroundTransparency"))};
    button->fireEvent("ResetPropertyToDefault", property);
    engine.apply(roots);
    CHECK_EQ(button->backgroundTransparency(), 1.0f);
}

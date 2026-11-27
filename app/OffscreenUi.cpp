#include "OffscreenUi.hpp"
#include <lunar/script/PlaySession.hpp>
#include <lunar/script/ScriptContext.hpp>

#include "GameWindow.hpp"

#include <lunar/assets/AssetCache.hpp>

#include <cstdlib>
#include <print>
#include <utility>

std::expected<std::unique_ptr<OffscreenUi>, std::string>
OffscreenUi::create(lunar::render::Context& context, lunar::render::OffscreenRenderer& offscreen,
                    lunar::DataModel& game, bool gameOverlay)
{
    std::unique_ptr<OffscreenUi> made(new OffscreenUi);
    made->offscreen_ = &offscreen;
    auto config = lunar::assets::AssetConfig::fromEnvironment();
    made->textures_
        = std::make_shared<lunar::assets::TextureLoader>(std::make_shared<lunar::assets::AssetCache>(config));
    lunar::ui::GameUi::Options options;
    options.contentDir = config.contentDir;
    options.images = [ui = made.get()](std::string_view id) {
        if (!ui->asked_.contains(id))
            ui->asked_.emplace(id);
        auto image = ui->textures_->image(id);
        if (!image && ui->textures_->state(id) != lunar::assets::TextureState::Failed)
            ui->loading_ = true;
        return image;
    };
    options.map.minimap = false;
    options.map.fullMapKey.reset();
    if (!gameOverlay) {
        options.showChat = false;
        options.showCoreGui = false;
        options.topInset = 0.0f;
    }
    auto ui = lunar::ui::GameUi::create(context, lunar::render::OffscreenRenderer::colorFormat, 1, game,
                                        std::move(options));
    if (!ui)
        return std::unexpected(ui.error());
    made->ui_ = std::move(*ui);
    made->ui_->attach(offscreen.renderer());
    made->ui_->core().menuSettings.healthBars = healthBarsFromEnvironment();
    // research: LUNAR_UI_SCALE=<scale> lays the GUI out at that many pixels
    // per GUI pixel, as a window on a HiDPI screen does (a 4K picture at 2
    // is what a 4K monitor at 200% shows).
    if (const char* scale = std::getenv("LUNAR_UI_SCALE"); scale && *scale)
        if (const float k = std::strtof(scale, nullptr); k > 0.0f && k <= 8.0f)
            made->scale_ = k;
    return made;
}

void OffscreenUi::bind(lunar::ClientSession& session)
{
    session.setViewportSize(
        {static_cast<float>(offscreen_->width()) / scale_, static_cast<float>(offscreen_->height()) / scale_});
    bind(session.client());
}

void OffscreenUi::bind(lunar::ScriptContext& context)
{
    ui_->gui().setDeferredInput(true);
    update(0.0);
    context.setTextMeasure([this](std::string_view text, const lunar::Font& font, float size, float width, bool rich) {
        return ui_->measureText(text, font, size, width, rich);
    });
    context.setImageLoadedChecker([this](std::string_view id) { return ui_->gui().imageLoaded(id); });
    context.setGuiHitTest([this](float x, float y) { return ui_->guiObjectsAt(x, y); });
    context.setGuiFocus(
        [this](lunar::TextBox* box, bool capture, bool entered) { ui_->focusTextBox(box, capture, entered); });
    context.setGuiLayoutQuery([this](lunar::Instance* object) { ui_->ensureGuiGeometry(object); });
    context.setGameInvitePromptHandler(ui_->gameInviteHandler());
    ui_->gui().onPageStep = context.guiPageStepper();
}

OffscreenUi::~OffscreenUi()
{
    if (ui_)
        ui_->detach();
}

void OffscreenUi::update(double dt)
{
    const lunar::Vector2 size{static_cast<float>(offscreen_->width()), static_cast<float>(offscreen_->height())};
    // A layout asks for the images it shows; while some were still loading,
    // wait for them and lay out again to draw them.
    for (int pass = 0; pass < 4; ++pass) {
        loading_ = false;
        ui_->update(dt, size, scale_);
        if (!loading_ || !waitForImages_)
            break;
        textures_->wait();
    }
    for (const std::string& id : asked_)
        if (textures_->state(id) == lunar::assets::TextureState::Failed && reported_.insert(id).second)
            std::println("GUI: no image {}: {}", id, textures_->error(id));
}

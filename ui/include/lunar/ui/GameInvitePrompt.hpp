#pragma once

#include <lunar/GameInvite.hpp>
#include <lunar/ui/Gui.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace lunar::ui {

class GameInvitePrompt {
public:
    GameInvitePrompt(FontLibrary& fonts, GlyphAtlas& atlas) : fonts_(&fonts), atlas_(&atlas) {}
    void setImageProvider(ImageProvider images) { images_ = std::move(images); }
    void open(std::shared_ptr<GameInviteSession> session);
    void close();
    bool visible() const { return session_ && !session_->closed(); }
    bool handle(const platform::Event& event);
    void update(double time, Vector2 screen, float scale);
    void draw(DrawList& list);

    RectF panelRect() const { return panel_; }
    RectF searchRect() const { return searchRect_; }
    RectF closeRect() const { return closeRect_; }
    RectF inviteRect(std::int64_t userId) const;
    const std::string& query() const { return search_.text; }

private:
    void filter();
    RectF rowRect(std::size_t index) const;
    std::int64_t targetAt(float x, float y) const;
    void activate(std::int64_t target);
    void text(DrawList& list, std::string_view value, RectF rect, float size, Color3 color, bool bold = false,
              std::optional<RectF> clip = {});

    FontLibrary* fonts_;
    GlyphAtlas* atlas_;
    ImageProvider images_;
    std::shared_ptr<GameInviteSession> session_;
    TextEditor search_;
    std::vector<std::size_t> filtered_;
    std::optional<std::int64_t> pressed_;
    std::int64_t focused_ = -3; // search; -1 close, -2 retry, positive friend id
    Vector2 screen_;
    RectF panel_, searchRect_, rows_, closeRect_, retryRect_;
    float scale_ = 1, scroll_ = 0;
    double time_ = 0;
};

} // namespace lunar::ui

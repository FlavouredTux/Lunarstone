#pragma once

#include <lunar/Classes.hpp>
#include <lunar/platform/Events.hpp>
#include <lunar/ui/DrawList.hpp>
#include <lunar/ui/FontLibrary.hpp>
#include <lunar/ui/Gui.hpp>

#include <deque>

namespace lunar::ui {

class Notifications {
public:
    Notifications(FontLibrary& fonts, GlyphAtlas& atlas) : fonts_(&fonts), atlas_(&atlas) {}
    void post(StarterGui::Notification notification);
    void update(double time, Vector2 screen, float scale);
    void draw(DrawList& list);
    bool handle(const platform::Event& event);
    void setImageProvider(ImageProvider images) { images_ = std::move(images); }
    std::size_t count() const { return pending_.size(); }
    RectF buttonRect(std::size_t notification, std::size_t button) const;

private:
    struct Entry {
        StarterGui::Notification notification;
        double shown = -1;
        std::uint64_t id = 0;
    };
    RectF rect(std::size_t index) const;
    void text(DrawList& list, std::string_view text, RectF rect, float size, bool bold = false);
    FontLibrary* fonts_;
    GlyphAtlas* atlas_;
    ImageProvider images_;
    std::deque<Entry> pending_;
    double time_ = 0;
    Vector2 screen_;
    float scale_ = 1;
    struct Press {
        std::uint64_t id;
        int button;
    };
    std::optional<Press> pressed_;
    std::uint64_t nextId_ = 0;
};

} // namespace lunar::ui

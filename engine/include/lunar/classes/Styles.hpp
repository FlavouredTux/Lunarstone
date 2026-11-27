#pragma once

// Roblox's styling: StyleSheets of StyleRules, linked to a GUI subtree by a
// StyleLink. A rule's Selector picks instances (by class, .tag, #name,
// :state) or makes a child for them (::UICorner), and gives them its
// properties. Included through <lunar/Classes.hpp>.

#include <atomic>
#include <lunar/DataTypes.hpp>
#include <lunar/Value.hpp>
#include <lunar/classes/Core.hpp>

#include <string>
#include <utility>
#include <vector>

namespace lunar {

class StyleRule;
class StyleSheet;

// What holds rules: a StyleSheet, or a StyleRule (its rules apply within it).
class StyleBase : public InstanceImpl<StyleBase, Instance, ClassKind::Abstract> {
public:
    // The rules in order: the list SetStyleRules gave, else the children
    // that are StyleRules.
    std::vector<StyleRule*> styleRules() const;
    void assignStyleRules(std::vector<StyleRule*> rules);
    // Whether SetStyleRules has given a list (the children don't count then).
    bool hasRuleList() const { return hasRuleList_; }

private:
    std::vector<Ref<StyleRule>> ruleList_;
    bool hasRuleList_ = false;
};

class StyleRule final : public InstanceImpl<StyleRule, StyleBase> {
public:
    LUNAR_FIELD(std::string, selector, Selector)
    LUNAR_FIELD(int, priority, Priority, 0)

    // The properties it gives, in the order they were set; a name set again
    // keeps its place. Values may name a token of the sheet ("$Name").
    const std::vector<std::pair<std::string, Value>>& styleProperties() const { return properties_; }
    const Value* styleProperty(std::string_view name) const;
    // Fires StyleRulePropertyChanged-by-name through eventFired.
    void setStyleProperty(std::string name, Value value);
    void setStyleProperties(std::vector<std::pair<std::string, Value>> properties);

private:
    std::vector<std::pair<std::string, Value>> properties_;
};

class StyleSheet final : public InstanceImpl<StyleSheet, StyleBase> {
public:
    // The sheets this one builds on (SetDerives), whose rules come first.
    std::vector<StyleSheet*> derives() const;
    void assignDerives(std::vector<StyleSheet*> sheets);

private:
    std::vector<Ref<StyleSheet>> derives_;
};

// A condition on its parent's size ("container query"): while the parent's
// AbsoluteSize is within MinSize..MaxSize (and its aspect ratio within
// AspectRatioRange), it's active, and selectors' "@Name" parts match the
// parent. The StyleEngine works out IsActive as it styles.
class StyleQuery final : public InstanceImpl<StyleQuery, Instance> {
public:
    LUNAR_FIELD(Vector2, minSize, MinSize, Vector2(0.0f, 0.0f))
    LUNAR_FIELD(Vector2, maxSize, MaxSize, Vector2(1e9f, 1e9f))
    LUNAR_FIELD(NumberRange, aspectRatioRange, AspectRatioRange, NumberRange(0.0f, 1e9f))
    LUNAR_PROPERTY() bool isActive() const { return active_; }
    void assignActive(bool active)
    {
        if (active == active_)
            return;
        active_ = active;
        changed.fire("IsActive");
    }

private:
    bool active_ = false;
};

// Applies its StyleSheet to its parent and everything under it.
class StyleLink final : public InstanceImpl<StyleLink, Instance> {
public:
    StyleLink() { live_.fetch_add(1, std::memory_order_relaxed); }
    StyleLink(const StyleLink& other) : InstanceImpl(other) { live_.fetch_add(1, std::memory_order_relaxed); }
    ~StyleLink() override { live_.fetch_sub(1, std::memory_order_relaxed); }
    LUNAR_REF(StyleSheet, styleSheet, StyleSheet)

    // How many there are, anywhere: with none, StyleEngine::apply has
    // nothing to look for.
    static std::int64_t live() { return live_.load(std::memory_order_relaxed); }

private:
    static inline std::atomic<std::int64_t> live_ = 0;
};

// Inside a StyleSheet: another sheet whose rules it takes on.
class StyleDerive final : public InstanceImpl<StyleDerive, Instance> {
public:
    LUNAR_REF(StyleSheet, styleSheet, StyleSheet)
    LUNAR_FIELD(int, priority, Priority, 0)
};

} // namespace lunar

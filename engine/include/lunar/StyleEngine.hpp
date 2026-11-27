#pragma once

// Applies StyleSheets (classes/Styles.hpp) to the GUI they're linked into:
// each StyleLink's sheet styles the link's parent and everything under it.
// A rule's Selector picks the instances it styles, and its properties are
// set on them as if assigned (tokens, "$Name", read from the sheet's
// attributes), except those the instance has set itself, which win. What
// styling set comes back to the default when no rule styles it any more; "::UICorner"-style pseudo instances are
// children made and dropped as rules need them.
//
// Selectors, the part Roblox's Lua app writes: a compound of ClassName,
// .Tag, #Name, @Query and :State parts (state parts never match yet: no
// hover or press styling), joined by ">" (child of) or ">>" or spaces
// (descendant of), optionally ending in "::Class" (a pseudo instance) and
// " #Name" (its name); alternatives are separated by commas. A StyleQuery
// is active while its parent's size meets it (worked out each pass, from
// the last layout).

#include <lunar/Instance.hpp>
#include <lunar/Signal.hpp>
#include <lunar/Value.hpp>

#include <cstddef>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace lunar {

class StyleRule;
class StyleSheet;

namespace styles {

struct Compound {
    std::string className;
    std::vector<std::string> tags;
    std::string name;
    std::vector<std::string> states;
    std::vector<std::string> queries; // "@Name": a StyleQuery child of that name is active
};

// One alternative: compounds right to left are the subject first, then
// what it must be a child (direct) or descendant of.
struct Selector {
    std::vector<Compound> compounds;  // left to right as written
    std::vector<bool> direct;         // between compounds i and i+1: ">" (true) or " " (false)
    std::string pseudoClass;          // "::UICorner": style a child of this class made for it
    std::string pseudoName;           // " #Name" after the pseudo class
};

// Nothing when the text isn't a selector this understands.
std::optional<std::vector<Selector>> parseSelector(std::string_view text);

// Whether the selector's subject matches `instance` (pseudo part aside).
bool matches(const Selector& selector, const Instance& instance);

} // namespace styles

class StyleEngine {
public:
    StyleEngine() = default;
    StyleEngine(const StyleEngine&) = delete;
    StyleEngine& operator=(const StyleEngine&) = delete;
    ~StyleEngine();

    // One pass: every StyleLink under the roots, its sheet applied.
    void apply(std::span<Instance* const> roots);
    // How many properties styling holds now (tests, research).
    std::size_t styledCount() const;

private:
    struct Styled {
        std::weak_ptr<Instance> instance;
        Value own;    // the instance's value without styling
        Value styled; // what styling set
        bool seen = false;
    };
    using Key = std::pair<const Instance*, std::string>;
    std::map<Key, Styled> styled_;
    // Pseudo instances made, by owner, class and name.
    std::map<std::tuple<const Instance*, std::string, std::string>, std::weak_ptr<Instance>> pseudo_;
    struct Parsed {
        std::string text;
        std::vector<styles::Selector> selectors;
    };
    std::unordered_map<const StyleRule*, Parsed> parsed_;

    // Properties set on an instance itself, which styling leaves alone, as
    // Roblox's does (Foundation's Text gives an icon its TextSize and the
    // "x-default-text-size" tag's 8 only as a fallback): ones that weren't
    // the class's default when the engine first saw them, and ones written
    // since by anyone but the engine, until ResetPropertyToDefault gives
    // them back (it fires eventFired under its own name, with the property).
    bool isExplicit(Instance& target, const std::string& property, const Value& current);
    void watch(Instance& target);
    struct Watch {
        std::weak_ptr<Instance> instance;
        Connection written, reset;
        void disconnect()
        {
            written.disconnect();
            reset.disconnect();
        }
    };
    std::set<Key> explicit_;
    std::map<const Instance*, Watch> watched_;
    std::map<std::pair<std::string, std::string>, std::optional<Value>> defaults_; // by class, property
    bool applying_ = false; // the engine's own writes, which don't make a property explicit
};

} // namespace lunar

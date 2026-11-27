#include <lunar/StyleEngine.hpp>

#include <lunar/ClassRegistry.hpp>
#include <lunar/Classes.hpp>
#include <lunar/Error.hpp>

#include <algorithm>
#include <functional>
#include <set>
#include <utility>

namespace lunar {

namespace styles {

namespace {

bool identStart(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool identChar(char c)
{
    return identStart(c) || (c >= '0' && c <= '9') || c == '-';
}

bool compoundMatches(const Compound& c, const Instance& instance)
{
    if (!c.className.empty() && !instance.isA(c.className))
        return false;
    for (const std::string& tag : c.tags)
        if (!instance.hasTag(tag))
            return false;
    if (!c.name.empty() && instance.name() != c.name)
        return false;
    for (const std::string& query : c.queries) {
        const auto* found = dynamic_cast<const StyleQuery*>(instance.findFirstChild(query));
        if (!found || !found->isActive())
            return false;
    }
    // GUESS: no :Hover or :Press styling yet; a rule with a state never applies.
    return c.states.empty();
}

bool matchFrom(const Selector& selector, std::size_t index, const Instance& instance)
{
    if (!compoundMatches(selector.compounds[index], instance))
        return false;
    if (index == 0)
        return true;
    if (selector.direct[index - 1]) {
        const Instance* parent = instance.parent();
        return parent && matchFrom(selector, index - 1, *parent);
    }
    for (const Instance* above = instance.parent(); above; above = above->parent())
        if (matchFrom(selector, index - 1, *above))
            return true;
    return false;
}

} // namespace

std::optional<std::vector<Selector>> parseSelector(std::string_view text)
{
    std::vector<Selector> out;
    std::size_t i = 0;
    auto space = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n'))
            ++i;
    };
    auto ident = [&] {
        const std::size_t start = i;
        while (i < text.size() && identChar(text[i]))
            ++i;
        return std::string(text.substr(start, i - start));
    };
    for (;;) {
        Selector selector;
        bool combinator = false; // a ">" waiting for its right side
        space();
        while (i < text.size() && text[i] != ',') {
            if (text.substr(i).starts_with("::")) {
                if (selector.compounds.empty() || combinator)
                    return std::nullopt;
                i += 2;
                selector.pseudoClass = ident();
                if (selector.pseudoClass.empty())
                    return std::nullopt;
                space();
                if (i < text.size() && text[i] == '#') {
                    ++i;
                    selector.pseudoName = ident();
                    if (selector.pseudoName.empty())
                        return std::nullopt;
                    space();
                }
                if (i < text.size() && text[i] != ',')
                    return std::nullopt; // the pseudo part ends the alternative
                break;
            }
            if (text[i] == '>') {
                if (selector.compounds.empty() || combinator)
                    return std::nullopt;
                // ">" a child of, ">>" a descendant of (as a space is).
                const bool descendant = text.substr(i).starts_with(">>");
                i += descendant ? 2 : 1;
                space();
                if (descendant) {
                    if (i >= text.size() || text[i] == ',')
                        return std::nullopt;
                    continue;
                }
                combinator = true;
                continue;
            }
            Compound compound;
            bool any = false;
            while (i < text.size()) {
                const char c = text[i];
                std::vector<std::string>* list = nullptr;
                if (c == '.')
                    list = &compound.tags;
                else if (c == '@')
                    list = &compound.queries;
                else if (c == ':' && !text.substr(i).starts_with("::"))
                    list = &compound.states;
                if (list) {
                    ++i;
                    list->push_back(ident());
                    if (list->back().empty())
                        return std::nullopt;
                } else if (c == '#') {
                    ++i;
                    compound.name = ident();
                    if (compound.name.empty())
                        return std::nullopt;
                } else if (identStart(c) && !any) {
                    compound.className = ident();
                } else {
                    break;
                }
                any = true;
            }
            if (!any)
                return std::nullopt;
            if (!selector.compounds.empty())
                selector.direct.push_back(combinator);
            selector.compounds.push_back(std::move(compound));
            combinator = false;
            space();
        }
        if (selector.compounds.empty() || combinator)
            return std::nullopt;
        out.push_back(std::move(selector));
        if (i >= text.size())
            return out;
        ++i; // the comma
    }
}

bool matches(const Selector& selector, const Instance& instance)
{
    return !selector.compounds.empty() && matchFrom(selector, selector.compounds.size() - 1, instance);
}

} // namespace styles

namespace {

// The sheets a link's sheet takes rules and tokens from, the ones it derives
// from first (StyleDerive children by priority, then SetDerives' list).
void sheetChain(const StyleSheet& sheet, std::vector<const StyleSheet*>& out, std::set<const StyleSheet*>& visiting)
{
    if (!visiting.insert(&sheet).second)
        return;
    std::vector<const StyleDerive*> derives;
    for (const InstancePtr& child : sheet.children())
        if (const auto* derive = dynamic_cast<const StyleDerive*>(child.get()); derive && derive->styleSheet())
            derives.push_back(derive);
    std::ranges::stable_sort(derives, {}, &StyleDerive::priority);
    for (const StyleDerive* derive : derives)
        sheetChain(*derive->styleSheet(), out, visiting);
    for (const StyleSheet* derived : sheet.derives())
        sheetChain(*derived, out, visiting);
    out.push_back(&sheet);
}

// A "$Name" token from the sheets (the linked sheet first), or the value
// itself; nothing when the token isn't there.
std::optional<Value> resolveToken(const Value& value, std::span<const StyleSheet* const> sheets)
{
    const auto* text = std::get_if<std::string>(&value);
    if (!text || !text->starts_with('$'))
        return value;
    const std::string_view name = std::string_view(*text).substr(1);
    for (auto it = sheets.rbegin(); it != sheets.rend(); ++it) {
        Value found = (*it)->getAttribute(name);
        if (!std::holds_alternative<std::monostate>(found))
            return found;
    }
    return std::nullopt;
}

} // namespace

StyleEngine::~StyleEngine()
{
    for (auto& [instance, watch] : watched_)
        watch.disconnect();
}

void StyleEngine::watch(Instance& target)
{
    auto it = watched_.find(&target);
    if (it != watched_.end() && it->second.instance.lock().get() == &target)
        return;
    if (it != watched_.end()) {
        // A gone instance's, at the same address: what was known of it goes.
        it->second.disconnect();
        std::erase_if(explicit_, [&](const Key& key) { return key.first == &target; });
        std::erase_if(styled_, [&](const auto& entry) { return entry.first.first == &target; });
        watched_.erase(it);
    }
    Watch watch{.instance = target.weak_from_this()};
    watch.written = target.changed.connect([this, at = &target](std::string_view property) {
        if (!applying_)
            explicit_.emplace(at, std::string(property));
    });
    // Reset: neither the instance's own nor styled any more; seen afresh, at
    // the class's default, the next pass styles it.
    watch.reset = target.eventFired.connect(
        [this, at = &target](std::string_view event, std::span<const Value> arguments) {
            if (event != "ResetPropertyToDefault" || arguments.empty())
                return;
            if (const auto* property = std::get_if<std::string>(&arguments.front())) {
                const Key key(at, *property);
                explicit_.erase(key);
                styled_.erase(key);
            }
        });
    watched_.emplace(&target, std::move(watch));
}

bool StyleEngine::isExplicit(Instance& target, const std::string& property, const Value& current)
{
    watch(target);
    const Key key(&target, property);
    if (explicit_.contains(key))
        return true;
    if (styled_.contains(key))
        return false; // styled before: a write since would have made it explicit
    // First sight: a value other than the class's default was set by someone.
    auto slot = defaults_.find({std::string(target.className()), property});
    if (slot == defaults_.end()) {
        std::optional<Value> fallback;
        try {
            if (InstancePtr fresh = ClassRegistry::create(target.className()))
                fallback = fresh->get(property);
        } catch (const Error&) {
        }
        slot = defaults_.emplace(std::pair{std::string(target.className()), property}, std::move(fallback)).first;
    }
    if (slot->second && !(current == *slot->second)) {
        explicit_.insert(key);
        return true;
    }
    return false;
}

void StyleEngine::apply(std::span<Instance* const> roots)
{
    // No StyleLink anywhere and nothing styled from before: no pass (it
    // walks every instance under the roots, and a script reading a GUI's
    // AbsolutePosition after a change runs one).
    if (StyleLink::live() == 0 && styled_.empty() && pseudo_.empty() && watched_.empty())
        return;
    for (auto& [key, styled] : styled_)
        styled.seen = false;
    // Instances gone since the last pass: forget them.
    for (auto it = watched_.begin(); it != watched_.end();) {
        if (!it->second.instance.expired()) {
            ++it;
            continue;
        }
        it->second.disconnect();
        std::erase_if(explicit_, [&](const Key& key) { return key.first == it->first; });
        it = watched_.erase(it);
    }
    std::set<std::tuple<const Instance*, std::string, std::string>> pseudoSeen;

    std::vector<StyleLink*> links;
    std::function<void(Instance&)> findLinks = [&](Instance& node) {
        for (const InstancePtr& child : node.children()) {
            if (auto* link = instanceCast<StyleLink>(child.get()); link && link->styleSheet())
                links.push_back(link);
            findLinks(*child);
        }
    };
    for (Instance* root : roots)
        if (root)
            findLinks(*root);

    struct Candidate {
        int priority;
        std::size_t order;
        const StyleRule* rule;
        const styles::Selector* selector;
    };

    for (StyleLink* link : links) {
        Instance* scope = link->parent();
        if (!scope)
            continue;
        std::vector<const StyleSheet*> sheets;
        std::set<const StyleSheet*> visiting;
        sheetChain(*link->styleSheet(), sheets, visiting);

        // The rules, indexed by what their subject needs: a tag, else a class.
        std::unordered_map<std::string, std::vector<Candidate>> byTag, byClass;
        std::vector<Candidate> rest;
        std::size_t order = 0;
        for (const StyleSheet* sheet : sheets)
            for (const StyleRule* rule : sheet->styleRules()) {
                Parsed& parsed = parsed_[rule];
                if (parsed.text != rule->selector() || (parsed.selectors.empty() && !rule->selector().empty())) {
                    parsed.text = rule->selector();
                    parsed.selectors = styles::parseSelector(parsed.text).value_or(std::vector<styles::Selector>{});
                }
                for (const styles::Selector& selector : parsed.selectors) {
                    const Candidate candidate{rule->priority(), order++, rule, &selector};
                    const styles::Compound& subject = selector.compounds.back();
                    if (!subject.tags.empty())
                        byTag[subject.tags.front()].push_back(candidate);
                    else if (!subject.className.empty())
                        byClass[subject.className].push_back(candidate);
                    else
                        rest.push_back(candidate);
                }
            }

        // The queries in the scope, active or not by their parent's size
        // as the last layout left it.
        std::function<void(Instance&)> evaluate = [&](Instance& node) {
            for (const InstancePtr& child : node.children()) {
                if (auto* query = instanceCast<StyleQuery>(child.get())) {
                    bool active = false;
                    if (const auto* parent = instanceCast<const GuiBase2d>(&node)) {
                        const Vector2 size = parent->absoluteSize();
                        const Vector2 lo = query->minSize(), hi = query->maxSize();
                        const float aspect = size.y > 0.0f ? size.x / size.y : 0.0f;
                        const NumberRange range = query->aspectRatioRange();
                        active = size.x >= lo.x && size.y >= lo.y && size.x <= hi.x && size.y <= hi.y
                                 && aspect >= range.min && aspect <= range.max;
                    }
                    query->assignActive(active);
                }
                evaluate(*child);
            }
        };
        evaluate(*scope);

        std::function<void(Instance&)> visit = [&](Instance& instance) {
            std::vector<Candidate> found;
            auto consider = [&](const std::vector<Candidate>& list) {
                for (const Candidate& c : list)
                    if (styles::matches(*c.selector, instance))
                        found.push_back(c);
            };
            for (const std::string& tag : instance.tags())
                if (auto it = byTag.find(tag); it != byTag.end())
                    consider(it->second);
            if (auto it = byClass.find(std::string(instance.className())); it != byClass.end())
                consider(it->second);
            consider(rest);
            if (!found.empty()) {
                std::ranges::sort(found, {}, [](const Candidate& c) { return std::pair(c.priority, c.order); });
                // What each target ends up with: later (higher) rules win.
                std::vector<std::pair<Instance*, std::vector<std::pair<std::string, Value>>>> targets;
                for (const Candidate& c : found) {
                    Instance* target = &instance;
                    if (!c.selector->pseudoClass.empty()) {
                        const auto key = std::tuple<const Instance*, std::string, std::string>(
                            &instance, c.selector->pseudoClass, c.selector->pseudoName);
                        InstancePtr child;
                        if (auto it = pseudo_.find(key); it != pseudo_.end())
                            child = it->second.lock();
                        if (!child || child->parent() != &instance) {
                            try {
                                child = ClassRegistry::create(c.selector->pseudoClass);
                            } catch (const Error&) {
                                child = nullptr; // a class the engine can't make
                            }
                            if (!child)
                                continue;
                            child->setName(c.selector->pseudoName.empty() ? c.selector->pseudoClass
                                                                          : c.selector->pseudoName);
                            child->setArchivable(false);
                            child->setParent(&instance);
                            pseudo_[key] = child;
                        }
                        pseudoSeen.insert(key);
                        target = child.get();
                    }
                    auto slot = std::ranges::find(targets, target, &decltype(targets)::value_type::first);
                    if (slot == targets.end())
                        slot = targets.insert(targets.end(), {target, {}});
                    for (const auto& [property, raw] : c.rule->styleProperties()) {
                        std::optional<Value> value = resolveToken(raw, sheets);
                        if (!value)
                            continue;
                        auto& props = slot->second;
                        if (auto p = std::ranges::find(props, property, &std::pair<std::string, Value>::first);
                            p != props.end())
                            p->second = std::move(*value);
                        else
                            props.emplace_back(property, std::move(*value));
                    }
                }
                for (auto& [target, props] : targets)
                    for (auto& [property, value] : props) {
                        Value current;
                        try {
                            current = target->get(property);
                        } catch (const Error&) {
                            continue; // not a property of this class
                        }
                        if (isExplicit(*target, property, current)) {
                            styled_.erase(Key(target, property)); // its own value stands
                            continue;
                        }
                        const Key key(target, property);
                        auto it = styled_.find(key);
                        if (it != styled_.end() && it->second.instance.lock().get() != target) {
                            styled_.erase(it); // a gone instance's, at the same address
                            it = styled_.end();
                        }
                        if (it == styled_.end())
                            it = styled_.emplace(key, Styled{target->weak_from_this(), current, {}, false}).first;
                        else if (!(current == it->second.styled))
                            it->second.own = current; // set since styling last did
                        it->second.seen = true;
                        applying_ = true;
                        try {
                            target->set(property, value);
                            it->second.styled = target->get(property);
                        } catch (const Error&) {
                            it->second.styled = current; // a value this property can't take
                        }
                        applying_ = false;
                    }
            }
            for (const InstancePtr& child : std::vector(instance.children().begin(), instance.children().end()))
                visit(*child);
        };
        visit(*scope);
    }

    // What no rule styles any more gets its own value back.
    for (auto it = styled_.begin(); it != styled_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        if (InstancePtr instance = it->second.instance.lock()) {
            applying_ = true;
            try {
                if (instance->get(it->first.second) == it->second.styled)
                    instance->set(it->first.second, it->second.own);
            } catch (const Error&) {
            }
            applying_ = false;
        }
        it = styled_.erase(it);
    }
    for (auto it = pseudo_.begin(); it != pseudo_.end();) {
        if (pseudoSeen.contains(it->first)) {
            ++it;
            continue;
        }
        if (InstancePtr child = it->second.lock(); child && !child->destroyed())
            child->destroy();
        it = pseudo_.erase(it);
    }
}

std::size_t StyleEngine::styledCount() const
{
    return styled_.size();
}

} // namespace lunar

#include <lunar/Selector.hpp>

#include <lunar/Property.hpp>

#include <cctype>
#include <charconv>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>

namespace lunar {

namespace {

struct Complex;
using SelectorList = std::vector<Complex>;

// [$Attr], [$Attr = value], [Prop = value].
struct Test {
    std::string name;
    bool attribute = false;
    std::optional<std::string> value; // as written, quotes taken off
};

struct Compound {
    std::string className; // empty for any
    std::vector<std::string> tags;
    std::vector<std::string> names;
    std::vector<Test> tests;
    std::vector<SelectorList> nots;
    std::vector<SelectorList> hases;
    bool never = false; // a pseudo-class only the GUI's state could match
};

enum class Combinator : std::uint8_t { Child, Descendant };

struct Step {
    Combinator before = Combinator::Descendant; // how it hangs off the step before (or the scope)
    Compound compound;
};

struct Complex {
    bool scoped = false; // starts with a combinator: relative to the scope
    std::vector<Step> steps;
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    SelectorList list(char end)
    {
        SelectorList out;
        while (true) {
            out.push_back(complex(end));
            skipSpace();
            if (at(','))
                ++pos_;
            else
                break;
        }
        return out;
    }

    bool done()
    {
        skipSpace();
        return pos_ >= text_.size();
    }
    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    void expect(char c)
    {
        skipSpace();
        if (!at(c))
            fail(std::format("expected '{}'", c));
        ++pos_;
    }
    [[noreturn]] void fail(std::string_view why) const
    {
        throw std::runtime_error(std::format("{} at {} in \"{}\"", why, pos_ + 1, text_));
    }

private:
    bool at(char c) const { return pos_ < text_.size() && text_[pos_] == c; }
    void skipSpace()
    {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])))
            ++pos_;
    }
    static bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; }
    std::string ident()
    {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && identChar(text_[pos_]))
            ++pos_;
        if (start == pos_)
            fail("expected a name");
        return std::string(text_.substr(start, pos_ - start));
    }

    // A combinator here, if there is one: '>' or '>>' (spaces are the
    // caller's to read as a descendant one).
    std::optional<Combinator> combinator()
    {
        skipSpace();
        if (!at('>'))
            return std::nullopt;
        ++pos_;
        if (at('>')) {
            ++pos_;
            return Combinator::Descendant;
        }
        return Combinator::Child;
    }

    Complex complex(char end)
    {
        Complex out;
        Combinator next = Combinator::Descendant;
        if (auto first = combinator()) {
            out.scoped = true;
            next = *first;
        }
        while (true) {
            skipSpace();
            if (pos_ >= text_.size() || at(',') || at(end))
                break;
            out.steps.push_back({next, compound()});
            const std::size_t before = pos_;
            if (auto c = combinator())
                next = *c;
            else
                next = Combinator::Descendant;
            if (pos_ == before && !(pos_ >= text_.size() || at(',') || at(end))
                && !std::isspace(static_cast<unsigned char>(text_[pos_ - 1])))
                fail("unexpected character");
        }
        if (out.steps.empty())
            fail("empty selector");
        return out;
    }

    Compound compound()
    {
        Compound out;
        bool any = false;
        if (pos_ < text_.size() && (std::isalpha(static_cast<unsigned char>(peek())) || peek() == '_')) {
            out.className = ident();
            any = true;
        } else if (at('*')) {
            ++pos_;
            any = true;
        }
        while (pos_ < text_.size()) {
            if (at('.')) {
                ++pos_;
                out.tags.push_back(ident());
            } else if (at('#')) {
                ++pos_;
                out.names.push_back(ident());
            } else if (at('[')) {
                ++pos_;
                out.tests.push_back(test());
            } else if (at(':')) {
                ++pos_;
                const std::string pseudo = ident();
                if (pseudo == "not" || pseudo == "has") {
                    expect('(');
                    SelectorList inner = list(')');
                    expect(')');
                    (pseudo == "not" ? out.nots : out.hases).push_back(std::move(inner));
                } else {
                    out.never = true;
                }
            } else {
                break;
            }
            any = true;
        }
        if (!any)
            fail("expected a selector");
        return out;
    }

    Test test()
    {
        Test out;
        skipSpace();
        if (at('$')) {
            ++pos_;
            out.attribute = true;
        }
        out.name = ident();
        skipSpace();
        if (at('=')) {
            ++pos_;
            skipSpace();
            if (at('"') || at('\'')) {
                const char quote = text_[pos_++];
                const std::size_t start = pos_;
                while (pos_ < text_.size() && text_[pos_] != quote)
                    ++pos_;
                if (pos_ >= text_.size())
                    fail("unterminated string");
                out.value = std::string(text_.substr(start, pos_ - start));
                ++pos_;
            } else {
                const std::size_t start = pos_;
                while (pos_ < text_.size() && text_[pos_] != ']')
                    ++pos_;
                std::string value(text_.substr(start, pos_ - start));
                while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                    value.pop_back();
                out.value = std::move(value);
            }
        }
        expect(']');
        return out;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

bool equals(const Value& value, const std::string& text)
{
    if (const auto* b = std::get_if<bool>(&value))
        return text == (*b ? "true" : "false");
    if (const auto* d = std::get_if<double>(&value)) {
        double parsed = 0.0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        return error == std::errc() && end == text.data() + text.size() && parsed == *d;
    }
    if (const auto* s = std::get_if<std::string>(&value))
        return *s == text;
    return false;
}

bool matches(const Instance& instance, const SelectorList& list, const Instance* scope);

bool matchesCompound(const Instance& instance, const Compound& compound)
{
    if (compound.never)
        return false;
    if (!compound.className.empty() && !instance.isA(compound.className))
        return false;
    for (const std::string& tag : compound.tags)
        if (!instance.hasTag(tag))
            return false;
    for (const std::string& name : compound.names)
        if (instance.name() != name)
            return false;
    for (const Test& test : compound.tests) {
        Value value;
        if (test.attribute) {
            value = instance.getAttribute(test.name);
        } else {
            const Property* property = instance.classInfo().findProperty(test.name);
            if (!property || !property->get)
                return false;
            value = property->get(instance);
        }
        if (std::holds_alternative<std::monostate>(value))
            return false;
        if (test.value && !equals(value, *test.value))
            return false;
    }
    for (const SelectorList& inner : compound.nots)
        if (matches(instance, inner, nullptr))
            return false;
    for (const SelectorList& inner : compound.hases) {
        bool found = false;
        for (const InstancePtr& descendant : instance.descendants())
            if (matches(*descendant, inner, &instance)) {
                found = true;
                break;
            }
        if (!found)
            return false;
    }
    return true;
}

// Whether steps[0..last] match with steps[last] on instance. scope: what a
// scoped selector hangs off (null: no scope, as inside :not).
bool matchesSteps(const Instance& instance, const Complex& complex, std::size_t last, const Instance* scope)
{
    const Step& step = complex.steps[last];
    if (!matchesCompound(instance, step.compound))
        return false;
    const Instance* parent = instance.parent();
    if (last == 0) {
        if (!complex.scoped || !scope)
            return true;
        if (step.before == Combinator::Child)
            return parent == scope;
        return scope->isAncestorOf(&instance);
    }
    if (step.before == Combinator::Child)
        return parent && parent != scope && matchesSteps(*parent, complex, last - 1, scope);
    for (const Instance* above = parent; above && above != scope; above = above->parent())
        if (matchesSteps(*above, complex, last - 1, scope))
            return true;
    return false;
}

bool matches(const Instance& instance, const SelectorList& list, const Instance* scope)
{
    for (const Complex& complex : list)
        if (matchesSteps(instance, complex, complex.steps.size() - 1, scope))
            return true;
    return false;
}

} // namespace

std::expected<std::vector<InstancePtr>, std::string> queryDescendants(const Instance& root, std::string_view selector)
{
    SelectorList list;
    try {
        Parser parser(selector);
        list = parser.list('\0');
        if (!parser.done())
            parser.fail("unexpected character");
    } catch (const std::runtime_error& e) {
        return std::unexpected(std::string(e.what()));
    }
    std::vector<InstancePtr> out;
    for (const InstancePtr& descendant : root.descendants())
        if (matches(*descendant, list, &root))
            out.push_back(descendant);
    return out;
}

} // namespace lunar

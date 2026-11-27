#include <lunar/Xml.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>

namespace lunar::xml {

namespace {

struct Failure {
    std::string message;
    std::size_t offset;
};

constexpr bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void appendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(std::string_view source) : src_(source) {}

    Element document()
    {
        skipMisc();
        if (!at("<"))
            fail("expected a root element");
        Element root = element();
        skipMisc();
        if (!done())
            fail("unexpected content after the root element");
        return root;
    }

private:
    bool done() const { return pos_ >= src_.size(); }
    bool at(std::string_view s) const { return src_.substr(pos_).starts_with(s); }

    [[noreturn]] void fail(std::string message) const { throw Failure{std::move(message), pos_}; }

    void expect(std::string_view s)
    {
        if (!at(s))
            fail(std::format("expected '{}'", s));
        pos_ += s.size();
    }

    void skipWhitespace()
    {
        while (!done() && isSpace(src_[pos_]))
            ++pos_;
    }

    void skipPast(std::string_view terminator, std::string_view what)
    {
        std::size_t end = src_.find(terminator, pos_);
        if (end == std::string_view::npos)
            fail(std::format("unterminated {}", what));
        pos_ = end + terminator.size();
    }

    // Whitespace, the <?xml?> declaration, comments and a doctype between elements.
    void skipMisc()
    {
        for (;;) {
            skipWhitespace();
            if (at("<?"))
                skipPast("?>", "processing instruction");
            else if (at("<!--"))
                skipPast("-->", "comment");
            else if (at("<!DOCTYPE"))
                skipPast(">", "doctype");
            else
                return;
        }
    }

    std::string_view name()
    {
        std::size_t start = pos_;
        while (!done() && !isSpace(src_[pos_]) && !std::string_view("/>=").contains(src_[pos_]))
            ++pos_;
        if (pos_ == start)
            fail("expected a name");
        return src_.substr(start, pos_ - start);
    }

    Element element()
    {
        expect("<");
        Element e;
        e.name = name();

        for (;;) {
            skipWhitespace();
            if (at("/>")) {
                pos_ += 2;
                return e;
            }
            if (at(">")) {
                ++pos_;
                break;
            }
            std::string key(name());
            skipWhitespace();
            expect("=");
            skipWhitespace();
            if (done() || (src_[pos_] != '"' && src_[pos_] != '\''))
                fail("expected a quoted attribute value");
            char quote = src_[pos_++];
            std::size_t end = src_.find(quote, pos_);
            if (end == std::string_view::npos)
                fail("unterminated attribute value");
            std::string value = decode(src_.substr(pos_, end - pos_));
            pos_ = end + 1;
            e.attributes.emplace_back(std::move(key), std::move(value));
        }

        for (;;) {
            if (done())
                fail(std::format("<{}> is never closed", e.name));
            if (at("</")) {
                pos_ += 2;
                if (name() != e.name)
                    fail(std::format("mismatched closing tag, expected </{}>", e.name));
                skipWhitespace();
                expect(">");
                return e;
            }
            if (at("<!--")) {
                skipPast("-->", "comment");
            } else if (at("<![CDATA[")) {
                pos_ += 9;
                std::size_t end = src_.find("]]>", pos_);
                if (end == std::string_view::npos)
                    fail("unterminated CDATA section");
                e.text += src_.substr(pos_, end - pos_);
                pos_ = end + 3;
            } else if (at("<?")) {
                skipPast("?>", "processing instruction");
            } else if (at("<")) {
                e.children.push_back(element());
            } else {
                std::size_t end = std::min(src_.find('<', pos_), src_.size());
                e.text += decode(src_.substr(pos_, end - pos_));
                pos_ = end;
            }
        }
    }

    std::string decode(std::string_view raw) const
    {
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') {
                out += raw[i];
                continue;
            }
            std::size_t semi = raw.find(';', i);
            if (semi == std::string_view::npos)
                fail("unterminated entity");
            std::string_view entity = raw.substr(i + 1, semi - i - 1);
            if (entity == "lt")
                out += '<';
            else if (entity == "gt")
                out += '>';
            else if (entity == "amp")
                out += '&';
            else if (entity == "quot")
                out += '"';
            else if (entity == "apos")
                out += '\'';
            else if (entity.starts_with('#'))
                appendUtf8(out, codePoint(entity.substr(1)));
            else
                fail(std::format("unknown entity &{};", entity));
            i = semi;
        }
        return out;
    }

    std::uint32_t codePoint(std::string_view digits) const
    {
        int base = 10;
        if (digits.starts_with('x') || digits.starts_with('X')) {
            base = 16;
            digits.remove_prefix(1);
        }
        std::uint32_t cp = 0;
        auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), cp, base);
        if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size() || cp > 0x10FFFF)
            fail(std::format("bad character reference &#{};", digits));
        return cp;
    }

    std::string_view src_;
    std::size_t pos_ = 0;
};

} // namespace

const std::string* Element::attribute(std::string_view key) const
{
    for (const auto& [k, v] : attributes)
        if (k == key)
            return &v;
    return nullptr;
}

const Element* Element::child(std::string_view childName) const
{
    for (const Element& c : children)
        if (c.name == childName)
            return &c;
    return nullptr;
}

std::expected<Element, ParseError> parse(std::string_view document)
{
    try {
        return Parser(document).document();
    } catch (const Failure& failure) {
        auto before = document.substr(0, std::min(failure.offset, document.size()));
        return std::unexpected(ParseError{failure.message, 1 + static_cast<int>(std::ranges::count(before, '\n'))});
    }
}

} // namespace lunar::xml

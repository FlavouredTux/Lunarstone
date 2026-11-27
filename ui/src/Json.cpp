#include "Json.hpp"

#include <lunar/ui/Font.hpp>

#include <charconv>
#include <cstdint>

namespace lunar::ui::json {

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Value> document()
    {
        auto value = parseValue(0);
        skipSpace();
        if (!value || pos_ != text_.size())
            return std::nullopt;
        return value;
    }

private:
    void skipSpace()
    {
        while (pos_ < text_.size()
               && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool consume(char c)
    {
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool literal(std::string_view word)
    {
        if (text_.substr(pos_, word.size()) != word)
            return false;
        pos_ += word.size();
        return true;
    }

    std::optional<std::string> parseString()
    {
        if (!consume('"'))
            return std::nullopt;
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"')
                return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size())
                return std::nullopt;
            const char e = text_[pos_++];
            switch (e) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'u': {
                std::uint32_t cp = 0;
                if (pos_ + 4 > text_.size())
                    return std::nullopt;
                auto [end, ec] = std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, cp, 16);
                if (ec != std::errc{} || end != text_.data() + pos_ + 4)
                    return std::nullopt;
                pos_ += 4;
                appendUtf8(out, cp);
                break;
            }
            default:
                out += e;
            }
        }
        return std::nullopt;
    }

    std::optional<Value> parseValue(int depth)
    {
        if (depth > 64)
            return std::nullopt;
        skipSpace();
        if (pos_ >= text_.size())
            return std::nullopt;
        const char c = text_[pos_];
        if (c == '{') {
            ++pos_;
            auto object = std::make_shared<Object>();
            if (consume('}'))
                return Value{object};
            do {
                auto key = parseString();
                if (!key || !consume(':'))
                    return std::nullopt;
                auto value = parseValue(depth + 1);
                if (!value)
                    return std::nullopt;
                (*object)[*key] = std::move(*value);
            } while (consume(','));
            if (!consume('}'))
                return std::nullopt;
            return Value{object};
        }
        if (c == '[') {
            ++pos_;
            auto array = std::make_shared<Array>();
            if (consume(']'))
                return Value{array};
            do {
                auto value = parseValue(depth + 1);
                if (!value)
                    return std::nullopt;
                array->push_back(std::move(*value));
            } while (consume(','));
            if (!consume(']'))
                return std::nullopt;
            return Value{array};
        }
        if (c == '"') {
            auto s = parseString();
            if (!s)
                return std::nullopt;
            return Value{std::move(*s)};
        }
        if (literal("true"))
            return Value{true};
        if (literal("false"))
            return Value{false};
        if (literal("null"))
            return Value{};
        double number = 0;
        auto [end, ec] = std::from_chars(text_.data() + pos_, text_.data() + text_.size(), number);
        if (ec != std::errc{})
            return std::nullopt;
        pos_ = static_cast<std::size_t>(end - text_.data());
        return Value{number};
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

} // namespace

std::optional<Value> parse(std::string_view text)
{
    return Parser(text).document();
}

} // namespace lunar::ui::json

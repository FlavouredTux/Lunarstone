#pragma once

// Minimal test framework: TEST registers a case, CHECK* throw on failure.

#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lt {

struct TestCase {
    std::string_view name;
    void (*fn)();
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(std::string_view name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct Failure {
    std::string message;
};

// Thrown by SKIP: the test can't run here (no GPU, say), which isn't a failure.
struct Skipped {
    std::string reason;
};

[[noreturn]] inline void fail(std::string_view message, std::source_location loc)
{
    throw Failure{std::format("{}:{}: {}", loc.file_name(), loc.line(), message)};
}

template <class T>
std::string show(const T& value)
{
    if constexpr (std::is_pointer_v<T>)
        return std::format("{}", static_cast<const void*>(value));
    else if constexpr (std::formattable<T, char>)
        return std::format("{}", value);
    else
        return "<unprintable>";
}

template <class A, class B>
void checkEq(const A& a, const B& b, std::string_view exprA, std::string_view exprB,
             std::source_location loc = std::source_location::current())
{
    if (!(a == b))
        fail(std::format("CHECK_EQ({}, {}) failed: {} != {}", exprA, exprB, show(a), show(b)), loc);
}

} // namespace lt

// NOLINTBEGIN(bugprone-macro-parentheses): name is always an identifier
#define TEST(name)                                                                                                     \
    static void name();                                                                                                \
    static const ::lt::Registrar name##_registrar{#name, &name};                                                       \
    static void name()
// NOLINTEND(bugprone-macro-parentheses)

#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__))                                                                                            \
            ::lt::fail("CHECK(" #__VA_ARGS__ ") failed", std::source_location::current());                             \
    } while (0)

#define SKIP(reason)                                                                                                   \
    throw ::lt::Skipped                                                                                                \
    {                                                                                                                  \
        reason                                                                                                         \
    }

#define CHECK_EQ(a, b) ::lt::checkEq((a), (b), #a, #b)

#define CHECK_THROWS(expr, Exception)                                                                                  \
    do {                                                                                                               \
        bool thrown_ = false;                                                                                          \
        try {                                                                                                          \
            (void)(expr);                                                                                              \
        } catch (const Exception&) {                                                                                   \
            thrown_ = true;                                                                                            \
        }                                                                                                              \
        if (!thrown_)                                                                                                  \
            ::lt::fail("CHECK_THROWS(" #expr ", " #Exception ") did not throw", std::source_location::current());      \
    } while (0)

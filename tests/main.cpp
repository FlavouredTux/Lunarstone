#include "framework.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <print>
#include <string_view>

namespace {

int run(int argc, char** argv)
{
    std::string_view filter = argc > 1 ? argv[1] : "";
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    for (const auto& test : lt::registry()) {
        if (!filter.empty() && !test.name.contains(filter))
            continue;
        try {
            test.fn();
            ++passed;
        } catch (const lt::Skipped& skip) {
            ++skipped;
            std::println(stderr, "SKIP {}: {}", test.name, skip.reason);
        } catch (const lt::Failure& failure) {
            ++failed;
            std::println(stderr, "FAIL {}\n  {}", test.name, failure.message);
        } catch (const std::exception& e) {
            ++failed;
            std::println(stderr, "FAIL {}\n  unexpected exception: {}", test.name, e.what());
        }
    }

    if (skipped > 0)
        std::println("{} passed, {} failed, {} skipped", passed, failed, skipped);
    else
        std::println("{} passed, {} failed", passed, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace

// Usage: lunarstone_tests [name-filter]
int main(int argc, char** argv)
{
    // No test may see a real Roblox login: drop the variables that carry one
    // and point the config dir (where the default cookie file is) at nothing.
    // Before any thread starts, so changing the environment is safe.
    for (const char* name : {"LUNAR_ROBLOSECURITY", "ROBLOSECURITY", "LUNAR_ROBLOX_API_KEY"})
        unsetenv(name);                                                   // NOLINT(concurrency-mt-unsafe)
    setenv("XDG_CONFIG_HOME", "/nonexistent/lunarstone-tests-config", 1); // NOLINT(concurrency-mt-unsafe)
    // Nor write decoded textures or the console's web answers into the real
    // caches: the tests that keep them set a directory of their own.
    setenv("LUNAR_DECODED_CACHE", "off", 1); // NOLINT(concurrency-mt-unsafe)
    setenv("LUNAR_HTTP_CACHE", "off", 1);    // NOLINT(concurrency-mt-unsafe)
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fputs(e.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}

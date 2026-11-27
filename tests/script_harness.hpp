#pragma once

// A game plus a script context whose output lands in `lines`, for the script tests.

#include "framework.hpp"

#include <lunar/Classes.hpp>
#include <lunar/script/ScriptContext.hpp>

#include <format>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

// Joins message pieces; keeps long expected messages out of single literals.
template <class... Parts>
std::string concat(const Parts&... parts)
{
    std::string text;
    (text.append(parts), ...);
    return text;
}

struct Harness {
    std::shared_ptr<lunar::DataModel> game = lunar::DataModel::create();
    lunar::ScriptContext context{game};
    std::vector<std::string> lines;

    Harness()
    {
        context.setOutput([this](lunar::MessageType type, std::string_view message) {
            switch (type) {
            case lunar::MessageType::Output:
                lines.emplace_back(message);
                break;
            case lunar::MessageType::Warning:
                lines.push_back(std::format("warn: {}", message));
                break;
            case lunar::MessageType::Error:
                lines.push_back(std::format("error: {}", message));
                break;
            case lunar::MessageType::Info:
                break;
            }
        });
    }

    void run(std::string_view source, std::source_location loc = std::source_location::current())
    {
        if (auto result = context.run(source, "Test"); !result)
            lt::fail(std::format("script failed: {}", result.error().message), loc);
    }

    std::string error(std::string_view source, std::source_location loc = std::source_location::current())
    {
        auto result = context.run(source, "Test");
        if (result)
            lt::fail("script was expected to fail", loc);
        return result.error().message;
    }

    // Runs source and returns what it printed, one line per print.
    std::vector<std::string> output(std::string_view source, std::source_location loc = std::source_location::current())
    {
        lines.clear();
        run(source, loc);
        return lines;
    }

    // Runs source the way the menu's Luau tab does: full access, with the
    // globals an executor offers and no game script can see.
    void console(std::string_view source, std::source_location loc = std::source_location::current())
    {
        auto result = context.run(source, "Console", nullptr, lunar::RunAccess::Full);
        if (!result)
            lt::fail(std::format("console code failed: {}", result.error().message), loc);
    }

    std::string consoleError(std::string_view source, std::source_location loc = std::source_location::current())
    {
        auto result = context.run(source, "Console", nullptr, lunar::RunAccess::Full);
        if (result)
            lt::fail("console code was expected to fail", loc);
        return result.error().message;
    }

    // The single line source printed.
    std::string print(std::string_view source, std::source_location loc = std::source_location::current())
    {
        auto printed = output(source, loc);
        if (printed.size() != 1)
            lt::fail(std::format("expected one line of output, got {}", printed.size()), loc);
        return printed[0];
    }

    void steps(int count, double dt = 1.0 / 60.0)
    {
        for (int i = 0; i < count; ++i)
            context.step(dt);
    }
};

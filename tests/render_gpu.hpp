#pragma once

// Shared Vulkan context for the GPU tests. GPU_TEST skips when there is no
// usable device and fails on any validation error the test caused.

#include "framework.hpp"

#include <lunar/render/Context.hpp>

#include <format>
#include <memory>
#include <string>

namespace lt {

// The contexts live until the process ends and are never destroyed: with
// two validation-layer instances alive at exit (gpu() and
// rayTracingGpu()), tearing the second down aborts inside the layer.
inline lunar::render::Context& gpu()
{
    static auto* context = new auto(lunar::render::Context::create({.validation = true}));
    if (!*context)
        SKIP(std::format("no Vulkan device: {}", context->error()));
    return ***context;
}

// One with ray queries, or null when the device has none.
inline lunar::render::Context* rayTracingGpu()
{
    static auto* made = new auto(lunar::render::Context::create({.validation = true, .rayTracing = true}));
    return *made && (**made)->rayTracing() ? (*made)->get() : nullptr;
}

inline void checkNoValidationErrors(lunar::render::Context& context)
{
    auto errors = context.takeValidationErrors();
    if (!errors.empty())
        throw Failure{std::format("{} Vulkan validation error(s), first: {}", errors.size(), errors.front())};
}

} // namespace lt

// NOLINTBEGIN(bugprone-macro-parentheses): name is always an identifier
#define GPU_TEST(name)                                                                                                 \
    static void name##_gpu(::lunar::render::Context& context);                                                         \
    TEST(name)                                                                                                         \
    {                                                                                                                  \
        ::lunar::render::Context& context = ::lt::gpu();                                                               \
        context.takeValidationErrors();                                                                                \
        name##_gpu(context);                                                                                           \
        ::lt::checkNoValidationErrors(context);                                                                        \
    }                                                                                                                  \
    static void name##_gpu(::lunar::render::Context& context)
// NOLINTEND(bugprone-macro-parentheses)

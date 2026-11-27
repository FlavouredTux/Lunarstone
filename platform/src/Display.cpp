#include <lunar/platform/Display.hpp>

#include <wayland-client.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace lunar::platform {

namespace {

struct Query {
    wl_registry* registry = nullptr;
    std::vector<wl_output*> outputs;
    std::vector<double> rates; // per output, 0 until its current mode arrives
};

constexpr wl_output_listener outputListener{
    .geometry = [](void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, const char*,
                   const char*, std::int32_t) {},
    .mode =
        [](void* data, wl_output* output, std::uint32_t flags, std::int32_t, std::int32_t, std::int32_t refresh) {
            auto* query = static_cast<Query*>(data);
            const auto it = std::ranges::find(query->outputs, output);
            if ((flags & WL_OUTPUT_MODE_CURRENT) && it != query->outputs.end() && refresh > 0)
                query->rates[static_cast<std::size_t>(it - query->outputs.begin())] = refresh / 1000.0; // mHz
        },
    .done = [](void*, wl_output*) {},
    .scale = [](void*, wl_output*, std::int32_t) {},
    .name = [](void*, wl_output*, const char*) {},
    .description = [](void*, wl_output*, const char*) {},
};

constexpr wl_registry_listener registryListener{
    .global =
        [](void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version) {
            if (std::string_view(interface) != wl_output_interface.name)
                return;
            auto* query = static_cast<Query*>(data);
            auto* output = static_cast<wl_output*>(
                wl_registry_bind(registry, name, &wl_output_interface, std::min<std::uint32_t>(version, 4)));
            query->outputs.push_back(output);
            query->rates.push_back(0.0);
            wl_output_add_listener(output, &outputListener, query);
        },
    .global_remove = [](void*, wl_registry*, std::uint32_t) {},
};

} // namespace

std::vector<double> displayRefreshRates()
{
    wl_display* display = wl_display_connect(nullptr);
    if (!display)
        return {};
    Query query;
    query.registry = wl_display_get_registry(display);
    wl_registry_add_listener(query.registry, &registryListener, &query);
    // The first roundtrip binds the outputs, the second gets their modes.
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    for (wl_output* output : query.outputs)
        wl_output_get_version(output) >= WL_OUTPUT_RELEASE_SINCE_VERSION ? wl_output_release(output)
                                                                         : wl_output_destroy(output);
    wl_registry_destroy(query.registry);
    wl_display_disconnect(display);
    std::erase(query.rates, 0.0);
    return query.rates;
}

} // namespace lunar::platform

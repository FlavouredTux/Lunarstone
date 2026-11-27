#include <lunar/render/Context.hpp>

#include <algorithm>
#include <cstdio>
#if defined(__SANITIZE_ADDRESS__)
#include <dlfcn.h>
#include <link.h>
#endif
#include <cstring>
#include <format>
#include <print>
#include <ranges>
#include <string>
#include <string_view>

namespace lunar::render {

namespace {

constexpr const char* validationLayer = "VK_LAYER_KHRONOS_validation";

bool hasLayer(std::string_view name)
{
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::ranges::any_of(layers, [&](const VkLayerProperties& layer) { return name == layer.layerName; });
}

bool hasInstanceExtension(std::string_view name)
{
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    return std::ranges::any_of(extensions, [&](const VkExtensionProperties& ext) { return name == ext.extensionName; });
}

bool hasDeviceExtension(VkPhysicalDevice device, std::string_view name)
{
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    return std::ranges::any_of(extensions, [&](const VkExtensionProperties& ext) { return name == ext.extensionName; });
}

VkDebugUtilsMessengerCreateInfoEXT messengerInfo(void* user, PFN_vkDebugUtilsMessengerCallbackEXT callback)
{
    return {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity
        = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = callback,
        .pUserData = user,
    };
}

// Under AddressSanitizer: keeps every loaded library (the driver among them)
// mapped after vkDestroyInstance, so LeakSanitizer can still name the driver's
// own leaks at exit and tools/lsan.supp can suppress them.
void keepDriversLoaded()
{
#if defined(__SANITIZE_ADDRESS__)
    dl_iterate_phdr(
        [](dl_phdr_info* info, std::size_t, void*) {
            if (info->dlpi_name && info->dlpi_name[0] != '\0')
                dlopen(info->dlpi_name, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
            return 0;
        },
        nullptr);
#endif
}

} // namespace

#if defined(__SANITIZE_ADDRESS__)
// LeakSanitizer picks this up by name. The NVIDIA driver keeps a few small
// allocations past vkDestroyInstance; nothing else is suppressed.
// NOLINTBEGIN(bugprone-reserved-identifier): names the sanitizer runtime looks for
extern "C" const char* __lsan_default_suppressions();
extern "C" const char* __lsan_default_suppressions()
{
    return "leak:libnvidia-glcore.so\n" "leak:libnvidia-glsi.so\n";
}
extern "C" const char* __lsan_default_options();
extern "C" const char* __lsan_default_options()
{
    return "print_suppressions=0";
}
// NOLINTEND(bugprone-reserved-identifier)
#endif

std::string_view toString(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    default:
        return "VkResult(?)";
    }
}

void vkCheck(VkResult result, std::string_view what)
{
    if (result == VK_SUCCESS)
        return;
    std::string message = std::format("{} failed: {} ({})", what, toString(result), static_cast<int>(result));
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY)
        throw OutOfMemoryError(message);
    throw VulkanError(message);
}

std::expected<std::unique_ptr<Context>, std::string> Context::create(const ContextOptions& options)
{
    std::unique_ptr<Context> context(new Context());
    try {
        context->createInstance(options);
        if (options.createSurface) {
            auto surface = options.createSurface(context->instance_);
            if (!surface)
                return std::unexpected("creating the surface failed: " + surface.error());
            context->surface_ = *surface;
        }
        context->rayTracingWanted_ = options.rayTracing;
        context->pickPhysicalDevice();
        context->createDevice();
    } catch (const VulkanError& e) {
        return std::unexpected(e.what());
    }
    return context;
}

Context::~Context()
{
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        fence_.reset();
        commandPool_.reset();
        allocator_.reset();
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_ != VK_NULL_HANDLE) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        destroy(instance_, messenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        keepDriversLoaded();
        vkDestroyInstance(instance_, nullptr);
    }
}

void Context::createInstance(const ContextOptions& options)
{
    std::uint32_t version = 0;
    vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3)
        throw VulkanError(std::format("the Vulkan loader is {}.{}, need 1.3", VK_API_VERSION_MAJOR(version),
                                      VK_API_VERSION_MINOR(version)));

    std::vector<const char*> extensions = options.instanceExtensions;
    std::vector<const char*> layers;
    const bool validation
        = options.validation && hasLayer(validationLayer) && hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (options.validation && !validation)
        std::println(stderr, "lunarstone: Vulkan validation requested but {} is not installed", validationLayer);
    if (validation) {
        layers.push_back(validationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Lunarstone",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "Lunarstone",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    // Chained in so instance creation and destruction get validated too.
    VkDebugUtilsMessengerCreateInfoEXT debugInfo = messengerInfo(this, &debugCallback);
    VkInstanceCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = validation ? &debugInfo : nullptr,
        .pApplicationInfo = &app,
        .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    vkCheck(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");

    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        vkCheck(create(instance_, &debugInfo, nullptr, &messenger_), "vkCreateDebugUtilsMessengerEXT");
    }
}

// The extensions and features ray queries take.
bool Context::supportsRayQueries(VkPhysicalDevice device)
{
    for (const char* name : {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                             VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME})
        if (!hasDeviceExtension(device, name))
            return false;
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &rayQuery};
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                .pNext = &acceleration};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};
    vkGetPhysicalDeviceFeatures2(device, &features);
    // Independent blending: the ray traced opaque pass and decals blend their
    // colour and the denoiser's inputs each their own way (MeshRenderer).
    return rayQuery.rayQuery && acceleration.accelerationStructure && features12.bufferDeviceAddress
           && features12.scalarBlockLayout && features.features.shaderInt64 && features.features.independentBlend;
}

void Context::pickPhysicalDevice()
{
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    int bestScore = -1;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3)
            continue;

        VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                    .pNext = &features13};
        VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};
        vkGetPhysicalDeviceFeatures2(device, &features);
        if (!features13.dynamicRendering || !features13.synchronization2)
            continue;
        // Textures are one big array indexed per instance.
        if (!features12.shaderSampledImageArrayNonUniformIndexing || !features12.descriptorBindingPartiallyBound)
            continue;
        if (surface_ != VK_NULL_HANDLE && !hasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            continue;

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

        std::optional<std::uint32_t> family;
        for (std::uint32_t i = 0; i < familyCount && !family; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                continue;
            VkBool32 present = VK_TRUE;
            if (surface_ != VK_NULL_HANDLE)
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &present);
            if (present)
                family = i;
        }
        if (!family)
            continue;

        int score = 0;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score = 3;
        else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            score = 2;
        else if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
            score = 1;
        if (score > bestScore) {
            bestScore = score;
            physicalDevice_ = device;
            properties_ = properties;
            queueFamily_ = *family;
            queueTimestampValidBits_ = families[*family].timestampValidBits;
            anisotropy_ = features.features.samplerAnisotropy == VK_TRUE;
            textureCompressionBC_ = features.features.textureCompressionBC == VK_TRUE;
            rayTracingSupported_ = supportsRayQueries(device);
        }
    }
    if (physicalDevice_ == VK_NULL_HANDLE)
        throw VulkanError(
            surface_ != VK_NULL_HANDLE
                ? "no Vulkan 1.3 device with dynamic rendering can present to the surface"
                : "no Vulkan 1.3 device with dynamic rendering, synchronization2 and descriptor indexing");
}

void Context::createDevice()
{
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamily_,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    // Demotion is how glslc compiles discard for Vulkan 1.3; 1.3 requires it.
    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .shaderDemoteToHelperInvocation = VK_TRUE,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &features13,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &features12,
        .features = {.samplerAnisotropy = anisotropy_ ? VK_TRUE : VK_FALSE,
                     .textureCompressionBC = textureCompressionBC_ ? VK_TRUE : VK_FALSE},
    };
    std::vector<const char*> extensions;
    // Ray queries, when asked for: acceleration structures to trace against,
    // and buffers addressed from shaders and builds.
    rayTracing_ = rayTracingWanted_ && rayTracingSupported_;
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .rayQuery = VK_TRUE,
    };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &rayQueryFeatures,
        .accelerationStructure = VK_TRUE,
    };
    if (rayTracing_) {
        features13.pNext = &accelerationFeatures;
        features12.bufferDeviceAddress = VK_TRUE;
        features12.scalarBlockLayout = VK_TRUE;
        features.features.shaderInt64 = VK_TRUE;
        features.features.independentBlend = VK_TRUE;
        extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    }
    // How much device memory is free for us, so textures can stay within it.
    const bool memoryBudget = hasDeviceExtension(physicalDevice_, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (memoryBudget)
        extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (surface_ != VK_NULL_HANDLE) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        swapchainMutableFormat_ = hasDeviceExtension(physicalDevice_, VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
        if (swapchainMutableFormat_)
            extensions.push_back(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
    }

    VkDeviceCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    vkCheck(vkCreateDevice(physicalDevice_, &info, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    allocator_ = std::make_unique<Allocator>(physicalDevice_, device_, memoryBudget, rayTracing_);

    VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queueFamily_,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCheck(vkCreateCommandPool(device_, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
    commandPool_ = UniqueCommandPool(device_, pool);

    VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCheck(vkCreateFence(device_, &fenceInfo, nullptr, &fence), "vkCreateFence");
    fence_ = UniqueFence(device_, fence);
}

std::vector<std::string> Context::takeValidationErrors()
{
    std::scoped_lock lock(validationMutex_);
    return std::exchange(validationErrors_, {});
}

void Context::submitAndWait(const std::function<void(VkCommandBuffer)>& record)
{
    VkCommandBufferAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool_.get(),
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(device_, &allocInfo, &cmd), "vkAllocateCommandBuffers");

    try {
        VkCommandBufferBeginInfo begin{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkCheck(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
        record(cmd);
        vkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

        VkCommandBufferSubmitInfo cmdInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd};
        VkSubmitInfo2 submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &cmdInfo,
        };
        VkFence fence = fence_.get();
        vkCheck(vkQueueSubmit2(queue_, 1, &submit, fence), "vkQueueSubmit2");
        vkCheck(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        vkCheck(vkResetFences(device_, 1, &fence), "vkResetFences");
    } catch (...) {
        vkFreeCommandBuffers(device_, commandPool_.get(), 1, &cmd);
        throw;
    }
    vkFreeCommandBuffers(device_, commandPool_.get(), 1, &cmd);
}

void Context::waitIdle() const
{
    vkCheck(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
}

VkBool32 Context::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                                const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    const bool error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    std::println(stderr, "vulkan {}: {}", error ? "error" : "warning", data->pMessage);
    // Loader complaints (a broken layer manifest somewhere) aren't our bugs.
    if (error && (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)) {
        auto* context = static_cast<Context*>(user);
        std::scoped_lock lock(context->validationMutex_);
        context->validationErrors_.emplace_back(data->pMessage);
    }
    return VK_FALSE;
}

} // namespace lunar::render

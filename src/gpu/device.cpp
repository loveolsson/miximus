#include "detail/device.hpp"

#include "detail/external_image_support.hpp"
#include "logger/logger.hpp"
#ifdef MIXIMUS_HAS_CUDA
#include "transfer/detail/cuda_transfer.hpp"
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace miximus::gpu::detail {

void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::format("{} failed: Vulkan result {}", operation, static_cast<int>(result)));
    }
}

VkFormat native_format(format_e format)
{
    switch (format) {
        case format_e::rgba_unorm8:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case format_e::rgba_unorm16:
            return VK_FORMAT_R16G16B16A16_UNORM;
        case format_e::rgba16_float:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case format_e::r32_uint:
            return VK_FORMAT_R32_UINT;
    }

    throw std::invalid_argument("unknown image format");
}

uint32_t texel_bytes(format_e format)
{
    return format == format_e::rgba_unorm16 || format == format_e::rgba16_float ? 8 : 4;
}

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /* message_types */,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*                                       user)
{
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        static_cast<device_state_s*>(user)->errors.fetch_add(1, std::memory_order_relaxed);
    }

    logger::log_error_noexcept("gpu", "Vulkan validation: {}", data->pMessage);
    return VK_FALSE;
}

int device_score(VkPhysicalDeviceType type)
{
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return 3;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return 2;
        default:
            return 1;
    }
}

std::string uuid_string(const uint8_t* uuid)
{
    std::string result;
    for (size_t i = 0; i < VK_UUID_SIZE; ++i) {
        result += std::format("{:02x}", uuid[i]);
    }

    return result;
}

uint32_t describe_queue_families(std::span<const VkQueueFamilyProperties> families, nlohmann::json& entry)
{
    uint32_t selected_queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < families.size(); ++i) {
        entry["queues"].push_back({
            {"flags",          families[i].queueFlags        },
            {"count",          families[i].queueCount        },
            {"timestamp_bits", families[i].timestampValidBits}
        });
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) &&
            selected_queue_family == UINT32_MAX) {
            selected_queue_family = i;
        }
    }

    return selected_queue_family;
}

bool describe_texture_formats(const VolkInstanceTable& instance_vk, VkPhysicalDevice candidate, nlohmann::json& entry)
{
    bool formats_ok = true;
    for (const auto format :
         {format_e::rgba_unorm8, format_e::rgba_unorm16, format_e::rgba16_float, format_e::r32_uint}) {
        VkFormatProperties format_properties{};
        instance_vk.vkGetPhysicalDeviceFormatProperties(candidate, native_format(format), &format_properties);
        VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if (format != format_e::r32_uint) {
            required |=
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        }

        const bool supported = (format_properties.optimalTilingFeatures & required) == required;
        entry["formats"].push_back({
            {"format",           static_cast<int>(format)               },
            {"optimal_features", format_properties.optimalTilingFeatures},
            {"supported",        supported                              }
        });
        // Only formats used by the application belong to its device floor.
        // Float and integer images remain available for callers on capable devices.
        if (format == format_e::rgba_unorm8 || format == format_e::rgba_unorm16) {
            formats_ok &= supported;
        }
    }

    return formats_ok;
}

void describe_memory_types(const VkPhysicalDeviceMemoryProperties& memory_properties, nlohmann::json& entry)
{
    const auto memory_types = std::span(memory_properties.memoryTypes).first(memory_properties.memoryTypeCount);
    const auto memory_heaps = std::span(memory_properties.memoryHeaps);
    for (const auto& type : memory_types) {
        entry["memory_types"].push_back({
            {"flags",      type.propertyFlags               },
            {"heap",       type.heapIndex                   },
            {"heap_bytes", memory_heaps[type.heapIndex].size}
        });
    }
}

void describe_extensions(std::span<const VkExtensionProperties>              extensions,
                         const VkPhysicalDevicePortabilitySubsetFeaturesKHR& portability,
                         bool                                                has_portability_subset,
                         nlohmann::json&                                     entry)
{
    if (has_portability_subset) {
        entry["portability_subset"] = {
            {"image_view_format_swizzle",          bool(portability.imageViewFormatSwizzle)                },
            {"image_view_format_reinterpretation", bool(portability.imageViewFormatReinterpretation)       },
            {"events",                             bool(portability.events)                                },
            {"required_optional_subset_features",  "none; identity views, triangle lists, no Vulkan events"}
        };
    }

    for (const auto& e : extensions) {
        entry["extensions"].push_back(e.extensionName);
    }
}

void destroy_pipelines(device_state_s& state)
{
    for (const auto& formats : state.pipelines) {
        for (auto pipeline : formats) {
            if (pipeline != nullptr) {
                state.vk.vkDestroyPipeline(state.device, pipeline, nullptr);
            }
        }
    }

    for (const auto& formats : state.mix_pipelines) {
        for (auto pipeline : formats) {
            if (pipeline != nullptr) {
                state.vk.vkDestroyPipeline(state.device, pipeline, nullptr);
            }
        }
    }

    for (auto pipeline : state.conversion_pipelines) {
        if (pipeline != nullptr) {
            state.vk.vkDestroyPipeline(state.device, pipeline, nullptr);
        }
    }
}

} // namespace

void device_state_s::initialize(const device_options_s& configuration)
{
    options = configuration;
    if (options.max_recordings == 0 || options.max_recordings > 64 || options.descriptor_page_size == 0 ||
        options.descriptor_page_size > 4096) {
        throw std::invalid_argument("invalid Vulkan arena limits");
    }

    const bool surface_maintenance_available = initialize_instance();
    const auto device_extensions             = select_physical_device(surface_maintenance_available);
    initialize_logical_device(device_extensions);
    initialize_allocator();
    initialize_submission_timeline();
    initialize_pipelines();
}

bool device_state_s::initialize_instance()
{
    static std::once_flag loader_once;
    std::call_once(loader_once, [] { check(volkInitialize(), "volkInitialize"); });
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        throw std::runtime_error("Vulkan 1.3 loader required");
    }

    uint32_t count{};
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "instance extensions");
    std::vector<VkExtensionProperties> instance_extensions(count);
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, instance_extensions.data()), "instance extensions");
    const auto has_instance_extension = [&](const char* name) {
        return std::ranges::any_of(instance_extensions,
                                   [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
    };

    std::vector<const char*> enabled_extensions;
    std::vector<const char*> layers;

    VkInstanceCreateFlags flags{};
    if (has_instance_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabled_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    if (options.presentation) {
        const auto extensions = glfwGetRequiredInstanceExtensions(&count);
        if ((extensions == nullptr) || count == 0) {
            throw std::runtime_error("GLFW Vulkan surface extensions unavailable; initialize GLFW first");
        }

        enabled_extensions.insert(enabled_extensions.end(), extensions, extensions + count);
        if (has_instance_extension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
            has_instance_extension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
            enabled_extensions.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
            enabled_extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        }
    }

    VkDebugUtilsMessengerCreateInfoEXT debug_info{};
    debug_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_info.pfnUserCallback                   = debug_callback;
    debug_info.pUserData                         = this;
    VkValidationFeatureEnableEXT synchronization = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT      validation{};
    validation.sType                         = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures    = &synchronization;
    validation.pNext                         = &debug_info;
    if (options.validation) {
        check(vkEnumerateInstanceLayerProperties(&count, nullptr), "instance layers");
        std::vector<VkLayerProperties> available(count);
        check(vkEnumerateInstanceLayerProperties(&count, available.data()), "instance layers");
        if (!std::ranges::any_of(available, [](const auto& p) {
                return std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0;
            })) {
            throw std::runtime_error("requested VK_LAYER_KHRONOS_validation is not installed");
        }

        layers.push_back("VK_LAYER_KHRONOS_validation");
        enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        enabled_extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }

    VkApplicationInfo application{};
    application.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Miximus";
    application.apiVersion       = VK_API_VERSION_1_3;

    VkInstanceCreateInfo create{};
    create.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pNext                   = options.validation ? &validation : nullptr;
    create.flags                   = flags;
    create.pApplicationInfo        = &application;
    create.enabledExtensionCount   = static_cast<uint32_t>(enabled_extensions.size());
    create.ppEnabledExtensionNames = enabled_extensions.data();
    create.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    create.ppEnabledLayerNames     = layers.data();
    check(vkCreateInstance(&create, nullptr, &instance), "vkCreateInstance");
    volkLoadInstanceTable(&instance_vk, instance);
    if (options.validation) {
        check(instance_vk.vkCreateDebugUtilsMessengerEXT(instance, &debug_info, nullptr, &debug), "debug messenger");
    }

    return has_instance_extension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
}

std::vector<const char*>
device_state_s::select_cuda_extensions([[maybe_unused]] std::span<const uint8_t, VK_UUID_SIZE> uuid,
                                       [[maybe_unused]] std::span<const VkExtensionProperties> extensions)
{
    std::vector<const char*> device_extensions;
    cuda_missing_support.clear();
#ifdef MIXIMUS_HAS_CUDA
    cuda_device_index = -1;
    if (options.use_cuda) {
        cuda_device_index = transfer::detail::find_cuda_device(uuid, cuda_missing_support);
        if (cuda_device_index >= 0) {
            for (const auto* extension :
                 {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME}) {
                if (!std::ranges::any_of(extensions, [&](const auto& available) {
                        return std::strcmp(available.extensionName, extension) == 0;
                    })) {
                    cuda_missing_support.emplace_back(extension);
                }
            }
            if (!cuda_missing_support.empty()) {
                cuda_device_index = -1;
            }
        }
    }
    cuda_external_memory = cuda_device_index >= 0;
    if (cuda_external_memory) {
        device_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        device_extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    }
#else
    cuda_missing_support.emplace_back("CUDA support was not compiled into this build");
#endif
    return device_extensions;
}

std::vector<const char*> device_state_s::select_physical_device(bool surface_maintenance_available)
{
    uint32_t count{};
    check(instance_vk.vkEnumeratePhysicalDevices(instance, &count, nullptr), "physical devices");
    std::vector<VkPhysicalDevice> devices(count);
    check(instance_vk.vkEnumeratePhysicalDevices(instance, &count, devices.data()), "physical devices");
    nlohmann::json report{
        {"devices",    nlohmann::json::array()},
        {"validation", options.validation     },
        {"api_floor",  "1.3"                  }
    };

    // Collect every candidate's diagnostics, but commit capabilities only for
    // the best eligible device. An explicit UUID still goes through the same checks.
    int                                best_score = -1;
    std::vector<const char*>           device_extensions;
    std::string                        requested_device_uuid = options.device_uuid;
    std::vector<VkExtensionProperties> selected_extensions;
    std::erase(requested_device_uuid, '-');
    for (const auto candidate : devices) {
        VkPhysicalDeviceIDProperties identity{};
        identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;

        VkPhysicalDeviceProperties2 device_properties{};
        device_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        device_properties.pNext = &identity;
        instance_vk.vkGetPhysicalDeviceProperties2(candidate, &device_properties);

        VkPhysicalDevicePresentWaitFeaturesKHR present_wait_features{};
        present_wait_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;

        VkPhysicalDevicePresentIdFeaturesKHR present_id_features{};
        present_id_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
        present_id_features.pNext = &present_wait_features;

        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{};
        maintenance.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        maintenance.pNext = &present_id_features;

        VkPhysicalDeviceVulkan13Features vulkan13_features{};
        vulkan13_features.pNext = &maintenance;
        vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

        VkPhysicalDeviceVulkan12Features vulkan12_features{};
        vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12_features.pNext = &vulkan13_features;

        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &vulkan12_features;
        instance_vk.vkGetPhysicalDeviceFeatures2(candidate, &features);
        instance_vk.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        instance_vk.vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        check(instance_vk.vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr),
              "device extensions");
        std::vector<VkExtensionProperties> extensions(count);
        check(instance_vk.vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, extensions.data()),
              "device extensions");
        const auto has_device_extension = [&](const char* name) {
            return std::ranges::any_of(extensions,
                                       [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
        };

        VkPhysicalDevicePortabilitySubsetFeaturesKHR portability{};
        portability.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
        if (has_device_extension("VK_KHR_portability_subset")) {
            VkPhysicalDeviceFeatures2 subset{};
            subset.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            subset.pNext = &portability;
            instance_vk.vkGetPhysicalDeviceFeatures2(candidate, &subset);
        }

        nlohmann::json entry{
            {"name",                device_properties.properties.deviceName   },
            {"uuid",                uuid_string(identity.deviceUUID)          },
            {"vendor_id",           device_properties.properties.vendorID     },
            {"device_id",           device_properties.properties.deviceID     },
            {"api_version",         device_properties.properties.apiVersion   },
            {"driver_version",      device_properties.properties.driverVersion},
            {"dynamic_rendering",   bool(vulkan13_features.dynamicRendering)  },
            {"synchronization2",    bool(vulkan13_features.synchronization2)  },
            {"timeline_semaphores", bool(vulkan12_features.timelineSemaphore) },
            {"queues",              nlohmann::json::array()                   },
            {"formats",             nlohmann::json::array()                   },
            {"memory_types",        nlohmann::json::array()                   },
            {"extensions",          nlohmann::json::array()                   }
        };

        describe_extensions(extensions, portability, has_device_extension("VK_KHR_portability_subset"), entry);

        const auto selected_queue_family = describe_queue_families(families, entry);

        const bool formats_ok = describe_texture_formats(instance_vk, candidate, entry);

        // Memory topology is diagnostic here; allocations select a type from the chosen device.
        VkPhysicalDeviceMemoryProperties memory_properties{};
        instance_vk.vkGetPhysicalDeviceMemoryProperties(candidate, &memory_properties);
        describe_memory_types(memory_properties, entry);

        const bool supported =
            device_properties.properties.apiVersion >= VK_API_VERSION_1_3 &&
            (vulkan13_features.dynamicRendering != 0U) && (vulkan13_features.synchronization2 != 0U) &&
            (vulkan12_features.timelineSemaphore != 0U) && selected_queue_family != UINT32_MAX && formats_ok &&
            (!options.presentation || has_device_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        entry["supported"] = supported;
        report["devices"].push_back(entry);
        const int score = device_score(device_properties.properties.deviceType);

        if (!supported || score <= best_score ||
            (!requested_device_uuid.empty() && requested_device_uuid != uuid_string(identity.deviceUUID))) {
            continue;
        }

        physical     = candidate;
        properties   = device_properties.properties;
        memory       = memory_properties;
        queue_family = selected_queue_family;

        VkFormatProperties working_features{};
        instance_vk.vkGetPhysicalDeviceFormatProperties(candidate, VK_FORMAT_R16G16B16A16_UNORM, &working_features);
        buffer_conversion = (features.features.shaderStorageImageExtendedFormats != 0U) &&
                            ((working_features.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0U);
        best_score             = score;
        separate_present_queue = options.presentation && families[selected_queue_family].queueCount > 1;
        swapchain_maintenance  = options.presentation && (maintenance.swapchainMaintenance1 != 0U) &&
                                has_device_extension(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) &&
                                surface_maintenance_available;
        present_wait = options.presentation && present_wait_features.presentWait != 0U &&
                       present_id_features.presentId != 0U &&
                       has_device_extension(VK_KHR_PRESENT_WAIT_EXTENSION_NAME) &&
                       has_device_extension(VK_KHR_PRESENT_ID_EXTENSION_NAME);
        device_extensions = select_cuda_extensions(identity.deviceUUID, extensions);
        if (options.presentation) {
            device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }

        if (swapchain_maintenance) {
            device_extensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        }

        if (present_wait) {
            device_extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            device_extensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        }

        if (has_device_extension("VK_KHR_portability_subset")) {
            device_extensions.push_back("VK_KHR_portability_subset");
        }

        report["selected_uuid"] = uuid_string(identity.deviceUUID);
        if (options.external_image_import) {
            selected_extensions = std::move(extensions);
        }
    }

    enable_external_image_import(selected_extensions, device_extensions);
    report["external_image_import"] = {
        {"requested",          external_image_import.requested         },
        {"enabled",            external_image_import.enabled           },
        {"enabled_extensions", external_image_import.enabled_extensions},
        {"missing_support",    external_image_import.missing_support   },
    };
    report["buffer_conversion"]      = buffer_conversion;
    report["separate_present_queue"] = separate_present_queue;
    report["swapchain_maintenance"]  = swapchain_maintenance;
    report["present_wait"]           = present_wait;
    report["cuda_external_memory"]   = cuda_external_memory;
    report["cuda_requested"]         = options.use_cuda;
    report["use_cuda"]               = cuda_external_memory;
    diagnostics                      = report.dump(2);
    if (physical == nullptr) {
        throw std::runtime_error("No matching Vulkan device meets the feature/format floor: " + diagnostics);
    }

    return device_extensions;
}

void device_state_s::enable_external_image_import(std::span<const VkExtensionProperties> selected_extensions,
                                                  std::vector<const char*>&              device_extensions)
{
    // Import capability never participates in device eligibility or scoring.
    // Keep its owned extension strings alive through vkCreateDevice, and avoid
    // duplicates with the independently requested CUDA extension set.
    std::vector<std::string_view> import_extensions;
    import_extensions.reserve(selected_extensions.size());
    for (const auto& extension : selected_extensions) {
        import_extensions.emplace_back(extension.extensionName);
    }
    external_image_import = probe_external_image_import(options.external_image_import, import_extensions);
    for (const auto& extension : external_image_import.enabled_extensions) {
        if (!std::ranges::any_of(device_extensions,
                                 [&extension](const char* existing) { return extension == existing; })) {
            device_extensions.push_back(extension.c_str());
        }
    }
}

void device_state_s::initialize_logical_device(std::span<const char* const> device_extensions)
{
    const std::array<float, 2> priorities{1, 1};

    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount       = separate_present_queue ? 2 : 1;
    queue_info.pQueuePriorities = priorities.data();

    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{};
    maintenance.sType                 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
    maintenance.swapchainMaintenance1 = static_cast<VkBool32>(swapchain_maintenance);

    VkPhysicalDevicePresentWaitFeaturesKHR present_wait_features{};
    present_wait_features.sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    present_wait_features.presentWait = static_cast<VkBool32>(present_wait);

    VkPhysicalDevicePresentIdFeaturesKHR present_id_features{};
    present_id_features.sType     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    present_id_features.presentId = static_cast<VkBool32>(present_wait);
    present_id_features.pNext     = &present_wait_features;
    maintenance.pNext             = present_wait ? &present_id_features : nullptr;

    VkPhysicalDeviceVulkan13Features vulkan13_features{};
    if (swapchain_maintenance) {
        vulkan13_features.pNext = &maintenance;
    } else if (present_wait) {
        vulkan13_features.pNext = &present_id_features;
    }
    vulkan13_features.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13_features.dynamicRendering = VK_TRUE;
    vulkan13_features.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features vulkan12_features{};
    vulkan12_features.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12_features.pNext             = &vulkan13_features;
    vulkan12_features.timelineSemaphore = VK_TRUE;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &vulkan12_features;

    VkPhysicalDeviceFeatures core_features{};
    core_features.shaderStorageImageExtendedFormats = static_cast<VkBool32>(buffer_conversion);
    device_info.pEnabledFeatures                    = &core_features;
    device_info.queueCreateInfoCount                = 1;
    device_info.pQueueCreateInfos                   = &queue_info;
    device_info.enabledExtensionCount               = static_cast<uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames             = device_extensions.data();
    check(instance_vk.vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");
    volkLoadDeviceTable(&vk, device);
    vk.vkGetDeviceQueue(device, queue_family, 0, &queue);
    vk.vkGetDeviceQueue(device, queue_family, separate_present_queue ? 1 : 0, &present_queue);
}

void device_state_s::initialize_allocator()
{
    VmaVulkanFunctions     functions{};
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.instance         = instance;
    allocator_info.physicalDevice   = physical;
    allocator_info.device           = device;
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr   = instance_vk.vkGetDeviceProcAddr;
    allocator_info.pVulkanFunctions = &functions;
    check(vmaCreateAllocator(&allocator_info, &allocator), "VMA allocator");
}

void device_state_s::initialize_submission_timeline()
{
    // Queue acceptance assigns values; the submission worker publishes completed values.
    VkSemaphoreTypeCreateInfo timeline_info{};
    timeline_info.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore.pNext = &timeline_info;
    check(vk.vkCreateSemaphore(device, &semaphore, nullptr, &timeline), "timeline semaphore");
}

uint64_t device_state_s::completed() const { return completed_value.load(std::memory_order_acquire); }

void device_state_s::retire(uint64_t after, std::function<void()> destroy)
{
    if (after == 0) {
        destroy();
        return;
    }

    const std::scoped_lock guard(retire_mutex);
    retired.push_back({after, std::move(destroy)});
}

void device_state_s::collect()
{
    const auto             value = completed();
    std::vector<retired_s> ready;
    {
        const std::scoped_lock guard(retire_mutex);
        for (auto it = retired.begin(); it != retired.end();) {
            if (it->completion <= value) {
                ready.push_back(std::move(*it));
                it = retired.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& item : ready) {
        item.destroy();
    }
}

device_state_s::~device_state_s()
{
    if (device != nullptr) {
        // Only final device teardown waits idle. Recordings never do this.
        vk.vkDeviceWaitIdle(device);
        for (auto& item : retired) {
            item.destroy();
        }

        destroy_pipelines(*this);

        if (conversion_pipeline_layout != nullptr) {
            vk.vkDestroyPipelineLayout(device, conversion_pipeline_layout, nullptr);
        }

        if (conversion_layout != nullptr) {
            vk.vkDestroyDescriptorSetLayout(device, conversion_layout, nullptr);
        }

        if (sampler != nullptr) {
            vk.vkDestroySampler(device, sampler, nullptr);
        }

        if (nearest_sampler != nullptr) {
            vk.vkDestroySampler(device, nearest_sampler, nullptr);
        }

        if (pipeline_layout != nullptr) {
            vk.vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }

        if (texture_layout != nullptr) {
            vk.vkDestroyDescriptorSetLayout(device, texture_layout, nullptr);
        }

        if (timeline != nullptr) {
            vk.vkDestroySemaphore(device, timeline, nullptr);
        }

        if (allocator != nullptr) {
            vmaDestroyAllocator(allocator);
        }

        vk.vkDestroyDevice(device, nullptr);
    }

    if (debug != nullptr) {
        instance_vk.vkDestroyDebugUtilsMessengerEXT(instance, debug, nullptr);
    }

    if (instance != nullptr) {
        instance_vk.vkDestroyInstance(instance, nullptr);
    }
}

void resource_state_s::check_host_access() const
{
    if (recording_uses.load() != 0 || last_use_timeline_value.load() > owner->completed()) {
        throw std::logic_error("host access before GPU/recording release");
    }
}

} // namespace miximus::gpu::detail

namespace miximus::gpu {

using detail::check;

namespace {
void allocate_external_memory(detail::device_state_s&     device,
                              const VkMemoryRequirements& requirements,
                              VkImage                     image,
                              VkBuffer                    buffer,
                              VkDeviceMemory&             memory)
{
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t index = 0; index < device.memory.memoryTypeCount; ++index) {
        if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
            (std::span(device.memory.memoryTypes)[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memory_type = index;
            break;
        }
    }
    if (memory_type == UINT32_MAX) {
        throw std::runtime_error("CUDA shared resource has no device-local memory type");
    }

    // Importers need the complete dedicated allocation, never a VMA suballocation.
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image  = image;
    dedicated.buffer = buffer;

    VkExportMemoryAllocateInfo export_info{};
    export_info.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.pNext       = &dedicated;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkMemoryAllocateInfo allocate{};
    allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.pNext           = &export_info;
    allocate.allocationSize  = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    check(device.vk.vkAllocateMemory(device.device, &allocate, nullptr, &memory), "allocate CUDA shared resource");
}
} // namespace

device_s::device_s(const device_options_s& options)
    : state_(std::make_shared<detail::device_state_s>())
{
    state_->initialize(options);

    auto report = nlohmann::json::parse(state_->diagnostics);
#ifdef MIXIMUS_HAS_CUDA
    if (state_->cuda_external_memory) {
        // CUDA is only a candidate until every required resource imports. This
        // constructor has not exposed the device or started any stream workers.
        state_->cuda_missing_support              = transfer::detail::qualify_cuda_transfers(*this);
        report["cuda_transfer_formats_qualified"] = state_->cuda_missing_support.empty();
        if (!state_->cuda_missing_support.empty()) {
            state_->cuda_external_memory   = false;
            state_->cuda_device_index      = -1;
            report["cuda_external_memory"] = false;
            report["use_cuda"]             = false;
        }
    }
#endif
    report["cuda_missing_support"] = state_->cuda_missing_support;
    state_->diagnostics            = report.dump(2);

    // Report the final selection once, after examining only the selected device.
    // Stream allocation and per-frame transfer paths do not repeat this message.
    if (!options.use_cuda) {
        getlog("gpu")->warn("CUDA transfers disabled: --use-cuda was not specified");
    } else if (!state_->cuda_external_memory) {
        const auto& reasons = state_->cuda_missing_support;
        if (reasons.size() == 1 && reasons.front() == "CUDA is not supported on this device") {
            getlog("gpu")->warn("CUDA transfers disabled: {}", reasons.front());
        } else {
            std::string details;
            for (const auto& reason : reasons) {
                details += std::format("\n   --- {}", reason);
            }
            getlog("gpu")->warn("CUDA transfers disabled due to missing support:{}", details);
        }
    } else {
        getlog("gpu")->info("CUDA transfers enabled");
    }

    default_context_ = std::make_unique<recording_context_s>(create_recording_context(options.max_recordings));
    state_->start_submission_worker();
}

device_s::~device_s()
{
    state_->stop_submission_worker();
    default_context_.reset();
}

std::string device_s::diagnostics_json() const
{
    auto report = nlohmann::json::parse(state_->diagnostics);

    VmaTotalStatistics statistics{};
    vmaCalculateStatistics(state_->allocator, &statistics);
    report["memory"] = {
        {"allocation_count", statistics.total.statistics.allocationCount},
        {"allocation_bytes", statistics.total.statistics.allocationBytes},
        {"block_bytes",      statistics.total.statistics.blockBytes     }
    };

    return report.dump(2);
}

uint64_t                        device_s::validation_errors() const noexcept { return state_->errors.load(); }
bool                            device_s::uses_cuda_transfers() const noexcept { return state_->cuda_external_memory; }
external_image_import_support_s device_s::external_image_import_support() const
{
    return state_->external_image_import;
}
void device_s::collect() { state_->collect(); }

texture_s device_s::create_texture(extent_s extent, format_e format, sampling_e sampling, resource_sharing_e sharing)
{
    if ((extent.width == 0U) || (extent.height == 0U) || extent.width > state_->properties.limits.maxImageDimension2D ||
        extent.height > state_->properties.limits.maxImageDimension2D) {
        throw std::invalid_argument("invalid image extent");
    }

    if (format == format_e::r32_uint && sampling == sampling_e::mipmapped_linear) {
        throw std::invalid_argument("integer transfer images cannot have filtered mipmaps");
    }

    const uint32_t levels =
        sampling == sampling_e::mipmapped_linear ? std::bit_width(std::max(extent.width, extent.height)) : 1;
    if (levels > 1) {
        VkFormatProperties properties{};
        state_->instance_vk.vkGetPhysicalDeviceFormatProperties(
            state_->physical, detail::native_format(format), &properties);
        constexpr auto required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & required) != required) {
            throw std::runtime_error("image format does not support filtered mipmap blits");
        }
    }

    auto image        = std::make_shared<detail::texture_state_s>();
    image->owner      = state_;
    image->extent     = extent;
    image->format     = format;
    image->sampling   = sampling;
    image->mip_levels = levels;
    image->layouts.resize(levels, VK_IMAGE_LAYOUT_UNDEFINED);

    VkImageCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType   = VK_IMAGE_TYPE_2D;
    info.format      = detail::native_format(format);
    info.extent      = {.width = extent.width, .height = extent.height, .depth = 1};
    info.mipLevels   = levels;
    info.arrayLayers = 1;
    info.samples     = VK_SAMPLE_COUNT_1_BIT;
    info.tiling      = VK_IMAGE_TILING_OPTIMAL;
    info.usage       = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (format == format_e::rgba_unorm16 && state_->buffer_conversion) {
        info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }

    if (sharing == resource_sharing_e::cuda) {
        if (!state_->cuda_external_memory) {
            throw std::runtime_error("CUDA shared images require external-memory support");
        }

        VkPhysicalDeviceExternalImageFormatInfo external_format{};
        external_format.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        external_format.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkPhysicalDeviceImageFormatInfo2 query{};
        query.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        query.pNext  = &external_format;
        query.format = info.format;
        query.type   = info.imageType;
        query.tiling = info.tiling;
        query.usage  = info.usage;
        VkExternalImageFormatProperties external_properties{};
        external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        properties.pNext = &external_properties;
        check(state_->instance_vk.vkGetPhysicalDeviceImageFormatProperties2(state_->physical, &query, &properties),
              "query CUDA shared image format");
        if ((external_properties.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0U) {
            throw std::runtime_error("CUDA shared image format is not exportable");
        }

        VkExternalMemoryImageCreateInfo external{};
        external.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external.handleTypes = external_format.handleType;
        info.pNext           = &external;
        check(state_->vk.vkCreateImage(state_->device, &info, nullptr, &image->image), "create CUDA shared image");
        VkMemoryRequirements requirements{};
        state_->vk.vkGetImageMemoryRequirements(state_->device, image->image, &requirements);
        allocate_external_memory(*state_, requirements, image->image, VK_NULL_HANDLE, image->external_memory);
        check(state_->vk.vkBindImageMemory(state_->device, image->image, image->external_memory, 0),
              "bind CUDA shared image");
        image->external_allocation_bytes = requirements.size;
    } else {
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        check(vmaCreateImage(state_->allocator, &info, &allocation, &image->image, &image->allocation, nullptr),
              "allocate image");
    }

    VkImageViewCreateInfo view{};
    view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image            = image->image;
    view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view.format           = info.format;
    view.subresourceRange = {.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel   = 0,
                             .levelCount     = 1,
                             .baseArrayLayer = 0,
                             .layerCount     = 1};
    check(state_->vk.vkCreateImageView(state_->device, &view, nullptr, &image->view), "image view");
    image->sampled_view = image->view;
    if (levels > 1) {
        view.subresourceRange.levelCount = levels;
        check(state_->vk.vkCreateImageView(state_->device, &view, nullptr, &image->sampled_view),
              "mipmapped image view");
    }

    return texture_s(std::move(image));
}

buffer_s device_s::create_buffer(size_t bytes, host_access_e access, size_t alignment, resource_sharing_e sharing)
{
    if ((bytes == 0U) || (alignment == 0U) || (alignment & (alignment - 1)) != 0) {
        throw std::invalid_argument("invalid buffer size/alignment");
    }

    auto buffer    = std::make_shared<detail::buffer_state_s>();
    buffer->owner  = state_;
    buffer->bytes  = bytes;
    buffer->access = access;

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size  = bytes;
    info.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    if (sharing == resource_sharing_e::cuda) {
        if (!state_->cuda_external_memory || access != host_access_e::device_only) {
            throw std::invalid_argument("CUDA shared buffers require device-only external memory");
        }
        VkPhysicalDeviceExternalBufferInfo query{};
        query.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
        query.usage      = info.usage;
        query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkExternalBufferProperties properties{};
        properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
        state_->instance_vk.vkGetPhysicalDeviceExternalBufferProperties(state_->physical, &query, &properties);
        if ((properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ==
            0U) {
            throw std::runtime_error("CUDA shared storage buffer is not exportable");
        }
        VkExternalMemoryBufferCreateInfo external{};
        external.sType          = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        external.handleTypes    = query.handleType;
        info.pNext              = &external;
        buffer->external_buffer = true;
        check(state_->vk.vkCreateBuffer(state_->device, &info, nullptr, &buffer->buffer), "create CUDA shared buffer");
        VkMemoryRequirements requirements{};
        state_->vk.vkGetBufferMemoryRequirements(state_->device, buffer->buffer, &requirements);
        allocate_external_memory(*state_, requirements, VK_NULL_HANDLE, buffer->buffer, buffer->external_memory);
        check(state_->vk.vkBindBufferMemory(state_->device, buffer->buffer, buffer->external_memory, 0),
              "bind CUDA shared buffer");
        buffer->info = {.bytes = static_cast<size_t>(requirements.size), .device_local = true};
        return buffer_s(std::move(buffer));
    }

    VmaAllocationCreateInfo allocation{};
    allocation.usage =
        access == host_access_e::device_only ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    if (access != host_access_e::device_only) {
        allocation.flags =
            VMA_ALLOCATION_CREATE_MAPPED_BIT |
            (access == host_access_e::sequential_write ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                                       : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
    }

    VmaAllocationInfo mapped{};
    check(vmaCreateBufferWithAlignment(
              state_->allocator, &info, &allocation, alignment, &buffer->buffer, &buffer->allocation, &mapped),
          "allocate buffer");
    buffer->mapped   = mapped.pMappedData;
    const auto flags = std::span(state_->memory.memoryTypes)[mapped.memoryType].propertyFlags;
    buffer->info     = {
            .bytes         = static_cast<size_t>(mapped.size),
            .device_local  = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0,
            .host_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0,
            .host_cached   = (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0,
    };

    if ((buffer->mapped != nullptr) && reinterpret_cast<uintptr_t>(buffer->mapped) % alignment != 0) {
        throw std::runtime_error("mapped allocation cannot meet requested host alignment");
    }

    return buffer_s(std::move(buffer));
}

} // namespace miximus::gpu

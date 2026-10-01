#include "external_image_win.hpp"

#include "device.hpp"
#include "resource.hpp"

#include <bit>
#include <cstring>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <format>
#include <stdexcept>
#include <wrl/client.h>

namespace miximus::gpu::detail {
namespace {

using Microsoft::WRL::ComPtr;

void check_d3d(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        throw std::runtime_error(std::format("{} failed: HRESULT {:#x}", operation, static_cast<uint32_t>(result)));
    }
}

} // namespace

struct external_image_platform_s
{
    ComPtr<ID3D11Device1> device;

    explicit external_image_platform_s(const device_state_s& owner)
    {
        VkPhysicalDeviceIDProperties identity{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2  properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &identity,
        };
        owner.instance_vk.vkGetPhysicalDeviceProperties2(owner.physical, &properties);
        if (identity.deviceLUIDValid == VK_FALSE) {
            throw std::runtime_error("Vulkan device has no DXGI adapter identity");
        }

        ComPtr<IDXGIFactory1> factory;
        check_d3d(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create DXGI factory");
        ComPtr<IDXGIAdapter1> selected;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const auto            result = factory->EnumAdapters1(index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            check_d3d(result, "enumerate DXGI adapter");
            DXGI_ADAPTER_DESC1 description{};
            check_d3d(adapter->GetDesc1(&description), "describe DXGI adapter");
            if (std::memcmp(&description.AdapterLuid, identity.deviceLUID, VK_LUID_SIZE) == 0) {
                selected = adapter;
                break;
            }
        }
        if (selected.Get() == nullptr) {
            throw std::runtime_error("No DXGI adapter matches the selected Vulkan device");
        }

        ComPtr<ID3D11Device> created;
        check_d3d(D3D11CreateDevice(selected.Get(),
                                    D3D_DRIVER_TYPE_UNKNOWN,
                                    nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    nullptr,
                                    0,
                                    D3D11_SDK_VERSION,
                                    &created,
                                    nullptr,
                                    nullptr),
                  "create external-image D3D11 device");
        check_d3d(created.As(&device), "query D3D11.1 device");
    }
};

namespace {

external_image_platform_s& platform(const std::shared_ptr<device_state_s>& device)
{
    const std::scoped_lock lock(device->external_image_mutex);
    if (!device->external_image_platform) {
        device->external_image_platform = std::make_shared<external_image_platform_s>(*device);
    }
    return *device->external_image_platform;
}

std::shared_ptr<texture_state_s>
import_texture(const std::shared_ptr<device_state_s>& device, const external_image_s& descriptor, bool writable)
{
    if (!device || device->external_image_import != external_image_import_support_e::supported) {
        throw std::runtime_error("D3D11 image sharing is not enabled");
    }
    if (descriptor.handle == nullptr || descriptor.handle == INVALID_HANDLE_VALUE || descriptor.extent.width == 0 ||
        descriptor.extent.height == 0 || descriptor.extent.width > device->properties.limits.maxImageDimension2D ||
        descriptor.extent.height > device->properties.limits.maxImageDimension2D) {
        throw std::invalid_argument("Invalid D3D11 image descriptor");
    }

    // Opening on the LUID-matched device rejects handles from a different adapter.
    ComPtr<ID3D11Texture2D> texture;
    check_d3d(platform(device).device->OpenSharedResource1(descriptor.handle, IID_PPV_ARGS(&texture)),
              "open NT D3D11 texture");
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    const auto expected_format =
        descriptor.order == channel_order_e::rgba ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    if ((descriptor.order != channel_order_e::rgba && descriptor.order != channel_order_e::bgra) ||
        description.Format != expected_format || description.Width != descriptor.extent.width ||
        description.Height != descriptor.extent.height || description.MipLevels != 1 || description.ArraySize != 1 ||
        description.SampleDesc.Count != 1 || description.SampleDesc.Quality != 0 ||
        description.Usage != D3D11_USAGE_DEFAULT || description.CPUAccessFlags != 0 ||
        (description.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) == 0 ||
        (description.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0 ||
        (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
        (writable && (description.BindFlags & D3D11_BIND_RENDER_TARGET) == 0)) {
        throw std::invalid_argument("D3D11 texture does not meet the external-image contract");
    }

    const auto format = descriptor.order == channel_order_e::rgba ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | (writable ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0U);
    VkPhysicalDeviceExternalImageFormatInfo external_query{
        .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
    };
    VkPhysicalDeviceImageFormatInfo2 query{
        .sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext  = &external_query,
        .format = format,
        .type   = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage  = usage,
    };
    VkExternalImageFormatProperties external_properties{.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2        properties{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_properties,
    };
    check(device->instance_vk.vkGetPhysicalDeviceImageFormatProperties2(device->physical, &query, &properties),
          "query D3D11 image import");
    if ((external_properties.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
        throw std::runtime_error("D3D11 image format is not importable by Vulkan");
    }

    auto image                    = std::make_shared<texture_state_s>();
    image->owner                  = device;
    image->extent                 = descriptor.extent;
    image->format                 = format_e::rgba_unorm8;
    image->sampling               = sampling_e::linear;
    image->layouts                = {VK_IMAGE_LAYOUT_GENERAL};
    image->external_memory_handle = native_handle_s::duplicate(descriptor.handle);

    VkExternalMemoryImageCreateInfo external{
        .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
    };
    VkImageCreateInfo info{
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext       = &external,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = format,
        .extent      = {.width = descriptor.extent.width, .height = descriptor.extent.height, .depth = 1},
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .tiling      = VK_IMAGE_TILING_OPTIMAL,
        .usage       = usage,
    };
    check(device->vk.vkCreateImage(device->device, &info, nullptr, &image->image), "create imported D3D11 image");
    VkMemoryRequirements requirements{};
    device->vk.vkGetImageMemoryRequirements(device->device, image->image, &requirements);
    VkMemoryWin32HandlePropertiesKHR handle_properties{.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    check(device->vk.vkGetMemoryWin32HandlePropertiesKHR(
              device->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, descriptor.handle, &handle_properties),
          "query D3D11 memory types");
    const auto memory_types = requirements.memoryTypeBits & handle_properties.memoryTypeBits;
    if (memory_types == 0) {
        throw std::runtime_error("D3D11 texture has no compatible Vulkan memory type");
    }
    VkMemoryDedicatedAllocateInfo dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image->image,
    };
    VkImportMemoryWin32HandleInfoKHR import{
        .sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
        .pNext      = &dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
        .handle     = image->external_memory_handle.get(),
    };
    VkMemoryAllocateInfo allocation{
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &import,
        .allocationSize  = requirements.size,
        .memoryTypeIndex = static_cast<uint32_t>(std::countr_zero(memory_types)),
    };
    check(device->vk.vkAllocateMemory(device->device, &allocation, nullptr, &image->external_memory),
          "import D3D11 memory");
    image->external_allocation_bytes = requirements.size;
    check(device->vk.vkBindImageMemory(device->device, image->image, image->external_memory, 0), "bind D3D11 memory");
    VkImageViewCreateInfo view{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image            = image->image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
    };
    check(device->vk.vkCreateImageView(device->device, &view, nullptr, &image->view), "create D3D11 image view");
    image->sampled_view = image->view;
    return image;
}

} // namespace

std::shared_ptr<texture_state_s> import_external_image(const std::shared_ptr<device_state_s>& device,
                                                       const external_image_s&                descriptor)
{
    return import_texture(device, descriptor, false);
}

std::shared_ptr<texture_state_s> create_external_image(const std::shared_ptr<device_state_s>& device, extent_s extent)
{
    if (!device || device->external_image_import != external_image_import_support_e::supported) {
        throw std::runtime_error("D3D11 image sharing is not enabled");
    }

    if (extent.width == 0 || extent.height == 0 || extent.width > device->properties.limits.maxImageDimension2D ||
        extent.height > device->properties.limits.maxImageDimension2D) {
        throw std::invalid_argument("Invalid external-image dimensions");
    }
    const D3D11_TEXTURE2D_DESC description{
        .Width      = extent.width,
        .Height     = extent.height,
        .MipLevels  = 1,
        .ArraySize  = 1,
        .Format     = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = {.Count = 1},
        .Usage      = D3D11_USAGE_DEFAULT,
        .BindFlags  = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        .MiscFlags  = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED,
    };
    ComPtr<ID3D11Texture2D> texture;
    check_d3d(platform(device).device->CreateTexture2D(&description, nullptr, &texture), "create shared D3D11 texture");
    ComPtr<IDXGIResource1> resource;
    check_d3d(texture.As(&resource), "query shared D3D11 resource");
    HANDLE shared{};
    check_d3d(
        resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared),
        "export D3D11 NT handle");
    native_handle_s handle(shared);
    auto            image = import_texture(device, {.handle = handle.get(), .extent = extent}, true);
    image->layouts        = {VK_IMAGE_LAYOUT_UNDEFINED};
    return image;
}

} // namespace miximus::gpu::detail

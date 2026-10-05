#include "gpu/detail/device.hpp"
#include "gpu/detail/external_image_copy.hpp"
#include "gpu/detail/external_image_export.hpp"
#include "gpu/detail/external_image_win.hpp"
#include "gpu/detail/resource.hpp"
#include "gpu/device.hpp"
#include "logger/logger.hpp"

#include <array>
#include <chrono>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <gtest/gtest.h>
#include <thread>
#include <wrl/client.h>

namespace miximus::gpu { namespace {

using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;

TEST(external_image_windows, DISABLED_RepeatedImportLifetimeStress)
{
    // Explicit long hardware regression: the idle D3D11 validation device used
    // to retain deferred resource destruction and fail near 90,000 imports on
    // NVIDIA/Windows. No GPU submissions or CEF are needed to reproduce it.
    if (!spdlog::get("gpu")) {
        logger::init_loggers(spdlog::level::info);
    }
    auto device = std::make_shared<detail::device_state_s>();
    device->initialize({.validation = true, .external_image_import = true});
    auto                     exported = detail::create_external_image(device, {.width = 1920, .height = 1080});
    detail::external_image_s descriptor{.handle = exported->external_memory_handle.get(), .extent = exported->extent};
    for (int index = 0; index < 360000; ++index) {
        SCOPED_TRACE(index);
        ASSERT_NO_THROW({
            auto imported = detail::import_external_image(device, descriptor);
            imported.reset();
        });
        if (index % 10000 == 0) {
            getlog("gpu")->info("Import stress completed {} iterations", index);
        }
    }
    exported.reset();
    device->collect();
    EXPECT_EQ(device->errors.load(), 0U);
}

TEST(external_image_windows, ImportsCompletedD3D11WritesAndKeepsTheBorrowedHandle)
{
    if (!spdlog::get("gpu")) {
        logger::init_loggers(spdlog::level::warn);
    }
    device_options_s                options{.validation = true, .external_image_import = true};
    device_s                        producer(options);
    device_s                        consumer(options);
    constexpr extent_s              extent{.width = 16, .height = 8};
    detail::external_image_export_s exported(producer, extent);
    auto                            source   = producer.create_texture(extent);
    auto                            commands = producer.try_record();
    ASSERT_TRUE(commands);
    commands->clear(source);
    exported.copy(*commands, source, {.compositing = compositing_e::replace});
    ASSERT_EQ(commands->submit().wait(5s), wait_result_e::ready);
    commands.reset();

    // Open the actual NT texture on its DXGI adapter, then write it using D3D11.
    // No host pixel upload or assumed Vulkan/D3D adapter enumeration order.
    const auto            descriptor = exported.descriptor();
    ComPtr<IDXGIFactory1> factory;
    ASSERT_TRUE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D>     texture;
    for (UINT index = 0; texture.Get() == nullptr; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto            result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        ASSERT_TRUE(SUCCEEDED(result));
        device.Reset();
        context.Reset();
        ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(adapter.Get(),
                                                D3D_DRIVER_TYPE_UNKNOWN,
                                                nullptr,
                                                0,
                                                nullptr,
                                                0,
                                                D3D11_SDK_VERSION,
                                                &device,
                                                nullptr,
                                                &context)));
        ComPtr<ID3D11Device1> device1;
        ASSERT_TRUE(SUCCEEDED(device.As(&device1)));
        device1->OpenSharedResource1(descriptor.handle, IID_PPV_ARGS(&texture));
    }
    ASSERT_TRUE(texture);
    ComPtr<ID3D11RenderTargetView> view;
    ASSERT_TRUE(SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &view)));
    const std::array<float, 4> color{0.25F, 0.5F, 0.75F, 1.0F};
    context->ClearRenderTargetView(view.Get(), color.data());
    const D3D11_QUERY_DESC query_description{.Query = D3D11_QUERY_EVENT};
    ComPtr<ID3D11Query>    query;
    ASSERT_TRUE(SUCCEEDED(device->CreateQuery(&query_description, &query)));
    context->End(query.Get());
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    HRESULT    completed{};
    while ((completed = context->GetData(query.Get(), nullptr, 0, 0)) == S_FALSE &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(completed, S_OK); // Flush alone is not proof of GPU completion.

    auto destination = consumer.create_texture(extent, format_e::rgba_unorm8);
    commands         = consumer.try_record();
    ASSERT_TRUE(commands);
    auto invalid = descriptor;
    invalid.extent.width += 1;
    EXPECT_THROW(detail::external_image_copy_s::submit(*commands, invalid, destination, {}, 100ms),
                 std::invalid_argument);
    const auto copied = detail::external_image_copy_s::submit(
        *commands, descriptor, destination, {.compositing = compositing_e::replace}, 100ms);
    ASSERT_EQ(copied.wait(5s), wait_result_e::ready);
    commands.reset();
    EXPECT_NO_THROW(detail::native_handle_s::duplicate(descriptor.handle));

    auto output = consumer.create_buffer(size_t{extent.width} * extent.height * 4, host_access_e::readback);
    commands    = consumer.try_record();
    ASSERT_TRUE(commands);
    commands->readback(destination, output);
    ASSERT_EQ(commands->submit().wait(5s), wait_result_e::ready);
    commands.reset();
    const auto       pixels = output.readable_bytes();
    const std::array expected{64, 128, 191, 255};
    for (size_t index = 0; index < pixels.size(); ++index) {
        EXPECT_NEAR(std::to_integer<int>(pixels[index]), expected.at(index % 4), 1);
    }
    EXPECT_EQ(producer.validation_errors(), 0U);
    EXPECT_EQ(consumer.validation_errors(), 0U);
}

}} // namespace miximus::gpu

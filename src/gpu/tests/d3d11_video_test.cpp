#include "gpu/detail/d3d11_video_pack.hpp"
#include "gpu/detail/external_video_copy.hpp"
#include "gpu/device.hpp"
#include "logger/logger.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <d3d11_1.h>
#include <gtest/gtest.h>
#include <vector>
#include <wrl/client.h>

namespace miximus::gpu { namespace {

using Microsoft::WRL::ComPtr;

void verify_carrier(DXGI_FORMAT format, uint32_t sample_bytes, uint32_t width = 256, uint32_t height = 256)
{
    SCOPED_TRACE(static_cast<int>(format));
    if (!spdlog::get("gpu")) {
        logger::init_loggers(spdlog::level::info);
    }

    device_s                    device({.validation = true, .external_image_import = true});
    auto                        producer = detail::external_video_copy_s::create_producer_device(device);
    ComPtr<ID3D11DeviceContext> context;
    producer->GetImmediateContext(&context);
    UINT       support{};
    const auto supported = producer->CheckFormatSupport(format, &support);
    if (FAILED(supported) || (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) == 0U) {
        GTEST_SKIP() << "D3D11 adapter does not support this video texture format";
    }

    const uint32_t       stride = width * sample_bytes;
    std::vector<uint8_t> expected(size_t{stride} * height * 3 / 2);
    for (size_t i = 0; i < expected.size() / sample_bytes; ++i) {
        const auto value              = static_cast<uint16_t>(format == DXGI_FORMAT_P010 ? (i & 1023U) << 6 : i);
        expected.at(i * sample_bytes) = static_cast<uint8_t>(value);
        if (sample_bytes == 2) {
            expected.at((i * sample_bytes) + 1) = static_cast<uint8_t>(value >> 8);
        }
    }

    const std::vector<uint8_t>                  empty(expected.size());
    const std::array<D3D11_SUBRESOURCE_DATA, 2> initial{
        {{.pSysMem = empty.data(), .SysMemPitch = stride}, {.pSysMem = expected.data(), .SysMemPitch = stride}}
    };
    const D3D11_TEXTURE2D_DESC source_description{.Width      = width,
                                                  .Height     = height,
                                                  .MipLevels  = 1,
                                                  .ArraySize  = 2,
                                                  .Format     = format,
                                                  .SampleDesc = {.Count = 1},
                                                  .Usage      = D3D11_USAGE_DEFAULT};
    ComPtr<ID3D11Texture2D>    source;
    ASSERT_EQ(producer->CreateTexture2D(&source_description, initial.data(), &source), S_OK);
    video_buffer_header_s header{};
    header.component_count         = 3;
    header.components.at(0).offset = sizeof(header);
    header.components.at(0).stride = stride;
    header.components.at(1).offset = static_cast<uint32_t>(sizeof(header)) + (stride * height);
    header.components.at(1).stride = stride;
    detail::d3d11_video_pack_s pack(producer.Get(),
                                    source_description,
                                    header,
                                    sizeof(header) + expected.size(),
                                    {.width = width, .height = height});
    pack.copy(context.Get(), source.Get(), 1);

    // Test-only readback of the carrier, before color conversion. Production
    // imports this exact allocation into Vulkan and never maps its pixels.
    ComPtr<ID3D11Device1> device1;
    ASSERT_EQ(producer.As(&device1), S_OK);
    ComPtr<ID3D11Texture2D> carrier;
    ASSERT_EQ(device1->OpenSharedResource1(pack.image().handle, IID_PPV_ARGS(&carrier)), S_OK);
    D3D11_TEXTURE2D_DESC readback_description{};
    carrier->GetDesc(&readback_description);
    readback_description.Usage          = D3D11_USAGE_STAGING;
    readback_description.BindFlags      = 0;
    readback_description.MiscFlags      = 0;
    readback_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    ASSERT_EQ(producer->CreateTexture2D(&readback_description, nullptr, &readback), S_OK);
    context->CopyResource(readback.Get(), carrier.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_EQ(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), S_OK);
    std::vector<uint8_t> actual(expected.size());
    const auto           row_bytes = size_t{pack.image().extent.width} * 4;
    for (size_t offset = 0; offset < actual.size(); offset += row_bytes) {
        std::memcpy(actual.data() + offset,
                    static_cast<const uint8_t*>(mapped.pData) + ((offset / row_bytes) * mapped.RowPitch),
                    std::min(row_bytes, actual.size() - offset));
    }
    context->Unmap(readback.Get(), 0);
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(device.validation_errors(), 0);
}

TEST(d3d11_video, PreservesEveryEightBitSampleAndArraySlice) { verify_carrier(DXGI_FORMAT_NV12, 1); }

TEST(d3d11_video, PreservesEveryTenBitSample) { verify_carrier(DXGI_FORMAT_P010, 2); }

TEST(d3d11_video, PreservesEverySixteenBitSample) { verify_carrier(DXGI_FORMAT_P016, 2); }

TEST(d3d11_video, PreservesChromaAcrossPartialCarrierRows)
{
    verify_carrier(DXGI_FORMAT_NV12, 1, 258, 130);
    verify_carrier(DXGI_FORMAT_P010, 2, 258, 130);
}

}} // namespace miximus::gpu

#include "d3d11_video_pack.hpp"

#include <algorithm>
#include <array>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <format>
#include <string_view>
#include <wrl/client.h>

namespace miximus::gpu::detail {
namespace {

using Microsoft::WRL::ComPtr;

void check(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        throw std::runtime_error(std::format("{}: HRESULT {:#x}", operation, static_cast<uint32_t>(result)));
    }
}

// Typed plane views expose the original integer samples as UNORM values.
// Multiplication and rounding recover every 8/16-bit word exactly; no filtering,
// transfer function, chroma resampling, or alpha operation belongs in transport.
constexpr std::string_view PACK_COMPONENTS = R"(
Texture2D<float> luma : register(t0);
Texture2D<float2> chroma : register(t1);
RWTexture2D<float4> carrier : register(u0);
cbuffer Layout : register(b0) {
    uint output_width;
    uint output_height;
    uint payload_bytes;
    uint uv_offset;
    uint y_stride;
    uint uv_stride;
    uint sample_bytes;
    uint y_height;
};
uint read_byte(uint address) {
    if (address >= payload_bytes) return 0;
    bool uv = address >= uv_offset;
    uint offset = uv ? address - uv_offset : address;
    uint stride = uv ? uv_stride : y_stride;
    uint row = offset / stride;
    uint column = (offset % stride) / sample_bytes;
    if (row >= (uv ? (y_height + 1) / 2 : y_height)) return 0;
    float sample = uv ? chroma.Load(int3(column / 2, row, 0))[column % 2]
                      : luma.Load(int3(column, row, 0));
    uint word = uint(round(sample * (sample_bytes == 1 ? 255.0 : 65535.0)));
    return (word >> ((offset % sample_bytes) * 8)) & 255;
}
[numthreads(16, 16, 1)]
void main(uint3 pixel : SV_DispatchThreadID) {
    if (pixel.x >= output_width || pixel.y >= output_height) return;
    uint offset = (pixel.y * output_width + pixel.x) * 4;
    carrier[pixel.xy] = float4(read_byte(offset), read_byte(offset + 1),
                               read_byte(offset + 2), read_byte(offset + 3)) / 255.0;
}
)";

} // namespace

struct d3d11_video_pack_s::impl_s
{
    ComPtr<ID3D11Texture2D>           input;
    ComPtr<ID3D11Texture2D>           carrier;
    ComPtr<ID3D11ShaderResourceView>  luma;
    ComPtr<ID3D11ShaderResourceView>  chroma;
    ComPtr<ID3D11UnorderedAccessView> output;
    ComPtr<ID3D11ComputeShader>       shader;
    ComPtr<ID3D11Buffer>              constants;
    native_handle_s                   handle;
    extent_s                          extent;

    void initialize(ID3D11Device*                device,
                    const D3D11_TEXTURE2D_DESC&  source,
                    const video_buffer_header_s& header,
                    size_t                       bytes,
                    extent_s                     visible)
    {
        const bool wide = source.Format == DXGI_FORMAT_P010 || source.Format == DXGI_FORMAT_P016;
        if ((!wide && source.Format != DXGI_FORMAT_NV12) || bytes <= sizeof(header) ||
            bytes > size_t{256} * 1024 * 1024 || header.component_count != 3 || source.Width < visible.width ||
            source.Height < visible.height) {
            throw std::invalid_argument("Unsupported D3D11 video component layout");
        }

        auto description           = source;
        description.MipLevels      = 1;
        description.ArraySize      = 1;
        description.Usage          = D3D11_USAGE_DEFAULT;
        description.CPUAccessFlags = 0;
        description.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags      = 0;
        check(device->CreateTexture2D(&description, nullptr, &input), "Create video plane copy texture");
        D3D11_SHADER_RESOURCE_VIEW_DESC view{.Format        = wide ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM,
                                             .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D};
        view.Texture2D.MipLevels = 1;
        check(device->CreateShaderResourceView(input.Get(), &view, &luma), "Create luma plane view");
        view.Format = wide ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        check(device->CreateShaderResourceView(input.Get(), &view, &chroma), "Create chroma plane view");

        const auto payload    = static_cast<uint32_t>(bytes - sizeof(header));
        extent.width          = std::min((payload + 3) / 4, 1024U);
        extent.height         = (payload + extent.width * 4 - 1) / (extent.width * 4);
        description.Width     = extent.width;
        description.Height    = extent.height;
        description.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        check(device->CreateTexture2D(&description, nullptr, &carrier), "Create shared video byte carrier");
        check(device->CreateUnorderedAccessView(carrier.Get(), nullptr, &output), "Create video carrier view");
        ComPtr<IDXGIResource1> resource;
        check(carrier.As(&resource), "Query video carrier sharing");
        HANDLE exported{};
        check(resource->CreateSharedHandle(
                  nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &exported),
              "Export video carrier");
        handle = native_handle_s(exported);

        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        const auto       compiled = D3DCompile(PACK_COMPONENTS.data(),
                                               PACK_COMPONENTS.size(),
                                               "video byte transport",
                                               nullptr,
                                               nullptr,
                                               "main",
                                               "cs_5_0",
                                               D3DCOMPILE_ENABLE_STRICTNESS,
                                               0,
                                               &code,
                                               &errors);
        if (FAILED(compiled)) {
            throw std::runtime_error(errors.Get() != nullptr ? static_cast<const char*>(errors->GetBufferPointer())
                                                             : "Compile video byte transport failed");
        }

        check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
              "Create video byte transport shader");
        const std::array<uint32_t, 8> layout{extent.width,
                                             extent.height,
                                             payload,
                                             header.components.at(1).offset - static_cast<uint32_t>(sizeof(header)),
                                             header.components.at(0).stride,
                                             header.components.at(1).stride,
                                             wide ? 2U : 1U,
                                             visible.height};
        const D3D11_BUFFER_DESC       buffer{
            .ByteWidth = sizeof(layout), .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_CONSTANT_BUFFER};
        const D3D11_SUBRESOURCE_DATA initial{.pSysMem = layout.data()};
        check(device->CreateBuffer(&buffer, &initial, &constants), "Create video transport layout");
    }
};

d3d11_video_pack_s::d3d11_video_pack_s(ID3D11Device*                device,
                                       const D3D11_TEXTURE2D_DESC&  source,
                                       const video_buffer_header_s& header,
                                       size_t                       bytes,
                                       extent_s                     visible)
    : impl_(std::make_unique<impl_s>())
{
    impl_->initialize(device, source, header, bytes, visible);
}

d3d11_video_pack_s::~d3d11_video_pack_s() = default;

void d3d11_video_pack_s::copy(ID3D11DeviceContext* context, ID3D11Texture2D* source, UINT slice)
{
    context->CopySubresourceRegion(impl_->input.Get(), 0, 0, 0, 0, source, slice, nullptr);
    std::array<ID3D11ShaderResourceView*, 2> views{impl_->luma.Get(), impl_->chroma.Get()};
    auto*                                    output    = impl_->output.Get();
    auto*                                    constants = impl_->constants.Get();
    context->CSSetShader(impl_->shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetConstantBuffers(0, 1, &constants);
    context->Dispatch((impl_->extent.width + 15) / 16, (impl_->extent.height + 15) / 16, 1);

    views.fill(nullptr);
    output    = nullptr;
    constants = nullptr;
    context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetConstantBuffers(0, 1, &constants);
    context->CSSetShader(nullptr, nullptr, 0);
}

d3d11_image_description_s d3d11_video_pack_s::image() const
{
    return {.handle      = impl_->handle.get(),
            .extent      = impl_->extent,
            .dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .format      = VK_FORMAT_R8G8B8A8_UNORM,
            .usage       = VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
}

size_t d3d11_video_pack_s::buffer_bytes() const
{
    return sizeof(video_buffer_header_s) + (size_t{impl_->extent.width} * impl_->extent.height * 4);
}

} // namespace miximus::gpu::detail

#include "gpu/detail/d3d11_video_pack.hpp"
#include "gpu/detail/external_video_copy.hpp"
#include "gpu/detail/fatal.hpp"
#include "platform_ingest.hpp"

// Windows SDK C++ helpers must be declared outside FFmpeg C linkage.
#include <d3d11.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <dxgi1_2.h>
#include <format>
#include <thread>
#include <wrl/client.h>

namespace miximus::nodes::ffmpeg::detail {
namespace {

using Microsoft::WRL::ComPtr;

void check(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        throw std::runtime_error(std::format("{}: HRESULT {:#x}", operation, static_cast<uint32_t>(result)));
    }
}

struct context_lock_s
{
    AVD3D11VADeviceContext& device;

    explicit context_lock_s(AVD3D11VADeviceContext& context)
        : device(context)
    {
        device.lock(device.lock_ctx);
    }

    ~context_lock_s() { device.unlock(device.lock_ctx); }

    context_lock_s(const context_lock_s&)            = delete;
    context_lock_s& operator=(const context_lock_s&) = delete;
    context_lock_s(context_lock_s&&)                 = delete;
    context_lock_s& operator=(context_lock_s&&)      = delete;
};

} // namespace

int create_platform_decode_device(AVBufferRef** result, AVHWDeviceType type, gpu::device_s* device)
{
    if (type != AV_HWDEVICE_TYPE_D3D11VA || device == nullptr) {
        return av_hwdevice_ctx_create(result, type, nullptr, nullptr, 0);
    }

    auto* reference = av_hwdevice_ctx_alloc(type);
    if (reference == nullptr) {
        return AVERROR(ENOMEM);
    }

    try {
        auto* context          = reinterpret_cast<AVHWDeviceContext*>(reference->data);
        auto* native           = static_cast<AVD3D11VADeviceContext*>(context->hwctx);
        auto  producer         = gpu::detail::external_video_copy_s::create_producer_device(*device);
        native->device         = producer.Detach();
        const auto initialized = av_hwdevice_ctx_init(reference);
        if (initialized < 0) {
            av_buffer_unref(&reference);
            return initialized;
        }
        *result = reference;
        return 0;
    } catch (...) {
        av_buffer_unref(&reference);
        throw;
    }
}

struct platform_ingest_s::impl_s
{
    gpu::device_s&                                      device;
    gpu::recording_context_s                            recordings;
    ComPtr<ID3D11Device>                                producer;
    std::unique_ptr<gpu::detail::d3d11_video_pack_s>    pack;
    ComPtr<ID3D11Query>                                 ready;
    std::unique_ptr<gpu::detail::external_video_copy_s> imported;
    D3D11_TEXTURE2D_DESC                                description{};
    size_t                                              bytes{};
    gpu::vec2i_t                                        visible{};

    explicit impl_s(gpu::device_s& gpu)
        : device(gpu)
        , recordings(gpu.create_recording_context(2))
    {
    }

    ~impl_s()                        = default;
    impl_s(const impl_s&)            = delete;
    impl_s& operator=(const impl_s&) = delete;
    impl_s(impl_s&&)                 = delete;
    impl_s& operator=(impl_s&&)      = delete;

    void initialize(AVD3D11VADeviceContext& context, ID3D11Texture2D* source, decoded_frame_s& decoded)
    {
        const auto           size = decoded.buffer_bytes;
        D3D11_TEXTURE2D_DESC input{};
        source->GetDesc(&input);
        if (pack && producer.Get() == context.device && input.Width == description.Width &&
            input.Height == description.Height && input.Format == description.Format && bytes == size &&
            visible == decoded.dimensions) {
            return;
        }

        imported.reset();
        pack.reset();
        ready.Reset();
        producer    = context.device;
        description = input;
        visible     = decoded.dimensions;
        pack        = std::make_unique<gpu::detail::d3d11_video_pack_s>(
            producer.Get(),
            input,
            decoded.component_header(),
            size,
            gpu::extent_s{.width  = static_cast<uint32_t>(decoded.dimensions.x),
                          .height = static_cast<uint32_t>(decoded.dimensions.y)});
        imported = std::make_unique<gpu::detail::external_video_copy_s>(device, pack->image(), pack->buffer_bytes());

        const D3D11_QUERY_DESC query{.Query = D3D11_QUERY_EVENT};
        check(producer->CreateQuery(&query, &ready), "Create video copy completion query");
        bytes = size;
    }

    void copy(decoded_frame_s& decoded)
    {
        auto* frame = decoded.native_frame();
        if (frame->format != AV_PIX_FMT_D3D11) {
            throw std::runtime_error("Expected D3D11 hardware frame");
        }

        auto*      frames  = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
        auto&      context = *static_cast<AVD3D11VADeviceContext*>(frames->device_ctx->hwctx);
        auto*      source  = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        const auto slice   = static_cast<UINT>(reinterpret_cast<uintptr_t>(frame->data[1]));
        initialize(context, source, decoded);
        {
            context_lock_s lock(context);
            pack->copy(context.device_context, source, slice);
            context.device_context->End(ready.Get());
            context.device_context->Flush();
        }

        // Flush submits work; only the event query proves it finished. Wait on
        // this decode worker, never on the render thread. Keep the AVFrame alive.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            HRESULT result{};
            {
                context_lock_s lock(context);
                result = context.device_context->GetData(ready.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            }
            if (result == S_OK) {
                break;
            }
            if (FAILED(result) || std::chrono::steady_clock::now() >= deadline) {
                gpu::detail::fatal_gpu_error("D3D11 decoded video copy did not complete");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

platform_ingest_s::platform_ingest_s(gpu::device_s& device)
    : impl_(std::make_unique<impl_s>(device))
{
}

platform_ingest_s::~platform_ingest_s() = default;

void platform_ingest_s::convert(decoded_frame_s& frame, const gpu::texture_s& target)
{
    impl_->copy(frame);
    auto record = impl_->recordings.try_record();
    if (!record) {
        throw std::runtime_error("Video conversion recording unavailable");
    }

    const auto complete = impl_->imported->convert(*record, target, frame.component_header(), frame.color);
    if (complete.wait(std::chrono::seconds(30)) != gpu::wait_result_e::ready) {
        gpu::detail::fatal_gpu_error("D3D11 video conversion did not complete");
    }
}

const char* platform_ingest_s::transfer_path() { return "D3D11 shared GPU texture to Vulkan"; }

} // namespace miximus::nodes::ffmpeg::detail

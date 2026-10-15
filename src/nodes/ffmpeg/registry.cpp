#include "registry.hpp"

#include "detail/hardware_devices.hpp"
#include "detail/playback.hpp"
#include "gpu/transfer/detail/memory_budget.hpp"
#include "utils/serial_executor.hpp"

#include <algorithm>
#include <vector>

namespace miximus::nodes::ffmpeg {

struct ffmpeg_registry_s::impl_s
{
    std::shared_ptr<const detail::hardware_devices_s>       hardware;
    gpu::device_s&                                          device;
    std::shared_ptr<gpu::transfer::detail::memory_budget_s> gpu_budget{
        std::make_shared<gpu::transfer::detail::memory_budget_s>(size_t{512} << 20)};
    gpu::transfer::texture_upload_service_s&                 uploads;
    std::vector<std::shared_ptr<detail::playback_session_s>> sessions;
    std::unique_ptr<utils::serial_executor_s>                control{std::make_unique<utils::serial_executor_s>()};

    explicit impl_s(gpu::device_s& gpu, gpu::transfer::texture_upload_service_s& service)
        : hardware(std::make_shared<detail::hardware_devices_s>(gpu))
        , device(gpu)
        , uploads(service)
    {
    }

    impl_s(const impl_s& other)            = delete;
    impl_s& operator=(const impl_s& other) = delete;
    impl_s(impl_s&& other)                 = delete;
    impl_s& operator=(impl_s&& other)      = delete;

    ~impl_s()
    {
        for (const auto& session : sessions) {
            session->stop();
        }

        control.reset();
        sessions.clear();
    }
};

ffmpeg_registry_s::ffmpeg_registry_s(gpu::device_s& device, gpu::transfer::texture_upload_service_s& uploads)
    : impl_(std::make_unique<impl_s>(device, uploads))
{
}

ffmpeg_registry_s::~ffmpeg_registry_s() = default;

std::shared_ptr<detail::playback_session_s> ffmpeg_registry_s::create(detail::playback_options_s options)
{
    auto session = std::shared_ptr<detail::playback_session_s>(new detail::playback_session_s(
        impl_->device, impl_->uploads, impl_->gpu_budget, std::move(options), impl_->hardware));
    impl_->sessions.push_back(session);
    impl_->control->post([session] { session->start(); });
    return session;
}

void ffmpeg_registry_s::retire(std::shared_ptr<detail::playback_session_s> session)
{
    if (!session) {
        return;
    }

    session->stop();
    std::erase(impl_->sessions, session);
    impl_->control->post([session = std::move(session)] { session->join(); });
}

} // namespace miximus::nodes::ffmpeg

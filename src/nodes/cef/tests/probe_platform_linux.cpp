#include "probe_platform.hpp"
#include "utils/owned_fd.hpp"

#include <cerrno>
#include <cstdint>
#include <iostream>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <system_error>
#include <vector>

namespace miximus::nodes::cef::tests {

void log_producer_fence(gpu::detail::native_handle_s::value_t dma_buf)
{
    dma_buf_export_sync_file exported{};
    exported.flags = DMA_BUF_SYNC_READ;
    exported.fd    = -1;
    if (ioctl(dma_buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) < 0) {
        throw std::system_error(errno, std::generic_category(), "inspect CEF producer fence");
    }
    utils::owned_fd_s fence{exported.fd};
    sync_file_info    info{};
    if (ioctl(fence.get(), SYNC_IOC_FILE_INFO, &info) < 0) {
        throw std::system_error(errno, std::generic_category(), "inspect CEF sync-file metadata");
    }
    // Metadata only: this does not map or read any image memory. A signalled
    // snapshot alone cannot prove that every producer write was published.
    std::cout << "CEF producer sync-file: fences=" << info.num_fences << " status=" << info.status << '\n';
    if (info.num_fences > 64) {
        throw std::runtime_error("Unexpected producer fence count");
    }
    std::vector<sync_fence_info> fences(info.num_fences);
    info.sync_fence_info = reinterpret_cast<uintptr_t>(fences.data());
    if (ioctl(fence.get(), SYNC_IOC_FILE_INFO, &info) < 0) {
        throw std::system_error(errno, std::generic_category(), "inspect CEF fence identities");
    }
    for (const auto& entry : fences) {
        std::cout << "Producer fence: driver=" << entry.driver_name << " timeline=" << entry.obj_name
                  << " status=" << entry.status << " timestamp_ns=" << entry.timestamp_ns << '\n';
    }
}

void configure_crash_probe()
{
    // Avoid systemd core collection delaying this probe's renderer-exit notification.
    // This affects only the probe and its children.
    const rlimit core_limit{.rlim_cur = 0, .rlim_max = 0};
    if (setrlimit(RLIMIT_CORE, &core_limit) != 0) {
        throw std::runtime_error("Cannot disable core dumps for the crash probe");
    }
}

} // namespace miximus::nodes::cef::tests

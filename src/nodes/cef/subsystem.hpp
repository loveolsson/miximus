#pragma once

#include "session.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace miximus::nodes::cef {

// A node's request owns no SDK work. Destruction schedules retirement without
// waiting; release acquired sessions and frame leases before destroying it.
class session_request_s
{
    struct state_s;
    std::shared_ptr<state_s> state_;
    explicit session_request_s(std::shared_ptr<state_s> state);
    friend class subsystem_s;

  public:
    ~session_request_s();
    session_request_s(const session_request_s& other)            = delete;
    session_request_s& operator=(const session_request_s& other) = delete;

    std::shared_ptr<session_s> session() const;
    std::string                error() const;
    bool                       reload(bool ignore_cache = false);
};

// Construct/destroy on the app startup/shutdown thread, with the GPU alive.
// All session allocation and retirement runs on the existing serial executor.
class subsystem_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    subsystem_s(gpu::device_s& device, const std::filesystem::path& profile_directory);
    // Explicit runtime location for isolated hardware probes.
    subsystem_s(gpu::device_s&               device,
                const std::filesystem::path& profile_directory,
                const std::filesystem::path& runtime_directory);
    ~subsystem_s();
    subsystem_s(const subsystem_s& other)            = delete;
    subsystem_s& operator=(const subsystem_s& other) = delete;

    std::unique_ptr<session_request_s> create_session(session_s::options_s options);
};

} // namespace miximus::nodes::cef

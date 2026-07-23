#pragma once
#include "types/settings_option.hpp"
#include "utils/serial_executor_fwd.hpp"

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace miximus::nodes::ndi {

class ndi_registry_s
{
    void* finder_{nullptr};
    bool  initialized_{false};

    mutable std::shared_mutex source_mutex_;
    std::vector<std::string>  source_names_;
    std::atomic<uint64_t>     source_list_version_{0};

    std::atomic<bool> running_{false};
    std::thread       discovery_thread_;

    std::unique_ptr<utils::serial_executor_s> control_executor_;

    void discovery_loop();

  public:
    ndi_registry_s();
    ~ndi_registry_s();

    ndi_registry_s(const ndi_registry_s&)            = delete;
    ndi_registry_s& operator=(const ndi_registry_s&) = delete;
    ndi_registry_s(ndi_registry_s&&)                 = delete;
    ndi_registry_s& operator=(ndi_registry_s&&)      = delete;

    uint64_t get_source_list_version() const { return source_list_version_.load(std::memory_order_relaxed); }

    std::vector<settings_option_s> get_source_options() const;

    utils::serial_executor_s* control_executor() { return control_executor_.get(); }

    static std::unique_ptr<ndi_registry_s> create_ndi_registry();
};

} // namespace miximus::nodes::ndi

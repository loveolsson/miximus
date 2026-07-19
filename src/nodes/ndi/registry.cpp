#include "registry.hpp"

#include "logger/logger.hpp"
#include "wrapper/ndi-sdk/ndi_inc.hpp"

#include <shared_mutex>
#include <utility>

namespace miximus::nodes::ndi {

ndi_registry_s::ndi_registry_s()
{
    if (!NDIlib_initialize()) {
        getlog("ndi")->error("NDIlib_initialize() failed — CPU may not support NDI");
        return;
    }

    initialized_ = true;

    NDIlib_find_create_t create{};
    create.show_local_sources = true;

    finder_ = NDIlib_find_create_v2(&create);
    if (finder_ == nullptr) {
        getlog("ndi")->error("NDIlib_find_create_v2() failed");
        return;
    }

    getlog("ndi")->info("NDI discovery started");

    running_          = true;
    discovery_thread_ = std::thread(&ndi_registry_s::discovery_loop, this);
}

ndi_registry_s::~ndi_registry_s()
{
    running_ = false;
    if (discovery_thread_.joinable()) {
        discovery_thread_.join();
    }

    if (finder_ != nullptr) {
        NDIlib_find_destroy(static_cast<NDIlib_find_instance_t>(finder_));
    }

    if (initialized_) {
        NDIlib_destroy();
    }
}

void ndi_registry_s::discovery_loop()
{
    auto log = getlog("ndi");

    while (running_.load()) {
        const bool changed = NDIlib_find_wait_for_sources(static_cast<NDIlib_find_instance_t>(finder_), 500);

        if (!running_.load()) {
            break;
        }

        if (!changed) {
            continue;
        }

        uint32_t               num_sources = 0;
        const NDIlib_source_t* sources =
            NDIlib_find_get_current_sources(static_cast<NDIlib_find_instance_t>(finder_), &num_sources);

        // Copy names while source pointers are valid (before next get_current_sources call)
        std::vector<std::string> names;
        names.reserve(num_sources);
        for (uint32_t i = 0; i < num_sources; ++i) {
            if (sources[i].p_ndi_name != nullptr) {
                names.emplace_back(sources[i].p_ndi_name);
            }
        }

        log->debug("NDI sources changed: {} source(s)", names.size());

        auto                     current_names = names;
        std::vector<std::string> previous_names;
        {
            const std::unique_lock lock(source_mutex_);
            previous_names = std::exchange(source_names_, std::move(names));
        }

        for (const auto& name : current_names) {
            if (std::ranges::find(previous_names, name) == previous_names.end()) {
                log->info("NDI source found: \"{}\"", name);
            }
        }
        for (const auto& name : previous_names) {
            if (std::ranges::find(current_names, name) == current_names.end()) {
                log->info("NDI source dropped: \"{}\"", name);
            }
        }

        ++source_list_version_;
    }
}

std::vector<settings_option_s> ndi_registry_s::get_source_options() const
{
    const std::shared_lock lock(source_mutex_);
    return make_settings_options_with_matching_labels(source_names_);
}

std::unique_ptr<ndi_registry_s> ndi_registry_s::create_ndi_registry() { return std::make_unique<ndi_registry_s>(); }

} // namespace miximus::nodes::ndi

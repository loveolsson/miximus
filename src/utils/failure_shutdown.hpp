#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace miximus::utils {

struct recovery_settings_s
{
    std::filesystem::path path;
    std::string           contents;
};

// Start the independent recovery and timeout workers before starting SDK workers.
void initialize_shutdown_monitor();
void request_failure_shutdown(std::string_view message, std::string_view detail = {}) noexcept;
bool failure_shutdown_requested() noexcept;

// Use only when returning/unwinding would release resources still owned by an SDK/GPU.
// The main thread can shut down other subsystems; the watchdog bounds any blocked joins.
[[noreturn]] void fail_without_unwinding(std::string_view message, std::string_view detail = {}) noexcept;

// Publish a fully serialized, consistent snapshot; failure handling never locks the graph.
void                                       publish_recovery_settings(std::filesystem::path path, std::string contents);
std::shared_ptr<const recovery_settings_s> recovery_settings();
void                                       freeze_recovery_settings();

// Unique, exclusively created recovery files never replace existing settings or recovery files.
std::filesystem::path write_recovery_settings(const recovery_settings_s& settings);
// Normal saves are forbidden once failure shutdown has been requested.
void write_settings_atomically(const recovery_settings_s& settings);

} // namespace miximus::utils

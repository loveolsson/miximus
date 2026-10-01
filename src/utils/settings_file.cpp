#include "failure_shutdown.hpp"
#include "filesystem.hpp"
#include "process_id.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace miximus::utils {
namespace {

std::filesystem::path write_unique(const recovery_settings_s& settings, std::string_view kind)
{
    static std::atomic_uint64_t sequence{};
    const auto                  stamp =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        auto path = settings.path;
        path += path_from_utf8(std::format(".{}-{}-{}-{}.json", kind, stamp, process_id(), sequence.fetch_add(1)));
        auto partial = path;
        partial += ".partial";
#ifdef _WIN32
        auto* raw = _wfopen(partial.c_str(), L"wbx");
#else
        auto* raw = std::fopen(partial.c_str(), "wbx");
#endif
        if (raw == nullptr) {
            if (errno == EEXIST) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "Cannot create settings file");
        }
        std::unique_ptr<FILE, decltype(&std::fclose)> file(raw, &std::fclose);
        if (std::fwrite(settings.contents.data(), 1, settings.contents.size(), file.get()) !=
                settings.contents.size() ||
            std::fflush(file.get()) != 0) {
            throw std::runtime_error("Cannot write settings file");
        }
#ifdef _WIN32
        const auto result = _commit(_fileno(file.get()));
#else
        const auto result = fsync(fileno(file.get()));
#endif
        if (result != 0) {
            throw std::system_error(errno, std::generic_category(), "Cannot flush settings file");
        }
        if (std::fclose(file.release()) != 0) {
            throw std::runtime_error("Cannot close settings file");
        }
        // Publish a complete recovery file without replacing even an existing
        // filename from another process. Incomplete writes keep a .partial suffix.
#ifdef _WIN32
        if (MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) == 0) {
            const auto error = GetLastError();
            if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
                std::filesystem::remove(partial);
                continue;
            }
            throw std::system_error(static_cast<int>(error), std::system_category(), "Cannot publish settings file");
        }
#else
        if (link(partial.c_str(), path.c_str()) != 0) {
            if (errno == EEXIST) {
                std::filesystem::remove(partial);
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "Cannot publish settings file");
        }
        std::filesystem::remove(partial);
#endif
        return path;
    }
    throw std::runtime_error("Cannot reserve a unique settings filename");
}

} // namespace

std::filesystem::path write_recovery_settings(const recovery_settings_s& settings)
{
    return write_unique(settings, "recovery");
}

void write_settings_atomically(const recovery_settings_s& settings)
{
    if (failure_shutdown_requested()) {
        throw std::runtime_error("Normal settings save disabled after an unrecoverable error");
    }
    const auto temporary = write_unique(settings, "saving");
    // A watchdog/worker failure during file preparation must also preserve the original.
    if (failure_shutdown_requested()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Normal settings save cancelled after an unrecoverable error");
    }
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), settings.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
        0) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(), "Cannot replace settings file");
    }
#else
    std::filesystem::rename(temporary, settings.path);
#endif
}

} // namespace miximus::utils

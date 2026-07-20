#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifdef LIBRARY_EXPORTS
#define LIBRARY_API __declspec(dllexport)
#else
#define LIBRARY_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define LIBRARY_API __attribute__((visibility("default")))
#else
#define LIBRARY_API
#endif

namespace miximus::static_files {
struct file_record_s
{
    std::string_view         filename;
    std::span<const uint8_t> gzipped;
    size_t                   size;
    std::string_view         mime;
    std::string_view         etag;
    LIBRARY_API std::string unzip() const;
};

struct file_map_s
{
    const std::span<const file_record_s> files;
    const std::string_view               bundle_hash;

    constexpr explicit file_map_s(std::span<const file_record_s> records, std::string_view hash) noexcept
        : files(records)
        , bundle_hash(hash)
    {
    }

    LIBRARY_API const file_record_s* get_file(std::string_view filename) const noexcept;
    LIBRARY_API const file_record_s& get_file_or_throw(std::string_view filename) const;
};

LIBRARY_API extern const file_map_s& get_web_files();
LIBRARY_API extern const file_map_s& get_resource_files();
} // namespace miximus::static_files

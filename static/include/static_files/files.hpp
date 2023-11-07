#pragma once
#include <string>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#ifdef LIBRARY_EXPORTS
#define LIBRARY_API __declspec(dllexport)
#else
#define LIBRARY_API __declspec(dllimport)
#endif
#else
#define LIBRARY_API
#endif

namespace miximus::static_files {
struct file_record_s
{
    std::string_view gzipped;
    size_t           size;
    std::string_view filename;
    std::string_view filename_lowercase;
    std::string_view mime;
    LIBRARY_API std::string unzip() const;
};

struct file_map_s
{
    using map_t = std::unordered_map<std::string_view, file_record_s>;

    const map_t files;

    explicit file_map_s(map_t&& o)
        : files(std::move(o))
    {
    }

    const file_record_s* get_file(std::string_view filename) const noexcept;
    const file_record_s& get_file_or_throw(std::string_view filename) const;
};

LIBRARY_API extern const file_map_s& get_web_files();
LIBRARY_API extern const file_map_s& get_resource_files();
} // namespace miximus::static_files

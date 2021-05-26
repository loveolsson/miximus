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
struct file_record
{
    std::string_view gzipped;
    std::string      raw;
    std::string_view mime;
};

typedef std::unordered_map<std::string_view, file_record> file_map_t;

LIBRARY_API extern const file_map_t& get_web_files();
LIBRARY_API extern const file_map_t& get_shader_files();
} // namespace miximus::static_files

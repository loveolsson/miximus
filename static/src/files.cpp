
#ifdef _WIN32
#define LIBRARY_EXPORTS
#endif

#include "static_files/files.hpp"

#include <fmt/format.h>
#include <gzip/decompress.hpp>

#include <stdexcept>

namespace miximus::static_files {

std::string file_record_s::unzip() const
{
    return gzip::decompress(reinterpret_cast<const char*>(gzipped.data()), gzipped.size());
}

const file_record_s* file_map_s::get_file(std::string_view filename) const noexcept
{
    if (const auto it = files.find(filename); it != files.end()) {
        return &it->second;
    }

    return nullptr;
}

const file_record_s* file_map_s::get_file_or_throw(std::string_view filename) const
{
    if (const auto* file = get_file(filename); file != nullptr) {
        return file;
    }

    throw std::out_of_range(fmt::format("File not found: {}", filename));
}

} // namespace miximus::static_files
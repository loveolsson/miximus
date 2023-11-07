
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
    auto data = gzip::decompress(gzipped.data(), gzipped.size());

    if (data.size() != size) {
        throw std::runtime_error(
            fmt::format("Unzipped file \"{}\" has size {} instead of expected size {}", filename, data.size(), size));
    }

    return data;
}

const file_record_s* file_map_s::get_file(std::string_view filename) const noexcept
{
    if (const auto it = files.find(filename); it != files.end()) {
        return &it->second;
    }

    return nullptr;
}

const file_record_s& file_map_s::get_file_or_throw(std::string_view filename) const
{
    const auto* it = get_file(filename);
    if (it != nullptr) {
        return *it;
    }

    throw std::out_of_range(fmt::format("File \"{}\" not found", filename));
}

} // namespace miximus::static_files
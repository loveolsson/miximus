
#ifdef _WIN32
#define LIBRARY_EXPORTS
#endif

#include "static_files/files.hpp"

#include <gzip/decompress.hpp>

#include <format>
#include <stdexcept>
#include <string>

namespace miximus::static_files {

std::string file_record_s::unzip() const
{
    auto data = gzip::decompress(reinterpret_cast<const char*>(gzipped.data()), gzipped.size());

    if (data.size() != size) {
        throw std::runtime_error(
            std::format("Unzipped file \"{}\" has size {} instead of expected size {}", filename, data.size(), size));
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

    throw std::out_of_range(std::format("File \"{}\" not found", filename));
}

} // namespace miximus::static_files

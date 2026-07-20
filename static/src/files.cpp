
#ifdef _WIN32
#define LIBRARY_EXPORTS
#endif

#include "static_files/files.hpp"

#ifndef ZLIB_CONST
#define ZLIB_CONST
#endif
#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <zlib.h>

namespace miximus::static_files {

std::string file_record_s::unzip() const
{
    if (gzipped.size() > std::numeric_limits<uInt>::max() || size > std::numeric_limits<uInt>::max()) {
        throw std::length_error(std::format("Bundled file \"{}\" is too large for zlib", filename));
    }

    std::string data(size, '\0');
    Bytef       empty_output{};

    z_stream stream{};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
    const int init_result = inflateInit2(&stream, MAX_WBITS + 16);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    if (init_result != Z_OK) {
        throw std::runtime_error(std::format("Failed to initialize zlib for \"{}\"", filename));
    }

    stream.next_in   = reinterpret_cast<const Bytef*>(gzipped.data());
    stream.avail_in  = static_cast<uInt>(gzipped.size());
    stream.next_out  = data.empty() ? &empty_output : reinterpret_cast<Bytef*>(data.data());
    stream.avail_out = data.empty() ? 1 : static_cast<uInt>(data.size());

    const int  inflate_result = inflate(&stream, Z_FINISH);
    const auto output_size    = static_cast<size_t>(stream.total_out);
    const auto remaining      = stream.avail_in;
    const int  end_result     = inflateEnd(&stream);

    if (inflate_result != Z_STREAM_END || end_result != Z_OK || remaining != 0 || output_size != size) {
        throw std::runtime_error(std::format("Failed to unzip file \"{}\" (zlib error {})", filename, inflate_result));
    }

    return data;
}

const file_record_s* file_map_s::get_file(std::string_view filename) const noexcept
{
    const auto it = std::ranges::lower_bound(files, filename, {}, &file_record_s::filename);
    if (it != files.end() && it->filename == filename) {
        return &*it;
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

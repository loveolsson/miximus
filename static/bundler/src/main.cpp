#include "directory.hpp"
#include "hex.hpp"
#include "mime.hpp"
#include "tab.hpp"

#include <boost/uuid/detail/sha1.hpp>
#include <gzip/compress.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

constexpr size_t BYTES_PER_LINE = 18;

namespace {
int bundle(const std::filesystem::path& src,
           const std::filesystem::path& dst,
           std::string_view             nspace,
           std::string_view             mapname)
{
    std::ofstream     target(dst);
    std::stringstream map;

    if (!target.is_open()) {
        std::cout << "Failed to open output file" << '\n';
        return EXIT_FAILURE;
    }

    auto files = get_file_paths(src);

    // Add the header of the file
    target << "#ifdef _WIN32" << '\n';
    target << "#define LIBRARY_EXPORTS" << '\n';
    target << "#endif" << '\n' << '\n';

    target << "#include \"static_files/files.hpp\"" << '\n';
    target << "#include <array>" << '\n';
    target << "#include <cstdint>" << '\n' << '\n';

    if (!nspace.empty()) {
        target << "namespace " << nspace << " {" << '\n' << '\n';
    }

    // Create the sorted file metadata array and its uniform bundle view.
    map << "const file_map_s& " << mapname << "()" << '\n';
    map << "{" << '\n';
    map << tab(1) << "static constexpr std::array<file_record_s, " << files.size() << "> records = {" << '\n';

    // Accumulate a bundle-level hash from each file's ETag
    boost::uuids::detail::sha1 bundle_sha1;

    // Iterate the files in the folder
    for (size_t fi = 0; fi < files.size(); ++fi) {
        const auto& filename  = files[fi];
        const auto  arr_name  = std::format("fileData{}", fi);
        auto        unix_name = filename.string();

        std::ranges::replace(unix_name, '\\', '/');

        std::ifstream file(src / filename, std::ifstream::binary);
        if (!file.is_open()) {
            std::cout << "Failed to read file: " << filename << '\n';
            return EXIT_FAILURE;
        }

        // Read the contents of the file
        using str_itr = std::istreambuf_iterator<char>;
        std::string file_data((str_itr(file)), str_itr());

        // Compress the buffer
        const auto compressed = gzip::compress(file_data.data(), file_data.size(), Z_BEST_COMPRESSION);
        const auto arr_size   = compressed.size();

        // SHA-1 of uncompressed data, used as ETag
        boost::uuids::detail::sha1 sha1;
        sha1.process_bytes(file_data.data(), file_data.size());
        boost::uuids::detail::sha1::digest_type digest;
        sha1.get_digest(digest);
        std::string sha_hex;
        sha_hex.reserve(40);
        for (const auto byte : digest) {
            sha_hex += std::format("{:02x}", byte);
        }

        bundle_sha1.process_bytes(sha_hex.data(), sha_hex.size());

        const auto comment = std::format("// File: {} ({} / {} compressed)", unix_name, file_data.size(), arr_size);

        target << comment << '\n';

        target << "constexpr std::array<uint8_t, " << arr_size << "> " << arr_name << " = {" << '\n';

        for (size_t i = 0; i < arr_size;) {
            target << tab(1);

            // Add the bytes in rows of BYTES_PER_LINE (18)
            for (size_t j = 0; (j < BYTES_PER_LINE) && (i < arr_size); ++j, ++i) {
                target << hex_u8(static_cast<uint8_t>(compressed[i])) << ", ";
            }

            target << '\n';
        }

        target << "};" << '\n' << '\n';

        map << tab(2) << "file_record_s{ " << comment << '\n';
        map << tab(3) << ".filename = \"" << unix_name << "\"," << '\n';
        map << tab(3) << ".gzipped = " << arr_name << "," << '\n';
        map << tab(3) << ".size = " << file_data.size() << "," << '\n';
        map << tab(3) << ".mime = \"" << get_mime(filename) << "\"," << '\n';
        map << tab(3) << ".etag = \"" << sha_hex << "\"," << '\n';
        map << tab(2) << "}," << '\n';
    }

    // Terminate the map declaration and add it to the file
    boost::uuids::detail::sha1::digest_type bundle_digest;
    bundle_sha1.get_digest(bundle_digest);
    std::string bundle_hash;
    bundle_hash.reserve(40);
    for (const auto byte : bundle_digest) {
        bundle_hash += std::format("{:02x}", byte);
    }

    map << tab(1) << "};" << '\n';
    map << tab(1) << "static constexpr file_map_s files(records, \"" << bundle_hash << "\");" << '\n' << '\n';
    map << tab(1) << "return files;" << '\n';
    map << "};" << '\n';

    target << map.rdbuf();

    if (!nspace.empty()) {
        target << '\n' << "} // namespace " << nspace << '\n';
    }

    target.close();

    return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char* argv[])
{
    std::cout << "Running bundler" << '\n';

    try {
        std::string_view src;
        std::string_view dst;
        std::string_view nspace;
        std::string_view mapname;

        for (int i = 1; i < argc; ++i) {
            const std::string_view opt(argv[i]);

            if (i + 1 >= argc) {
                std::cout << "Parameter " << opt << " is missing value" << '\n';
                return EXIT_FAILURE;
            }

            if (opt == "--src") {
                src = argv[++i];
            } else if (opt == "--dst") {
                dst = argv[++i];
            } else if (opt == "--namespace") {
                nspace = argv[++i];
            } else if (opt == "--mapname") {
                mapname = argv[++i];
            }
        }

        if (src.empty() || dst.empty() || mapname.empty()) {
            std::cout << "Missing parameter" << '\n';
            return EXIT_FAILURE;
        }

        std::cout << "Bundling folder" << src << "to " << dst << '\n';
        if (!nspace.empty()) {
            std::cout << "Using namespace " << nspace << '\n';
        }

        return bundle(src, dst, nspace, mapname);
    } catch (std::exception& e) {
        std::cout << "Exeption thrown: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

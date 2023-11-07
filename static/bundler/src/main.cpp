#include "directory.hpp"
#include "hex.hpp"
#include "mime.hpp"
#include "tab.hpp"

#include <fmt/format.h>
#include <gzip/compress.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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
    using std::endl;

    std::ofstream     target(dst);
    std::stringstream map;

    if (!target.is_open()) {
        std::cout << "Failed to open output file" << endl;
        return EXIT_FAILURE;
    }

    auto files = get_file_paths(src);

    // Add the header of the file
    target << "#ifdef _WIN32" << endl;
    target << "#define LIBRARY_EXPORTS" << endl;
    target << "#endif" << endl << endl;

    target << "#include \"static_files/files.hpp\"" << endl;
    target << "#include <array>" << endl;
    target << "#include <cstdint>" << endl << endl;

    if (!nspace.empty()) {
        target << "namespace " << nspace << " {" << endl << endl;
    }

    // Create the declaration of the map containing the uncompressed files
    map << "const file_map_s& " << mapname << "()" << endl;
    map << "{" << endl;
    map << tab(1) << "static const file_map_s files({" << endl;

    // Iterate the files in the folder
    for (int fi = 0; fi < files.size(); ++fi) {
        const auto& filename  = files[fi];
        const auto  arr_name  = fmt::format("fileData{}", fi);
        auto        unix_name = filename.string();

        std::replace(unix_name.begin(), unix_name.end(), '\\', '/');

        std::ifstream file(src / filename, std::ifstream::binary);
        if (!file.is_open()) {
            std::cout << "Failed to read file: " << filename << endl;
            return EXIT_FAILURE;
        }

        // Read the contents of the file
        using str_itr = std::istreambuf_iterator<char>;
        std::string file_data((str_itr(file)), str_itr());

        // Compress the buffer
        const auto compressed = gzip::compress(file_data.data(), file_data.size(), Z_BEST_COMPRESSION);
        const auto arr_size   = compressed.size();

        const auto comment = fmt::format("// File: {} ({} / {} compressed)", unix_name, file_data.size(), arr_size);

        target << comment << endl;

        target << "constexpr std::array<uint8_t, " << arr_size << "> " << arr_name << " = {" << endl;

        for (size_t i = 0; i < arr_size;) {
            target << tab(1);

            // Add the bytes in rows of BYTES_PER_LINE (18)
            for (size_t j = 0; (j < BYTES_PER_LINE) && (i < arr_size); ++j, ++i) {
                target << fmt_u8(compressed[i]) << ", ";
            }

            target << endl;
        }

        target << "};" << endl << endl;

        // Add an entry to the map that decompresses the file into the map
        map << tab(2) << "{ " << comment << endl;
        map << tab(3) << "\"" << unix_name << "\"," << endl;
        map << tab(3) << "{" << endl;
        map << tab(4) << ".gzipped = {reinterpret_cast<const char*>(" << arr_name << ".data()), " << arr_size << "},"
            << endl;
        map << tab(4) << ".size = " << file_data.size() << "," << endl;
        map << tab(4) << ".filename = \"" << unix_name << "\"," << endl;
        map << tab(4) << ".filename_lowercase = \"" << boost::to_lower_copy(unix_name) << "\"," << endl;
        map << tab(4) << ".mime = \"" << get_mime(filename) << "\"," << endl;
        map << tab(3) << "}," << endl;
        map << tab(2) << "}," << endl;
    }

    // Terminate the map declaration and add it to the file
    map << tab(1) << "});" << endl << endl;
    map << tab(1) << "return files;" << endl;
    map << "};" << endl;

    target << map.rdbuf();

    if (!nspace.empty()) {
        target << endl << "} // namespace " << nspace << endl;
    }

    target.close();

    return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char* argv[])
{
    using std::endl;

    std::cout << "Running bundler" << endl;

    try {
        std::string_view src;
        std::string_view dst;
        std::string_view nspace;
        std::string_view mapname;

        for (int i = 1; i < argc; ++i) {
            const std::string_view opt(argv[i]);

            if (i + 1 >= argc) {
                std::cout << "Parameter " << opt << " is missing value" << endl;
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
            std::cout << "Missing parameter" << endl;
            return EXIT_FAILURE;
        }

        std::cout << "Bundling folder" << src << "to " << dst << endl;
        if (!nspace.empty()) {
            std::cout << "Using namespace " << nspace << endl;
        }

        return bundle(src, dst, nspace, mapname);
    } catch (std::exception& e) {
        std::cout << "Exeption thrown: " << e.what() << endl;
        return EXIT_FAILURE;
    }
}
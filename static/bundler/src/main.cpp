#include <gzip/compress.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

constexpr size_t BYTES_PER_LINE = 18;

static auto tab(size_t i) { return std::string(i * 4, ' '); }

static auto get_file_paths(const std::filesystem::path& root)
{
    using itr = std::filesystem::recursive_directory_iterator;
    std::vector<std::filesystem::path> res;

    for (itr end, dir(root); dir != end; ++dir) {
        if (is_regular_file(*dir)) {
            res.emplace_back(relative(dir->path(), root));
        }
    }

    return res;
}

static std::string_view get_mime(const std::filesystem::path& name)
{
    static auto lookup = std::map<std::string_view, std::string_view>{
        {".css", "text/css;charset=UTF-8"},
        {".html", "text/html;charset=UTF-8"},
        {".js", "text/javascript;charset=UTF-8"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".png", "image/png"},
        {".gif", "image/gif"},
        {".webp", "image/webp"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
    };

    const auto it = lookup.find(name.extension().c_str());
    if (it != lookup.end()) {
        return it->second;
    }

    return "application/octet-stream";
}

static int bundle(const std::filesystem::path& src,
                  const std::filesystem::path& dst,
                  std::string_view             nspace,
                  std::string_view             mapname)
{
    using std::endl;

    constexpr std::array hex_char = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

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
    target << "#include <array>" << endl << endl;

    if (!nspace.empty()) {
        target << "namespace " << nspace << " {" << endl << endl;
    }

    // Create the declaration of the map containing the uncompressed files
    map << "const file_map_s& " << mapname << "()" << endl;
    map << "{" << endl;
    map << tab(1) << "static file_map_s files({" << endl;

    // Iterate the files in the folder
    for (int fi = 0; fi < files.size(); ++fi) {
        auto& filename  = files[fi];
        auto  unix_name = filename.string();
        std::replace(unix_name.begin(), unix_name.end(), '\\', '/');

        std::ifstream file(src.string() + filename.string(), std::ifstream::binary);
        if (!file.is_open()) {
            std::cout << "Failed to read file: " << filename << endl;
            return EXIT_FAILURE;
        }

        // Read the contents of the file
        using str_itr = std::istreambuf_iterator<char>;
        std::string file_data((str_itr(file)), str_itr());

        // Compress the buffer
        auto compressed = gzip::compress(file_data.data(), file_data.size(), Z_BEST_COMPRESSION);

        std::stringstream comment;
        comment << "// File: " << unix_name << " (" << file_data.size() << " / " << compressed.size() << " compressed)";

        target << std::dec << comment.str() << endl;

        target << "static const std::array<uint8_t, " << compressed.size() << "> fileData" << fi << " = {" << endl;
        for (size_t i = 0; i < compressed.size();) {
            target << tab(1);
            // Add the bytes in rows of 18
            for (size_t j = 0; j < BYTES_PER_LINE && i < compressed.size(); ++j, ++i) {
                uint8_t c = compressed[i];
                target << "0x" << hex_char.at(c >> 4U) << hex_char.at(c & 0x0FU) << ", ";
            }
            target << endl;
        }
        target << "};" << endl << endl;

        // Add an entry to the map that decompresses the file into the map
        map << tab(2) << "{ " << comment.str() << endl;
        map << tab(3) << "\"" << unix_name << "\"," << endl;
        map << tab(3) << "{" << endl;
        map << tab(4) << "{fileData" << fi << ".data(), fileData" << fi << ".size()}," << endl;
        map << tab(4) << "\"" << get_mime(filename) << "\"," << endl;
        map << tab(3) << "}," << endl;
        map << tab(2) << "}," << endl;
    }

    // Terminate the map declaration and add it to the file
    map << tab(1) << "});" << endl << endl;
    map << tab(1) << "return files;" << endl;
    map << "};" << endl;
    target << map.str();

    if (!nspace.empty()) {
        target << endl << "} // namespace " << nspace << endl;
    }

    target.close();

    return EXIT_SUCCESS;
}

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
            std::string_view opt(argv[i]);

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
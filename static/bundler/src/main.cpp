#include <frozen/map.h>
#include <gzip/compress.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

constexpr size_t BYTES_PER_LINE = 18;

static std::string tab(size_t i) { return std::string(i * 4, ' '); }

static std::vector<std::string> get_file_paths(const std::string& root)
{
    using itr = std::filesystem::recursive_directory_iterator;
    std::vector<std::string> res;

    for (itr end, dir(root); dir != end; ++dir) {
        if (is_regular_file(*dir)) {
            res.push_back(relative(dir->path(), root).string());
        }
    }

    return res;
}

static std::string_view get_mime(const std::string& name)
{
    constexpr frozen::map<std::string_view, std::string_view, 10> lookup = {
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

    auto ext = name.substr(name.find_last_of('.'));

    const auto* it = lookup.find(ext);
    if (it != lookup.end()) {
        return it->second;
    }

    return "application/octet-stream";
}

static int bundle(const std::string& src, const std::string& dst, const std::string& nspace, const std::string& mapname)
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
    target << "#include <array>" << endl << endl;
    target << "#include <gzip/decompress.hpp>" << endl << endl;

    target << "namespace " << nspace << " {" << endl;

    // Create the declaration of the map containing the uncompressed files
    map << "file_map_t " << mapname << "()" << endl;
    map << "{" << endl;
    map << tab(1) << "file_map_t files {" << endl;

    // Iterate the files in the folder
    for (int fi = 0; fi < files.size(); ++fi) {
        auto& filename  = files[fi];
        auto  unix_name = filename;
        std::replace(unix_name.begin(), unix_name.end(), '\\', '/');

        std::ifstream file(src + filename, std::ifstream::binary);
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
        for (int i = 0; i < compressed.size();) {
            target << tab(1);
            // Add the bytes in rows of 18
            for (size_t j = 0; j < BYTES_PER_LINE && i < compressed.size(); ++j, ++i) {
                target << "0x" << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<int>(static_cast<uint8_t>(compressed[i])) << ", ";
            }
            target << endl;
        }
        target << "};" << endl << endl;

        // Add an entry to the map that decompresses the file into the map
        map << tab(2) << "{ " << comment.str() << endl;
        map << tab(3) << "\"" << unix_name << "\"," << endl;
        map << tab(3) << "{" << endl;
        map << tab(4) << "{reinterpret_cast<const char *>(fileData" << fi << ".data()), fileData" << fi << ".size()},"
            << endl;
        map << tab(4) << "gzip::decompress(reinterpret_cast<const char *>(fileData" << fi << ".data()), fileData" << fi
            << ".size())," << endl;
        map << tab(4) << "\"" << get_mime(src + filename) << "\"," << endl;
        map << tab(3) << "}," << endl;
        map << tab(2) << "}," << endl;
    }

    // Terminate the map declaration and add it to the file
    map << tab(1) << "};" << endl << endl;
    map << tab(1) << "return files;" << endl;
    map << "};" << endl;
    target << map.str();

    target << "} // namespace " << nspace << endl;
    target.close();

    return EXIT_SUCCESS;
}

int main(int argc, char* argv[])
{
    using std::endl;

    std::cout << "Running bundler" << endl;

    try {
        std::string src;
        std::string dst;
        std::string nspace;
        std::string mapname;

        for (int i = 1; i < argc; ++i) {
            std::string opt(argv[i]);

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

        if (src.empty() || dst.empty() || nspace.empty() || mapname.empty()) {
            std::cout << "Missing parameter" << endl;
            return EXIT_FAILURE;
        }

        std::cout << "Bundling " << src << endl;

        return bundle(src, dst, nspace, mapname);
    } catch (std::exception& e) {
        std::cout << "Exeption thrown: " << e.what() << endl;
        return EXIT_FAILURE;
    }
}
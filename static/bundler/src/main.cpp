#include <gzip/compress.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

static string tab(size_t i) { return string(i * 4, ' '); }

static vector<string> get_file_paths(const string& root)
{
    using namespace filesystem;
    vector<string> res;

    for (recursive_directory_iterator end, dir(root); dir != end; ++dir) {
        if (is_regular_file(*dir)) {
            res.push_back(relative(dir->path(), root).string());
        }
    }

    return res;
}

static string get_mime(const string& name)
{
    string res;

    res = name.substr(name.find_last_of('.'));

    if (res == ".css") {
        return "text/css;charset=UTF-8";
    } else if (res == ".html") {
        return "text/html;charset=UTF-8";
    } else if (res == ".js") {
        return "text/javascript;charset=UTF-8";
    } else if (res == ".jpg" || res == ".jpeg") {
        return "image/jpeg";
    } else if (res == ".png") {
        return "image/png";
    } else if (res == ".gif") {
        return "image/gif";
    } else if (res == ".webp") {
        return "image/webp";
    } else if (res == ".svg") {
        return "image/svg+xml";
    } else if (res == ".ico") {
        return "image/x-icon";
    }

    return "application/octet-stream";
}

static int bundle(const string& src, const string& dst, const string& nspace, const string& mapname)
{
    ofstream     target(dst);
    stringstream map;

    if (!target.is_open()) {
        cout << "Failed to open output file" << endl;
        return EXIT_FAILURE;
    }

    auto files = get_file_paths(src);

    // Add the header of the file
    target << "#ifdef _WIN32" << endl;
    target << "#define LIBRARY_EXPORTS" << endl;
    target << "#endif" << endl << endl;

    target << "#include \"static_files/files.hpp\"" << endl;
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
        replace(unix_name.begin(), unix_name.end(), '\\', '/');

        ifstream file(src + filename, ifstream::binary);
        if (!file.is_open()) {
            cout << "Failed to read file: " << filename << endl;
            return EXIT_FAILURE;
        }

        // Read the contents of the file
        string file_data((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());

        // Compress the buffer
        auto compressed = gzip::compress(file_data.data(), file_data.size(), Z_BEST_COMPRESSION);

        stringstream comment;
        comment << "// File: " << unix_name << " (" << file_data.size() << " / " << compressed.size() << " compressed)";

        target << comment.str() << endl;

        target << "static const uint8_t fileData" << fi << "[] = {" << endl;
        for (int i = 0; i < compressed.size();) {
            target << tab(1);
            // Add the bytes in rows of 18
            for (int j = 0; j < 18 && i < compressed.size(); ++j, ++i) {
                target << "0x" << hex << setw(2) << setfill('0') << (int)(uint8_t)compressed[i] << ", ";
            }
            target << endl;
        }
        target << "};" << endl << endl;

        // Add an entry to the map that decompresses the file into the map
        map << tab(2) << "{ " << comment.str() << endl;
        map << tab(3) << "\"" << unix_name << "\"," << endl;
        map << tab(3) << "{" << endl;
        map << tab(4) << "{(const char*)fileData" << fi << ", " << compressed.size() << "}," << endl;
        map << tab(4) << "gzip::decompress((const char*)fileData" << fi << ", " << compressed.size() << ")," << endl;
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
    cout << "Running bundler" << endl;

    string src;
    string dst;
    string nspace;
    string mapname;

    for (int i = 1; i < argc; ++i) {
        string opt(argv[i]);

        if (opt == "--src" && i + 1 < argc) {
            src = argv[++i];
        } else if (opt == "--dst" && i + 1 < argc) {
            dst = argv[++i];
        } else if (opt == "--namespace" && i + 1 < argc) {
            nspace = argv[++i];
        } else if (opt == "--mapname" && i + 1 < argc) {
            mapname = argv[++i];
        }
    }

    if (src.empty() || dst.empty() || nspace.empty() || mapname.empty()) {
        cout << "Missing parameter" << endl;
        return EXIT_FAILURE;
    }

    cout << "Bundling " << src << endl;

    return bundle(src, dst, nspace, mapname);
}
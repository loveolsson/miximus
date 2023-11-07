#include <filesystem>
#include <vector>

static std::vector<std::filesystem::path> get_file_paths(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> res;

    using itr = std::filesystem::recursive_directory_iterator;
    for (itr dir(root); dir != itr(); ++dir) {
        if (is_regular_file(*dir)) {
            res.emplace_back(relative(*dir, root));
        }
    }

    return res;
}

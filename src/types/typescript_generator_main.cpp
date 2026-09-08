#include "typescript_generator.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: miximus_typescript_generator <output.ts>\n";
        return 1;
    }

    try {
        const auto changed = miximus::typescript::write_if_changed(argv[1], miximus::typescript::generate_typescript());
        std::cout << (changed ? "Generated " : "Unchanged ") << argv[1] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TypeScript contract generation failed: " << error.what() << '\n';
        return 1;
    }
}

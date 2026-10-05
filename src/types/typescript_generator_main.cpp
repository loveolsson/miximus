#include "typescript_generator.hpp"

#include <cstdio>
#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fputs("Usage: miximus_typescript_generator <output.ts>\n", stderr);
        return 1;
    }

    try {
        const auto changed = miximus::typescript::write_if_changed(argv[1], miximus::typescript::generate_typescript());
        std::cout << (changed ? "Generated " : "Unchanged ") << argv[1] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "TypeScript contract generation failed: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fputs("TypeScript contract generation failed: unknown exception\n", stderr);
        return 1;
    }
}

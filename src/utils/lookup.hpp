#pragma once

namespace miximus {

// Test that two lookup tables matches in both directions
constexpr auto verify_lookup = [](const auto& a, const auto& b) {
    auto test = [](const auto& a, const auto& b) {
        for (const auto& [id, name] : a) {
            const auto it = b.find(name);

            if (it == b.end()) {
                return false;
            }

            if (!(it->second == id)) { // frozed::string only has equality operator
                return false;
            }
        }

        return true;
    };

    return test(a, b) && test(b, a);
};

} // namespace miximus
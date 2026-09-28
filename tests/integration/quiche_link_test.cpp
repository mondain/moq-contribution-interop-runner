#include <cstdlib>
#include <iostream>
#include <string_view>

#include <quiche.h>

int main() {
    const char* version = quiche_version();
    if (version == nullptr) {
        std::cerr << "quiche_version() returned null\n";
        return EXIT_FAILURE;
    }

    constexpr std::string_view expected = "0.24.9";
    const std::string_view actual(version);
    if (actual != expected) {
        std::cerr << "unexpected quiche version: " << actual << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

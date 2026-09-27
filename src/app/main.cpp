#include "moq/interop/app/version.h"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        const moq::interop::app::BuildInfo info = moq::interop::app::build_info();
        std::cout << "moq-interop-runner " << info.version << "\n"
                  << "source: " << info.source_revision << '\n';
        for (const auto& [name, revision] : info.dependencies) {
            std::cout << name << ": " << revision << '\n';
        }
        return 0;
    }

    std::cerr << "Usage: moq-interop-runner --version\n";
    return 2;
}

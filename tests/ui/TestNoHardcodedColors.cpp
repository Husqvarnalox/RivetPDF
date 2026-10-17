// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

// ADR-0033: every color in the shell (src/ui, src/app) is a theme token.
// Raw color literals live only in ui/Theme.cpp (the palettes) and
// ui/Icons.cpp (icon geometry). User data colors (annotation presets) are
// pdf::PdfColor values and are not matched; converting a data color with
// ui::Color::rgba(variable, ...) is allowed (the arguments are not literals).

namespace fs = std::filesystem;

RIVET_TEST(noColorLiteralsOutsideTheTheme) {
    const fs::path root{RIVET_SOURCE_DIR};
    const std::regex literal(
        R"((Color::(rgba|gray)\(\s*[-0-9.])|(Color::(white|black)\(\))|((^|[^A-Za-z_])Color\{\s*[-0-9.]))");
    const std::vector<std::string> allowed = {"ui/Theme.cpp", "ui/Icons.cpp"};
    std::size_t scanned = 0;
    std::vector<std::string> offenders;
    for (const char* dir : {"src/ui", "src/app"}) {
        for (const auto& entry : fs::recursive_directory_iterator(root / dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".mm") continue;
            const std::string rel = fs::relative(entry.path(), root / "src").generic_string();
            bool skip = false;
            for (const std::string& a : allowed) skip = skip || rel == a;
            if (skip) continue;
            ++scanned;
            std::ifstream in(entry.path());
            std::string line;
            int number = 0;
            while (std::getline(in, line)) {
                ++number;
                if (std::regex_search(line, literal)) offenders.push_back(rel + ":" + std::to_string(number) + ": " + line);
            }
        }
    }
    for (const std::string& o : offenders) std::fprintf(stderr, "    color literal: %s\n", o.c_str());
    CHECK(scanned > 50);
    CHECK(offenders.empty());
}

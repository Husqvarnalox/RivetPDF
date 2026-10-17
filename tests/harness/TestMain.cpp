#include "RivetTest.h"

#include "TestProcess.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

// Points the temp directory at a per-process directory so tests that create or
// count files in it (e.g. "rivet-print-doc-*") cannot race concurrent test
// processes sharing the system temp directory.
std::filesystem::path isolateTempDirectory() {
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path base = fs::temp_directory_path(error);
    if (error) return {};
    const fs::path root = base / ("rivet-test-" + std::to_string(rivet::test::processId()));
    fs::remove_all(root, error);
    if (!fs::create_directories(root, error) || error) return {};
    const std::string value = root.string();
#if defined(_WIN32)
    _putenv_s("TMP", value.c_str());
    _putenv_s("TEMP", value.c_str());
#else
    ::setenv("TMPDIR", value.c_str(), 1);
#endif
    return root;
}

} // namespace

int main() {
    const std::filesystem::path root = isolateTempDirectory();
    const int result = rivet::test::runAll();
    if (!root.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    return result;
}

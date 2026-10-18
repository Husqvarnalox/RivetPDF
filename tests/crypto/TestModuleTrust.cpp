// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "crypto/ModuleTrust.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using rivet::crypto::checkModuleFile;
using rivet::crypto::loadTrustedModule;
using rivet::crypto::setModuleLoadHookForTesting;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::error_code ec;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::weakly_canonical(fs::temp_directory_path(ec), ec) / ("rivet-modtrust-" + std::to_string(stamp));
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, ec);
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

fs::path writeFile(const fs::path& p) {
    std::ofstream(p, std::ios::binary) << "not a real module";
    return p;
}

void expectRefused(const std::string& why, const char* needle) {
    if (why.find(needle) == std::string::npos) std::fprintf(stderr, "    reason was: '%s' (wanted '%s')\n", why.c_str(), needle);
    CHECK(!why.empty());
    CHECK(why.find(needle) != std::string::npos);
}

} // namespace

RIVET_TEST(moduleTrustRejectsEmptyAndRelativePaths) {
    CHECK(!checkModuleFile(fs::path()).empty());
    expectRefused(checkModuleFile("lib/module.so"), "absolute");
    // The development override never waives the absolute-path rule.
    expectRefused(checkModuleFile("lib/module.so", true), "absolute");
}

#if !defined(_WIN32)

RIVET_TEST(moduleTrustAcceptsOwnedPrivateFile) {
    TempDir d;
    const fs::path f = writeFile(d.path / "ok.so");
    fs::permissions(f, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace);
    const std::string why = checkModuleFile(f);
    if (!why.empty()) std::fprintf(stderr, "    unexpected refusal: %s\n", why.c_str());
    CHECK(why.empty());
}

RIVET_TEST(moduleTrustRejectsMissingDirectoryAndSpecialFiles) {
    TempDir d;
    expectRefused(checkModuleFile(d.path / "missing.so"), "not found");
    expectRefused(checkModuleFile(d.path), "regular file");
}

RIVET_TEST(moduleTrustRejectsGroupAndWorldWritableFiles) {
    TempDir d;
    const fs::path f = writeFile(d.path / "w.so");
    fs::permissions(f, fs::perms::group_write, fs::perm_options::add);
    expectRefused(checkModuleFile(f), "writable");
    fs::permissions(f, fs::perms::group_write, fs::perm_options::remove);
    CHECK(checkModuleFile(f).empty());
    fs::permissions(f, fs::perms::others_write, fs::perm_options::add);
    expectRefused(checkModuleFile(f), "writable");
    // The override accepts it.
    CHECK(checkModuleFile(f, true).empty());
}

RIVET_TEST(moduleTrustRejectsWorldWritableParentButAllowsStickyOnes) {
    TempDir d;
    const fs::path dir = d.path / "dir";
    fs::create_directories(dir);
    const fs::path f = writeFile(dir / "m.so");
    CHECK(checkModuleFile(f).empty());
    CHECK_EQ(::chmod(dir.c_str(), 0777), 0);
    expectRefused(checkModuleFile(f), "world-writable");
    CHECK(checkModuleFile(f, true).empty());
    CHECK_EQ(::chmod(dir.c_str(), 01777), 0); // sticky, like /tmp: others cannot replace our file
    CHECK(checkModuleFile(f).empty());
    CHECK_EQ(::chmod(dir.c_str(), 0700), 0);
}

RIVET_TEST(moduleTrustResolvesSymlinksAndChecksTheTarget) {
    TempDir d;
    const fs::path safeDir = d.path / "safe";
    const fs::path badDir = d.path / "bad";
    fs::create_directories(safeDir);
    fs::create_directories(badDir);
    const fs::path good = writeFile(safeDir / "libreal.so");
    const fs::path bad = writeFile(badDir / "libevil.so");
    fs::permissions(bad, fs::perms::others_write, fs::perm_options::add);

    // A link to a trusted regular file is fine (vendors ship libfoo.so -> libfoo.so.1.2).
    std::error_code ec;
    fs::create_symlink(good, safeDir / "liblink.so", ec);
    CHECK(!ec);
    CHECK(checkModuleFile(safeDir / "liblink.so").empty());

    // A link that escapes to a writable target is refused although the link itself is harmless.
    fs::create_symlink(bad, safeDir / "libescape.so", ec);
    CHECK(!ec);
    expectRefused(checkModuleFile(safeDir / "libescape.so"), "writable");

    // A link whose target is a directory is not a regular file.
    fs::create_directory_symlink(badDir, safeDir / "libdir.so", ec);
    CHECK(!ec);
    expectRefused(checkModuleFile(safeDir / "libdir.so"), "regular file");

    // A trusted target behind a link that lives in a world-writable directory: the link could be re-pointed.
    const fs::path openDir = d.path / "open";
    fs::create_directories(openDir);
    fs::create_symlink(good, openDir / "liblink.so", ec);
    CHECK(!ec);
    CHECK_EQ(::chmod(openDir.c_str(), 0777), 0);
    expectRefused(checkModuleFile(openDir / "liblink.so"), "world-writable");
    CHECK_EQ(::chmod(openDir.c_str(), 0700), 0);
}

// ---- loadTrustedModule: validation and load on the same object --------------

namespace {

fs::path installModule(const fs::path& dir, const char* name, const char* source) {
    const fs::path dst = dir / name;
    std::error_code ec;
    fs::copy_file(source, dst, fs::copy_options::overwrite_existing, ec);
    fs::permissions(dst, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                             fs::perms::others_exec,
                    fs::perm_options::replace, ec);
    return dst;
}

int moduleId(void* handle) {
    using Fn = int (*)();
    auto fn = reinterpret_cast<Fn>(::dlsym(handle, "rivetTestModuleId"));
    return fn ? fn() : -1;
}

// State for the swap hook (a plain function pointer: the production seam takes no closure).
struct Swap {
    fs::path replacement; // file moved over the validated path
    int calls = 0;
};
Swap g_swap;

void swapHook(const fs::path& validated) {
    ++g_swap.calls;
    std::error_code ec;
    fs::rename(g_swap.replacement, validated, ec);
}

} // namespace

RIVET_TEST(loadTrustedModuleLoadsAValidModule) {
    TempDir d;
    const fs::path m = installModule(d.path, "a.so", RIVET_TEST_MODULE_A);
    auto r = loadTrustedModule(m);
    if (!r.handle) std::fprintf(stderr, "    load failed: %s\n", r.error.c_str());
    CHECK(r.handle != nullptr);
    if (!(r.handle != nullptr)) return;
    CHECK_EQ(moduleId(r.handle), 1);
    ::dlclose(r.handle);
}

RIVET_TEST(loadTrustedModuleRefusesUntrustedFilesWithoutLoading) {
    TempDir d;
    const fs::path m = installModule(d.path, "a.so", RIVET_TEST_MODULE_A);
    fs::permissions(m, fs::perms::others_write, fs::perm_options::add);
    auto r = loadTrustedModule(m);
    CHECK(r.handle == nullptr);
    CHECK(r.refused);
    CHECK(r.error.find("writable") != std::string::npos);
    // The override loads it by path.
    auto o = loadTrustedModule(m, true);
    CHECK(o.handle != nullptr);
    if (!(o.handle != nullptr)) return;
    ::dlclose(o.handle);
    CHECK(loadTrustedModule(d.path / "missing.so").refused);
    CHECK(loadTrustedModule("relative/a.so").refused);
}

RIVET_TEST(loadTrustedModuleRefusesASymlinkAtTheOpenedName) {
    // checkModuleFile resolves links; the opened object is the resolved file, never the link.
    TempDir d;
    const fs::path real = installModule(d.path, "real.so", RIVET_TEST_MODULE_A);
    std::error_code ec;
    fs::create_symlink(real, d.path / "link.so", ec);
    CHECK(!ec);
    auto r = loadTrustedModule(d.path / "link.so");
    CHECK(r.handle != nullptr);
    if (!(r.handle != nullptr)) return;
    CHECK_EQ(moduleId(r.handle), 1);
    ::dlclose(r.handle);
}

RIVET_TEST(loadTrustedModuleSurvivesTheFileBeingSwappedBetweenCheckAndLoad) {
    TempDir d;
    const fs::path m = installModule(d.path, "a.so", RIVET_TEST_MODULE_A);
    g_swap.replacement = installModule(d.path, "evil.so", RIVET_TEST_MODULE_B);
    g_swap.calls = 0;
    setModuleLoadHookForTesting(&swapHook);
    auto r = loadTrustedModule(m);
    setModuleLoadHookForTesting(nullptr);
    CHECK_EQ(g_swap.calls, 1);
    // Path strategy: the replacement is detected and refused. Never "module 2 loaded".
    if (r.handle) ::dlclose(r.handle);
    CHECK(r.handle == nullptr);
    CHECK(r.refused);
    CHECK(r.error.find("replaced") != std::string::npos);
}

#else

RIVET_TEST(moduleTrustWindowsRefusesUncAndDevicePaths) {
    expectRefused(checkModuleFile(fs::path(L"\\\\server\\share\\m.dll")), "UNC");
    expectRefused(checkModuleFile(fs::path(L"\\\\?\\C:\\m.dll")), "UNC");
}

#endif

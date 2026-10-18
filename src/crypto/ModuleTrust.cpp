// SPDX-License-Identifier: MPL-2.0
#include "crypto/ModuleTrust.hpp"

#include <atomic>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rivet::crypto {

namespace {

namespace fs = std::filesystem;

std::atomic<ModuleLoadHook> g_hook{nullptr};

#if defined(_WIN32)

std::string checkPlatform(const fs::path& file) {
    const std::wstring& n = file.native();
    // UNC shares, \\?\ and \\.\ device paths and root-relative paths ("\foo") are refused.
    if (n.size() >= 2 && (n[0] == L'\\' || n[0] == L'/')) return "UNC / device / root-relative module paths are refused: " + file.string();
    if (n.size() < 3 || n[1] != L':' || (n[2] != L'\\' && n[2] != L'/'))
        return "module path must be a drive-letter absolute path: " + file.string();
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(file, ec);
    if (ec || st.type() == fs::file_type::not_found) return "module not found: " + file.string();
    if (st.type() == fs::file_type::symlink) return "module path is a symbolic link: " + file.string();
    if (st.type() != fs::file_type::regular) return "module path is not a regular file: " + file.string();
    return {};
}

#else

std::string checkDirectory(const fs::path& dir, const std::string& what) {
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0) return what + " directory cannot be inspected: " + dir.string();
    if (!S_ISDIR(st.st_mode)) return what + " parent is not a directory: " + dir.string();
    if ((st.st_mode & S_IWOTH) != 0 && (st.st_mode & S_ISVTX) == 0)
        return what + " directory is world-writable: " + dir.string();
    return {};
}

// File-object rules, applied to a stat of the path or (preferably) of an open descriptor.
std::string checkStat(const struct stat& st, const fs::path& real) {
    if (!S_ISREG(st.st_mode)) return "module is not a regular file: " + real.string();
    if (st.st_uid != 0 && st.st_uid != ::geteuid())
        return "module is owned by another user (expected root or the current user): " + real.string();
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) return "module is group- or world-writable: " + real.string();
    return {};
}

std::string checkPlatform(const fs::path& file) {
    struct stat link {};
    if (::lstat(file.c_str(), &link) != 0) return "module not found: " + file.string();

    // The directory holding the path itself (and so any symlink in it).
    if (auto why = checkDirectory(file.parent_path(), "module"); !why.empty()) return why;

    std::error_code ec;
    const fs::path real = fs::canonical(file, ec);
    if (ec) return "module path cannot be resolved: " + file.string();
    if (real != file) {
        if (auto why = checkDirectory(real.parent_path(), "resolved module"); !why.empty()) return why;
    }

    struct stat st {};
    if (::stat(real.c_str(), &st) != 0) return "module not found: " + real.string();
    return checkStat(st, real);
}

struct Fd {
    int fd = -1;
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
};

bool sameObject(const struct stat& a, const struct stat& b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }

ModuleLoadResult refuse(std::string why) {
    ModuleLoadResult r;
    r.error = std::move(why);
    r.refused = true;
    return r;
}

ModuleLoadResult loaderFailure(const char* fallback) {
    ModuleLoadResult r;
    const char* why = ::dlerror();
    r.error = why ? why : fallback;
    return r;
}

ModuleLoadResult loadPosix(const fs::path& file) {
    if (auto why = checkPlatform(file); !why.empty()) return refuse(std::move(why));

    std::error_code ec;
    const fs::path real = fs::canonical(file, ec);
    if (ec) return refuse("module path cannot be resolved: " + file.string());

    // Open the object once. O_NOFOLLOW: the resolved name must not have become a link.
    Fd f;
    f.fd = ::open(real.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (f.fd < 0) return refuse("module cannot be opened (or is a symbolic link): " + real.string());
    struct stat opened {};
    if (::fstat(f.fd, &opened) != 0) return refuse("module cannot be inspected: " + real.string());
    if (auto why = checkStat(opened, real); !why.empty()) return refuse(std::move(why));
    // The name we resolved must still denote the object we validated.
    struct stat byName {};
    if (::lstat(real.c_str(), &byName) != 0 || !sameObject(opened, byName))
        return refuse("module changed while it was being opened: " + real.string());

    if (const ModuleLoadHook hook = g_hook.load()) hook(real);

#if defined(__linux__)
    // dlopen of the descriptor itself: later changes to the name cannot affect what is mapped.
    {
        const std::string viaFd = "/proc/self/fd/" + std::to_string(f.fd);
        ::dlerror();
        if (void* h = ::dlopen(viaFd.c_str(), RTLD_NOW | RTLD_LOCAL)) {
            ModuleLoadResult r;
            r.handle = h;
            return r;
        }
        // /proc missing, or a module that needs $ORIGIN: fall through to the path strategy.
    }
#endif
    ::dlerror();
    void* h = ::dlopen(real.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) return loaderFailure("dlopen failed");
    // Post-load verification: the path must still be the validated object.
    struct stat after {};
    if (::stat(real.c_str(), &after) != 0 || !sameObject(opened, after)) {
        ::dlclose(h);
        return refuse("module was replaced while it was being loaded: " + real.string());
    }
    ModuleLoadResult r;
    r.handle = h;
    return r;
}

#endif

} // namespace

std::string checkModuleFile(const fs::path& file, bool allowInsecure) {
    if (file.empty()) return "no module path";
    if (!file.is_absolute()) return "the module path must be absolute: " + file.string();
    if (allowInsecure) {
        std::error_code ec;
        if (!fs::exists(file, ec)) return "module not found: " + file.string();
        return {};
    }
    return checkPlatform(file);
}

void setModuleLoadHookForTesting(ModuleLoadHook hook) { g_hook.store(hook); }

ModuleLoadResult loadTrustedModule(const fs::path& file, bool allowInsecure) {
    ModuleLoadResult r;
    if (file.empty()) {
        r.error = "no module path";
        r.refused = true;
        return r;
    }
    if (!file.is_absolute()) {
        r.error = "the module path must be absolute: " + file.string();
        r.refused = true;
        return r;
    }
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        r.error = "module not found: " + file.string();
        r.refused = true;
        return r;
    }
#if defined(_WIN32)
    HANDLE guard = INVALID_HANDLE_VALUE;
    if (!allowInsecure) {
        if (auto why = checkPlatform(file); !why.empty()) {
            r.error = std::move(why);
            r.refused = true;
            return r;
        }
        // Deny write / delete / rename by others while we load (loader opens with read|delete sharing).
        guard = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (guard == INVALID_HANDLE_VALUE) {
            r.error = "module cannot be opened exclusively: " + file.string();
            r.refused = true;
            return r;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(guard, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            ::CloseHandle(guard);
            r.error = "module is not a plain regular file: " + file.string();
            r.refused = true;
            return r;
        }
        if (const ModuleLoadHook hook = g_hook.load()) hook(file);
    }
    HMODULE h = ::LoadLibraryW(file.c_str());
    if (!h) r.error = "LoadLibrary failed (error " + std::to_string(::GetLastError()) + ")";
    if (guard != INVALID_HANDLE_VALUE) ::CloseHandle(guard);
    r.handle = reinterpret_cast<void*>(h);
    return r;
#else
    if (allowInsecure) {
        ::dlerror();
        r.handle = ::dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!r.handle) r = loaderFailure("dlopen failed");
        return r;
    }
    return loadPosix(file);
#endif
}

} // namespace rivet::crypto

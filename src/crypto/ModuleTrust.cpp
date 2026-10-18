// SPDX-License-Identifier: MPL-2.0
#include "crypto/ModuleTrust.hpp"

#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rivet::crypto {

namespace {

namespace fs = std::filesystem;

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
    if (!S_ISREG(st.st_mode)) return "module is not a regular file: " + real.string();
    if (st.st_uid != 0 && st.st_uid != ::geteuid())
        return "module is owned by another user (expected root or the current user): " + real.string();
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) return "module is group- or world-writable: " + real.string();
    return {};
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

} // namespace rivet::crypto

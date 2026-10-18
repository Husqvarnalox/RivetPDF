// SPDX-License-Identifier: MPL-2.0
#pragma once

// Pre-load trust check for native modules that Rivet dlopen()s / LoadLibrary()s
// (PKCS#11 token modules, CryptoPro CSP libraries). A module is arbitrary native
// code running with the user's rights, so before loading it Rivet refuses files
// another local user could have planted or could replace.
//
// POSIX rules (stat-based):
//   - the path is absolute;
//   - symbolic links are resolved (realpath) and the RESOLVED file is checked, so
//     a link cannot lead to an untrusted target; the directory holding the link is
//     checked as well (an attacker able to write there could re-point it);
//   - the resolved file is a regular file, owned by root or the current user, and
//     neither group- nor other-writable;
//   - the parent directory (of the link and of the resolved file) is not
//     other-writable, unless it has the sticky bit (like /tmp, where others cannot
//     replace or rename entries they do not own).
// Windows: documented best-effort. Only std::filesystem is used: the path must be
// absolute and drive-letter based (UNC and \\?\ / \\.\ device paths are refused),
// and must name an existing regular file; no ACL or signature inspection is done.
// checkModuleFile() alone is NOT atomic with a later load (TOCTOU). Loaders must
// use loadTrustedModule() below, which validates and loads the SAME file object:
//   - Linux: the file is opened once (O_NOFOLLOW), validated with fstat on that
//     descriptor, and dlopen()ed through /proc/self/fd/N, so replacing or
//     re-pointing the path afterwards cannot change what is loaded. Limits: a
//     module that relies on $ORIGIN to find its own dependencies cannot be
//     loaded that way; the loader then falls back to the macOS strategy; the
//     check of the directories above the file is still path based.
//   - macOS (and the Linux fallback, or when /proc is unavailable): there is no
//     dlopen-from-descriptor. The file is opened once and validated with fstat,
//     the path is dlopen()ed immediately, and afterwards dev/ino of the path are
//     compared with the validated descriptor; a mismatch unloads the library and
//     fails. REMAINING LIMIT: static initialisers of the swapped-in library have
//     already run when the mismatch is detected, so this narrows the window to
//     the dlopen call itself but does not eliminate it.
//   - Windows: the file is opened with FILE_SHARE_READ only (no write / delete /
//     rename by others) and kept open across LoadLibraryW. Not built or tested
//     on Windows here.
// None of this is a substitute for code signing.
//
// `allowInsecure` is the explicit development override: it skips everything but
// "absolute and exists". Production callers leave it false.

#include <filesystem>
#include <string>

namespace rivet::crypto {

// Returns an empty string when `file` may be loaded, otherwise a short reason
// (never contains anything but the path and the rule that failed).
std::string checkModuleFile(const std::filesystem::path& file, bool allowInsecure = false);

struct ModuleLoadResult {
    void* handle = nullptr; // dlopen / LoadLibrary handle, null on failure
    std::string error;      // reason when handle is null
    bool refused = false;   // true: the trust check refused the file; false: the loader itself failed
};

// Trust check and load as one operation (see above). POSIX: dlopen with
// RTLD_NOW | RTLD_LOCAL. Close the handle with dlclose / FreeLibrary.
// `allowInsecure` keeps its meaning (absolute + exists only, loaded by path).
ModuleLoadResult loadTrustedModule(const std::filesystem::path& file, bool allowInsecure = false);

// Test seam: called after the file was validated and before it is loaded, so a
// test can swap the file in that window. Pass nullptr to remove. Not for production.
using ModuleLoadHook = void (*)(const std::filesystem::path& file);
void setModuleLoadHookForTesting(ModuleLoadHook hook);

} // namespace rivet::crypto

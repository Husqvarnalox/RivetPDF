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
// The check is not atomic with the later load (TOCTOU); it blocks the common
// planting cases and is not a substitute for code signing.
//
// `allowInsecure` is the explicit development override: it skips everything but
// "absolute and exists". Production callers leave it false.

#include <filesystem>
#include <string>

namespace rivet::crypto {

// Returns an empty string when `file` may be loaded, otherwise a short reason
// (never contains anything but the path and the rule that failed).
std::string checkModuleFile(const std::filesystem::path& file, bool allowInsecure = false);

} // namespace rivet::crypto

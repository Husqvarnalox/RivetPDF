// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/AppDataLocation.hpp"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace rivet::app {

// The recently opened documents shown by the welcome view (ADR-0033 §5):
// most recent first, at most kMaxEntries, paths only (nothing about the
// content), stored as one UTF-8 path per line in "recent-files.txt" under
// the application-data directory. A missing or unreadable store is an empty
// list; write failures are ignored (the list is a convenience). Main thread.
class RecentFiles {
public:
    static constexpr std::size_t kMaxEntries = 10;

    // `appData` may be null: the list then lives in memory only.
    explicit RecentFiles(const platform::IAppDataLocation* appData);

    const std::vector<std::filesystem::path>& entries() const { return entries_; }
    // Moves (or inserts) `path` to the front, trims to kMaxEntries, saves.
    void add(const std::filesystem::path& path);
    void clear();

    // The store's file (empty when there is no app-data location).
    std::filesystem::path storePath() const;

private:
    void load();
    void save() const;

    const platform::IAppDataLocation* appData_;
    std::vector<std::filesystem::path> entries_;
};

} // namespace rivet::app

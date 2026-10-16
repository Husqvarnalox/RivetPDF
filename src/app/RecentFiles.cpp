// SPDX-License-Identifier: MPL-2.0
#include "app/RecentFiles.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>

namespace rivet::app {
namespace {

constexpr const char* kStoreName = "recent-files.txt";

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

} // namespace

RecentFiles::RecentFiles(const platform::IAppDataLocation* appData) : appData_(appData) { load(); }

std::filesystem::path RecentFiles::storePath() const {
    if (appData_ == nullptr) return {};
    const auto directory = appData_->directory();
    if (!directory.has_value()) return {};
    return *directory / kStoreName;
}

void RecentFiles::load() {
    entries_.clear();
    const std::filesystem::path store = storePath();
    if (store.empty()) return;
    std::ifstream in(store, std::ios::binary);
    std::string line;
    while (entries_.size() < kMaxEntries && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::filesystem::path path = fromUtf8(line);
        if (std::find(entries_.begin(), entries_.end(), path) == entries_.end()) entries_.push_back(path);
    }
}

void RecentFiles::save() const {
    const std::filesystem::path store = storePath();
    if (store.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(store.parent_path(), error);
    std::ofstream out(store, std::ios::binary | std::ios::trunc);
    for (const std::filesystem::path& path : entries_) out << toUtf8(path) << '\n';
}

void RecentFiles::add(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) absolute = path;
    absolute = absolute.lexically_normal();
    // A newline would split the entry: never store such a path.
    const std::string text = toUtf8(absolute);
    if (text.find('\n') != std::string::npos || text.find('\r') != std::string::npos) return;
    std::erase(entries_, absolute);
    entries_.insert(entries_.begin(), absolute);
    if (entries_.size() > kMaxEntries) entries_.resize(kMaxEntries);
    save();
}

void RecentFiles::clear() {
    entries_.clear();
    save();
}

} // namespace rivet::app

// SPDX-License-Identifier: MPL-2.0
#include "net/Http.hpp"

namespace rivet::net {

namespace {
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
} // namespace

std::optional<std::string_view> findHeader(const HttpHeaders& headers, std::string_view name) {
    for (const auto& [key, value] : headers) {
        if (key.size() != name.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < key.size(); ++i)
            if (lower(key[i]) != lower(name[i])) same = false;
        if (same) return std::string_view(value);
    }
    return std::nullopt;
}

} // namespace rivet::net

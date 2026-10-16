// SPDX-License-Identifier: MPL-2.0
#include "LinuxXSettings.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace rivet::platform {
namespace {

class Reader {
public:
    Reader(std::span<const std::uint8_t> data, bool msbFirst) : data_(data), msb_(msbFirst) {}

    bool skip(std::size_t count) {
        if (count > data_.size() - offset_) return false;
        offset_ += count;
        return true;
    }
    std::optional<std::uint32_t> card(std::size_t bytes) {
        if (bytes > data_.size() - offset_) return std::nullopt;
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            const std::uint32_t byte = data_[offset_ + (msb_ ? i : bytes - 1 - i)];
            value = (value << 8U) | byte;
        }
        offset_ += bytes;
        return value;
    }
    std::optional<std::string_view> bytes(std::size_t count) {
        if (count > data_.size() - offset_) return std::nullopt;
        const auto* begin = reinterpret_cast<const char*>(data_.data() + offset_);
        offset_ += count;
        return std::string_view{begin, count};
    }
    bool align4() { return skip((4 - offset_ % 4) % 4); }

private:
    std::span<const std::uint8_t> data_;
    bool msb_;
    std::size_t offset_ = 0;
};

} // namespace

std::optional<std::string> xsettingsString(std::span<const std::uint8_t> data, std::string_view name) {
    if (data.size() < 12) return std::nullopt;
    Reader reader(data, data[0] != 0);
    if (!reader.skip(4) || !reader.card(4)) return std::nullopt; // byte order + pad, serial
    const std::optional<std::uint32_t> count = reader.card(4);
    if (!count) return std::nullopt;
    for (std::uint32_t i = 0; i < *count; ++i) {
        const std::optional<std::uint32_t> type = reader.card(1);
        if (!type || !reader.skip(1)) return std::nullopt;
        const std::optional<std::uint32_t> nameLength = reader.card(2);
        if (!nameLength) return std::nullopt;
        const std::optional<std::string_view> settingName = reader.bytes(*nameLength);
        if (!settingName || !reader.align4() || !reader.card(4)) return std::nullopt; // last-change serial
        switch (*type) {
        case 0: // integer
            if (!reader.card(4)) return std::nullopt;
            break;
        case 1: { // string
            const std::optional<std::uint32_t> length = reader.card(4);
            if (!length) return std::nullopt;
            const std::optional<std::string_view> value = reader.bytes(*length);
            if (!value || !reader.align4()) return std::nullopt;
            if (*settingName == name) return std::string(*value);
            break;
        }
        case 2: // color: 4 x CARD16
            if (!reader.skip(8)) return std::nullopt;
            break;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

bool themeNameIsDark(std::string_view themeName) {
    std::string lower(themeName);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("dark") != std::string::npos;
}

} // namespace rivet::platform

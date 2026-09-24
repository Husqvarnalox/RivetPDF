#pragma once

#include <string_view>

namespace rivet::core::log {

// Minimal built-in logging. No third-party dependency, stderr sink,
// thread-safe, timestamped.
//
// PRIVACY: never log document contents, paths selected for anonymization,
// or user text. Log operational events only.

enum class Level : std::uint8_t {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
};

void setLevel(Level level);
Level level();

void write(Level level, std::string_view message);

inline void debug(std::string_view message) { write(Level::Debug, message); }
inline void info(std::string_view message) { write(Level::Info, message); }
inline void warning(std::string_view message) { write(Level::Warning, message); }
inline void error(std::string_view message) { write(Level::Error, message); }

} // namespace rivet::core::log

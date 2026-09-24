#include "core/Log.hpp"
#include "core/Time.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace rivet::core::log {

namespace {

std::mutex& logMutex() {
    static std::mutex m;
    return m;
}

Level& currentLevel() {
    static Level level = [] {
        // RIVET_LOG=debug|info|warning|error overrides the default (Info).
        if (const char* env = std::getenv("RIVET_LOG")) {
            const std::string value = env;
            if (value == "debug") return Level::Debug;
            if (value == "warning") return Level::Warning;
            if (value == "error") return Level::Error;
        }
        return Level::Info;
    }();
    return level;
}

const char* levelTag(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARN";
        case Level::Error: return "ERROR";
    }
    return "?";
}

} // namespace

void setLevel(Level level) {
    std::lock_guard lock(logMutex());
    currentLevel() = level;
}

Level level() {
    std::lock_guard lock(logMutex());
    return currentLevel();
}

void write(Level level, std::string_view message) {
    {
        std::lock_guard lock(logMutex());
        if (level < currentLevel()) return;
    }

    const auto wallMs = wallClockMilliseconds();
    const std::time_t seconds = static_cast<std::time_t>(wallMs / 1000);
    const int millis = static_cast<int>(wallMs % 1000);

    std::tm local{};
    localtime_r(&seconds, &local);

    char timeBuffer[32];
    std::snprintf(timeBuffer, sizeof(timeBuffer), "%02d:%02d:%02d.%03d",
                  local.tm_hour, local.tm_min, local.tm_sec, millis);

    std::lock_guard lock(logMutex());
    std::fprintf(stderr, "[%s] [%s] %.*s\n",
                 timeBuffer, levelTag(level),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

} // namespace rivet::core::log

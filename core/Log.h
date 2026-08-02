#pragma once

#include <format>
#include <string_view>
#include <utility>

// Thin logging facade so the rest of the codebase never depends directly on a
// concrete logging library. When spdlog is available the facade forwards to it;
// otherwise it falls back to a small stderr/stdout writer. Message formatting
// always uses std::format (C++20), keeping call sites identical either way.
namespace sonar::log {

enum class Level { Trace, Debug, Info, Warn, Error, Off };

void init(Level level = Level::Info);
void shutdown();
void setLevel(Level level);

// Single sink used by every templated helper below.
void write(Level level, std::string_view message);

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace sonar::log

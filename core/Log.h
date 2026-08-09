#pragma once

#include <string_view>
#include <utility>

#include "core/Format.h"

// Thin logging facade so the rest of the codebase never depends directly on a
// concrete logging library. When spdlog is available the facade forwards to it;
// otherwise it falls back to a small stderr/stdout writer. Message formatting
// uses the project's own brace formatter (see core/Format.h), so the code stays
// buildable on compilers whose libstdc++ predates std::format.
namespace sonar::log {

enum class Level { Trace, Debug, Info, Warn, Error, Off };

void init(Level level = Level::Info);
void shutdown();
void setLevel(Level level);

// Single sink used by every templated helper below.
void write(Level level, std::string_view message);

template <class... Args>
void trace(std::string_view fmt, Args&&... args) {
    write(Level::Trace, sonar::fmt::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void debug(std::string_view fmt, Args&&... args) {
    write(Level::Debug, sonar::fmt::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void info(std::string_view fmt, Args&&... args) {
    write(Level::Info, sonar::fmt::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void warn(std::string_view fmt, Args&&... args) {
    write(Level::Warn, sonar::fmt::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::string_view fmt, Args&&... args) {
    write(Level::Error, sonar::fmt::format(fmt, std::forward<Args>(args)...));
}

}  // namespace sonar::log

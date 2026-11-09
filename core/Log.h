#pragma once

#include <string_view>
#include <utility>

#include "core/Format.h"

// Logging facade: forwards to spdlog when available, else writes to stderr/stdout.
// Formatting uses core/Format.h.
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

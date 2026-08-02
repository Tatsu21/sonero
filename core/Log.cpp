#include "core/Log.h"

#ifdef SONAR_HAVE_SPDLOG
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#else
#include <cstdio>
#include <mutex>
#endif

namespace sonar::log {

#ifdef SONAR_HAVE_SPDLOG

namespace {
spdlog::level::level_enum toSpdLevel(Level level) {
    switch (level) {
        case Level::Trace: return spdlog::level::trace;
        case Level::Debug: return spdlog::level::debug;
        case Level::Info:  return spdlog::level::info;
        case Level::Warn:  return spdlog::level::warn;
        case Level::Error: return spdlog::level::err;
        case Level::Off:   return spdlog::level::off;
    }
    return spdlog::level::info;
}
}  // namespace

void init(Level level) {
    static auto logger = spdlog::stdout_color_mt("sonar");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%-5l%$] %v");
    setLevel(level);
}

void shutdown() { spdlog::shutdown(); }

void setLevel(Level level) { spdlog::set_level(toSpdLevel(level)); }

void write(Level level, std::string_view message) {
    spdlog::log(toSpdLevel(level), "{}", message);
}

#else  // ----------------------------------------------------------- fallback

namespace {
std::mutex g_mutex;
Level g_level = Level::Info;

const char* levelName(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
        case Level::Off:   return "OFF";
    }
    return "INFO";
}
}  // namespace

void init(Level level) { g_level = level; }

void shutdown() {}

void setLevel(Level level) { g_level = level; }

void write(Level level, std::string_view message) {
    if (g_level == Level::Off ||
        static_cast<int>(level) < static_cast<int>(g_level)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::FILE* out = (level == Level::Error || level == Level::Warn) ? stderr : stdout;
    std::fprintf(out, "[%-5s] %.*s\n", levelName(level),
                 static_cast<int>(message.size()), message.data());
}

#endif

}  // namespace sonar::log

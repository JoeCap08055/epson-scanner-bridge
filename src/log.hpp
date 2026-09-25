#pragma once

#include <string>

namespace logx {

enum class Level { debug = 0, info, warn, error };

void set_level(Level level);
bool parse_level(const std::string& s, Level& out);

void write(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

} // namespace logx

#define LOG_DEBUG(...) ::logx::write(::logx::Level::debug, __VA_ARGS__)
#define LOG_INFO(...)  ::logx::write(::logx::Level::info,  __VA_ARGS__)
#define LOG_WARN(...)  ::logx::write(::logx::Level::warn,  __VA_ARGS__)
#define LOG_ERROR(...) ::logx::write(::logx::Level::error, __VA_ARGS__)

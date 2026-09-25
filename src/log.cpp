#include "log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace logx {

namespace {
std::atomic<Level> g_level{Level::info};
std::mutex g_mtx;

// Under systemd, stderr goes to the journal, and sd-daemon(3) priority
// prefixes let journald assign the right severity.
const bool g_journal = std::getenv("JOURNAL_STREAM") != nullptr;

const char* prefix(Level level)
{
    switch (level) {
    case Level::debug: return g_journal ? "<7>debug: " : "debug: ";
    case Level::info:  return g_journal ? "<6>info: "  : "info: ";
    case Level::warn:  return g_journal ? "<4>warn: "  : "warn: ";
    case Level::error: return g_journal ? "<3>error: " : "error: ";
    }
    return "";
}
} // namespace

void set_level(Level level) { g_level = level; }

bool parse_level(const std::string& s, Level& out)
{
    if (s == "debug") { out = Level::debug; return true; }
    if (s == "info")  { out = Level::info;  return true; }
    if (s == "warn" || s == "warning") { out = Level::warn; return true; }
    if (s == "error") { out = Level::error; return true; }
    return false;
}

void write(Level level, const char* fmt, ...)
{
    if (level < g_level.load()) return;

    std::lock_guard<std::mutex> lock(g_mtx);
    std::fputs(prefix(level), stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace logx

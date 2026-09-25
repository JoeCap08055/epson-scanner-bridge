#include "config.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

std::string trim(const std::string& s)
{
    const char* ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Drops a trailing "# comment" or "; comment". A comment marker must be at
// the start of the value or follow whitespace, so values such as paths
// containing '#' survive.
std::string strip_inline_comment(const std::string& s)
{
    for (size_t i = 0; i < s.size(); ++i) {
        if ((s[i] == '#' || s[i] == ';') && (i == 0 || s[i-1] == ' ' || s[i-1] == '\t')) {
            return s.substr(0, i);
        }
    }
    return s;
}

bool parse_bool(const std::string& v, bool& out)
{
    if (v == "true" || v == "yes" || v == "on" || v == "1")  { out = true;  return true; }
    if (v == "false" || v == "no" || v == "off" || v == "0") { out = false; return true; }
    return false;
}

bool parse_int(const std::string& v, long min, long max, int base, long& out)
{
    if (v.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, base);
    if (errno != 0 || *end != '\0' || n < min || n > max) return false;
    out = n;
    return true;
}

} // namespace

bool load_config(const std::string& path, Config& out, std::string& err)
{
    std::ifstream in(path);
    if (!in) {
        err = path + ": cannot open: " + std::strerror(errno);
        return false;
    }

    Config cfg;
    std::string line;
    int lineno = 0;
    auto fail = [&](const std::string& msg) {
        err = path + ":" + std::to_string(lineno) + ": " + msg;
        return false;
    };

    while (std::getline(in, line)) {
        ++lineno;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') continue;  // section headers are ignored

        size_t eq = t.find('=');
        if (eq == std::string::npos) return fail("expected 'key = value'");

        std::string key = trim(t.substr(0, eq));
        std::string val = trim(strip_inline_comment(t.substr(eq + 1)));
        long n = 0;

        if (key == "scanner_address") {
            cfg.scanner_address = val;
        } else if (key == "netif_path") {
            if (val.empty()) return fail("netif_path must not be empty");
            cfg.netif_path = val;
        } else if (key == "socket_path") {
            if (val.empty()) return fail("socket_path must not be empty");
            cfg.socket_path = val;
        } else if (key == "socket_mode") {
            if (!parse_int(val, 0, 07777, 8, n)) return fail("socket_mode must be an octal mode, e.g. 0660");
            cfg.socket_mode = static_cast<mode_t>(n);
        } else if (key == "socket_group") {
            cfg.socket_group = val;
        } else if (key == "prevent_timeout") {
            if (!parse_bool(val, cfg.prevent_timeout)) return fail("prevent_timeout must be true or false");
        } else if (key == "reconnect_min_s") {
            if (!parse_int(val, 1, 86400, 10, n)) return fail("reconnect_min_s must be 1..86400");
            cfg.reconnect_min_s = static_cast<int>(n);
        } else if (key == "reconnect_max_s") {
            if (!parse_int(val, 1, 86400, 10, n)) return fail("reconnect_max_s must be 1..86400");
            cfg.reconnect_max_s = static_cast<int>(n);
        } else if (key == "probe_port") {
            if (!parse_int(val, 0, 65535, 10, n)) return fail("probe_port must be 0..65535");
            cfg.probe_port = static_cast<int>(n);
        } else if (key == "cleanup_stale") {
            if (!parse_bool(val, cfg.cleanup_stale)) return fail("cleanup_stale must be true or false");
        } else if (key == "emit_status_events") {
            if (!parse_bool(val, cfg.emit_status_events)) return fail("emit_status_events must be true or false");
        } else if (key == "log_level") {
            if (!logx::parse_level(val, cfg.log_level)) return fail("log_level must be debug, info, warn or error");
        } else {
            LOG_WARN("%s:%d: unknown key '%s' ignored", path.c_str(), lineno, key.c_str());
        }
    }

    if (cfg.scanner_address.empty()) {
        err = path + ": scanner_address is required";
        return false;
    }
    if (cfg.reconnect_max_s < cfg.reconnect_min_s) {
        err = path + ": reconnect_max_s must be >= reconnect_min_s";
        return false;
    }
    if (cfg.socket_path.size() >= 108) {
        err = path + ": socket_path is too long for a Unix socket";
        return false;
    }

    out = cfg;
    return true;
}

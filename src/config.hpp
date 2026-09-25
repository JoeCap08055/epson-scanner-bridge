#pragma once

#include <string>
#include <sys/types.h>

#include "log.hpp"

#define DEFAULT_CONFIG_PATH "/etc/es2netif-bridge.conf"
#define DEFAULT_NETIF_PATH  "/usr/lib/x86_64-linux-gnu/epsonscan2/non-free-exec/es2netif"
#define DEFAULT_SOCKET_PATH "/run/es2netif-bridge/events.sock"

struct Config
{
    std::string scanner_address;
    std::string netif_path   = DEFAULT_NETIF_PATH;
    std::string socket_path  = DEFAULT_SOCKET_PATH;
    mode_t      socket_mode  = 0660;
    std::string socket_group;
    bool        prevent_timeout    = true;
    int         reconnect_min_s    = 5;
    int         reconnect_max_s    = 60;
    int         probe_port         = 1865;
    bool        cleanup_stale      = true;
    bool        emit_status_events = true;
    logx::Level log_level          = logx::Level::info;
};

// Parses and validates the file at `path`. On failure returns false and
// sets `err` to a message that includes the file name and line number.
bool load_config(const std::string& path, Config& out, std::string& err);

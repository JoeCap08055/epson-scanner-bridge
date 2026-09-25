#pragma once

// One session with an es2netif child process. This follows the procedure of
// ipc::IPCInterfaceImpl (epsonscan2, ipcInterfaceImpl.cpp) closely, but only
// covers the parts needed to receive interrupt events: fork/exec, connect,
// open, status, close.

#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>

#include "config.hpp"
#include "ipc_interrupt.hpp"

// es2netif hard-codes this location, so it cannot be configured.
#define NETIF_WORK_PATH "/tmp/epsonWork/"

class NetifSession
{
public:
    using EventSink = ipc::ipc_interrupt::event_callback;
    using PreventTimeout = ipc::ipc_interrupt::prevent_timeout_callback;
    using ErrorSink = ipc::ipc_interrupt::error_callback;

    // The callbacks run on the interrupt thread.
    NetifSession(EventSink on_event, PreventTimeout prevent_timeout, ErrorSink on_error);
    ~NetifSession();

    NetifSession(const NetifSession&) = delete;
    NetifSession& operator=(const NetifSession&) = delete;

    // Starts es2netif and opens "//<scanner_address>". On failure the session
    // is left fully closed and `err` explains why.
    bool open(const Config& cfg, std::string& err);

    // Sends the close request, stops the interrupt listener, terminates the
    // child and removes interrupt.dat. It is safe to call more than once.
    void close();

    bool is_open() const { return id_ > 0; }
    int socket_fd() const { return socket_; }
    pid_t pid() const { return pid_; }
    bool interrupt_supported() const { return interrupt_supported_; }
    bool extended_transfer_supported() const { return extended_transfer_supported_; }

    // Non-blocking check for the child having exited. If it has, it is reaped
    // and this returns true (with the wait status in `status`).
    bool reap_if_exited(int& status);

    // Handles readability on socket_fd(). Returns false if es2netif closed
    // the connection.
    bool check_socket();

    // Tries a plain TCP connect to host:port and gives up after `timeout_ms`.
    static bool probe_tcp(const std::string& host, int port, int timeout_ms, std::string& err);

    // Kills stray es2netif processes (other than our own child) and removes a
    // leftover interrupt shared-memory segment.
    void cleanup_stale(const std::string& netif_path);

private:
    bool fork_(const std::string& path, std::string& err);
    bool connect_(std::string& err);
    bool open_(const std::string& udi, key_t sem_key, std::string& err);
    bool get_status_(uint32_t status_type, uint32_t& stat);
    bool recv_reply_(uint32_t* token);
    void terminate_child_();

    ssize_t send_all_(const void* data, size_t size);
    ssize_t recv_all_(void* data, size_t size);
    ssize_t send_message_(ipc::ipc_header hdr, const char* payload);
    void set_timeout_(double seconds);

    EventSink on_event_;
    PreventTimeout prevent_timeout_;
    ErrorSink on_error_;

    pid_t    pid_ = -1;
    int      port_ = -1;
    int      socket_ = -1;
    uint32_t id_ = 0;
    bool     interrupt_supported_ = false;
    bool     extended_transfer_supported_ = false;
    bool     dat_owned_ = false;
    std::unique_ptr<ipc::ipc_interrupt> interrupt_;
};

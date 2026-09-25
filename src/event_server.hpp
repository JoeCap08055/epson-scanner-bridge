#pragma once

#include <string>
#include <sys/types.h>

// Unix-domain stream socket that sends NDJSON lines to a single client.
// When a new client connects, the previous one is disconnected.
class EventServer
{
public:
    EventServer() = default;
    ~EventServer();

    EventServer(const EventServer&) = delete;
    EventServer& operator=(const EventServer&) = delete;

    bool start(const std::string& path, mode_t mode, const std::string& group, std::string& err);
    void stop();

    int listen_fd() const { return listen_fd_; }
    int client_fd() const { return client_fd_; }
    bool has_client() const { return client_fd_ >= 0; }
    // True while buffered output is waiting for the socket to become writable.
    bool wants_write() const { return client_fd_ >= 0 && !pending_.empty(); }

    void on_listen_readable();
    // Call with the poll revents for client_fd().
    void on_client_event(short revents);

    // Queues `line` plus a newline for the current client. Without a client
    // the line is dropped.
    void send_line(const std::string& line);

private:
    void flush_();
    void drop_client_(const char* why);

    std::string path_;
    int listen_fd_ = -1;
    int client_fd_ = -1;
    std::string pending_;
};

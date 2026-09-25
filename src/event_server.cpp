#include "event_server.hpp"

#include <cerrno>
#include <cstring>
#include <grp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.hpp"

namespace {
// A client that falls further behind than this is disconnected.
const size_t max_pending = 64 * 1024;
}

EventServer::~EventServer()
{
    stop();
}

bool EventServer::start(const std::string& path, mode_t mode, const std::string& group, std::string& err)
{
    stop();

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        err = "socket path too long: " + path;
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    // Remove a stale socket, but refuse to delete anything that isn't one.
    struct stat st;
    if (lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            err = path + " exists and is not a socket";
            return false;
        }
        ::unlink(path.c_str());
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        err = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = "bind " + path + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    if (::listen(fd, 4) != 0) {
        err = std::string("listen: ") + std::strerror(errno);
        ::close(fd);
        ::unlink(path.c_str());
        return false;
    }
    if (chmod(path.c_str(), mode) != 0) {
        LOG_WARN("chmod %s: %s", path.c_str(), std::strerror(errno));
    }
    if (!group.empty()) {
        struct group* gr = getgrnam(group.c_str());
        if (!gr) {
            LOG_WARN("socket_group '%s' not found", group.c_str());
        } else if (chown(path.c_str(), static_cast<uid_t>(-1), gr->gr_gid) != 0) {
            LOG_WARN("chown %s to group %s: %s", path.c_str(), group.c_str(), std::strerror(errno));
        }
    }

    listen_fd_ = fd;
    path_ = path;
    LOG_INFO("listening on %s", path.c_str());
    return true;
}

void EventServer::stop()
{
    if (client_fd_ >= 0) drop_client_("server stopping");
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(path_.c_str());
        path_.clear();
    }
}

void EventServer::on_listen_readable()
{
    for (;;) {
        int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_WARN("accept: %s", std::strerror(errno));
            }
            return;
        }
        if (client_fd_ >= 0) drop_client_("replaced by new client");
        client_fd_ = fd;
        LOG_INFO("client connected");
    }
}

void EventServer::on_client_event(short revents)
{
    if (client_fd_ < 0) return;

    if (revents & (POLLIN | POLLHUP | POLLERR | POLLRDHUP)) {
        // Anything the client sends is ignored. We read only to notice EOF.
        char buf[512];
        for (;;) {
            ssize_t n = ::recv(client_fd_, buf, sizeof(buf), 0);
            if (n > 0) continue;
            if (n == 0) { drop_client_("client closed"); return; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            drop_client_(std::strerror(errno));
            return;
        }
        if (revents & (POLLHUP | POLLERR)) { drop_client_("client hung up"); return; }
    }
    if (revents & POLLOUT) flush_();
}

void EventServer::send_line(const std::string& line)
{
    if (client_fd_ < 0) return;
    if (pending_.size() + line.size() + 1 > max_pending) {
        drop_client_("client too slow");
        return;
    }
    pending_ += line;
    pending_ += '\n';
    flush_();
}

void EventServer::flush_()
{
    while (client_fd_ >= 0 && !pending_.empty()) {
        ssize_t n = ::send(client_fd_, pending_.data(), pending_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            pending_.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        drop_client_(n < 0 ? std::strerror(errno) : "write failed");
        return;
    }
}

void EventServer::drop_client_(const char* why)
{
    if (client_fd_ < 0) return;
    LOG_INFO("client disconnected (%s)", why);
    ::close(client_fd_);
    client_fd_ = -1;
    pending_.clear();
}

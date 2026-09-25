#include "netif_session.hpp"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "log.hpp"

namespace {

const double seconds = 1.0;
// Matches IPCInterfaceImpl::default_timeout_.
const double default_timeout = 60 * seconds;
// How long to wait for es2netif to print its port.
const int port_read_timeout_ms = 10000;
// Grace period after SIGHUP before resorting to SIGKILL.
const int terminate_grace_ms = 3000;

std::string errstr(const char* what)
{
    return std::string(what) + ": " + std::strerror(errno);
}

std::string dat_file_path()
{
    return std::string(NETIF_WORK_PATH) + IPC_INTERRUPT_DATA_FILE;
}

void sleep_ms(int ms)
{
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}

bool ensure_dat_file(std::string& err)
{
    if (mkdir(NETIF_WORK_PATH, 0777) != 0 && errno != EEXIST) {
        err = errstr("mkdir " NETIF_WORK_PATH);
        return false;
    }
    std::string dat = dat_file_path();
    struct stat st;
    if (stat(dat.c_str(), &st) == 0) return true;  // ftok() only needs the inode
    int fd = ::open(dat.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) {
        err = errstr(("create " + dat).c_str());
        return false;
    }
    ::close(fd);
    return true;
}

} // namespace

NetifSession::NetifSession(EventSink on_event, PreventTimeout prevent_timeout, ErrorSink on_error)
: on_event_(std::move(on_event))
, prevent_timeout_(std::move(prevent_timeout))
, on_error_(std::move(on_error))
{
}

NetifSession::~NetifSession()
{
    close();
}

// Port of IPCInterfaceImpl::Open(). One difference: Epson carries on without
// interrupts if the shared memory can't be set up, but interrupts are all this
// daemon exists for, so we treat that as a failed open.
bool NetifSession::open(const Config& cfg, std::string& err)
{
    close();
    interrupt_supported_ = false;
    extended_transfer_supported_ = false;

    // Create interrupt.dat before cleanup. The key of a stale segment comes
    // from ftok() on this file, and a recreated file often gets the same inode
    // back.
    if (!ensure_dat_file(err)) {
        return false;
    }
    if (cfg.cleanup_stale) {
        cleanup_stale(cfg.netif_path);
    }

    if (!fork_(cfg.netif_path, err)) {
        close();
        return false;
    }
    LOG_DEBUG("es2netif pid %d listening on port %d", pid_, port_);

    int tries_left = 5;
    std::string cerr;
    while (!connect_(cerr) && 0 < --tries_left) {
        sleep_ms(1000);
    }
    if (!tries_left) {
        err = "cannot connect to es2netif: " + cerr;
        close();
        return false;
    }

    try {
        dat_owned_ = true;
        interrupt_.reset(new ipc::ipc_interrupt(on_event_, prevent_timeout_, on_error_,
                                                dat_file_path(), IPC_SHARED_ID, IPC_SEMAHORE_KEY));
        interrupt_->start();
        LOG_DEBUG("interrupt channel ready, sem_key = %d", interrupt_->sem_key());
    } catch (const std::exception& e) {
        err = std::string("interrupt channel setup failed: ") + e.what() + " (" + std::strerror(errno) + ")";
        close();
        return false;
    }

    std::string udi = "//" + cfg.scanner_address;
    if (!open_(udi, interrupt_->sem_key(), err)) {
        close();
        return false;
    }

    uint32_t v = 0;
    if (!get_status_(ipc::status_interrupt_supported, v)) {
        err = "get status (interrupt_supported) failed";
        close();
        return false;
    }
    interrupt_supported_ = v != 0;

    v = 0;
    if (!get_status_(ipc::status_extended_transfer_supported, v)) {
        err = "get status (extended_transfer_supported) failed";
        close();
        return false;
    }
    extended_transfer_supported_ = v != 0;

    if (!interrupt_supported_) {
        LOG_WARN("es2netif reports that interrupts are not supported for %s; no events will arrive",
                 cfg.scanner_address.c_str());
    }
    return true;
}

// Port of IPCInterfaceImpl::Close().
void NetifSession::close()
{
    if (is_open() && socket_ >= 0) {
        ipc::ipc_header hdr{};
        ipc::hdr_token(hdr, id_);
        ipc::hdr_type(hdr, ipc::ipc_hdr_type_close);
        set_timeout_(2 * seconds);
        if (send_message_(hdr, nullptr) < 0) {
            LOG_DEBUG("failure sending close to es2netif");
        }
    }
    id_ = 0;

    if (interrupt_) {
        interrupt_->stop();
        interrupt_.reset();
    }

    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
    terminate_child_();
    port_ = -1;

    // Like Epson, remove interrupt.dat when the session ends, but only if this
    // session got as far as using it.
    if (dat_owned_) {
        dat_owned_ = false;
        std::string dat = dat_file_path();
        if (::unlink(dat.c_str()) != 0 && errno != ENOENT) {
            LOG_DEBUG("unlink %s: %s", dat.c_str(), std::strerror(errno));
        }
    }
}

bool NetifSession::reap_if_exited(int& status)
{
    if (pid_ <= 0) return false;
    pid_t w = waitpid(pid_, &status, WNOHANG);
    if (w == pid_) {
        pid_ = -1;
        return true;
    }
    return false;
}

bool NetifSession::check_socket()
{
    if (socket_ < 0) return false;
    char buf[256];
    for (;;) {
        ssize_t n = ::recv(socket_, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            LOG_DEBUG("discarding %zd unsolicited bytes from es2netif", n);
            continue;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        LOG_DEBUG("recv from es2netif: %s", std::strerror(errno));
        return false;
    }
}

// Port of kill_() in ipcInterfaceImpl.cpp. It adds a SIGKILL fallback so a
// wedged child can't stall the daemon.
void NetifSession::terminate_child_()
{
    if (pid_ <= 1) {
        pid_ = -1;
        return;
    }
    LOG_DEBUG("terminating es2netif pid %d (port %d)", pid_, port_);
    if (kill(pid_, SIGHUP) != 0 && errno != ESRCH) {
        LOG_WARN("kill es2netif: %s", std::strerror(errno));
    }
    int status = 0;
    for (int waited = 0; ; waited += 50) {
        pid_t w = waitpid(pid_, &status, WNOHANG);
        if (w == pid_ || (w < 0 && errno == ECHILD)) break;
        if (waited >= terminate_grace_ms) {
            LOG_WARN("es2netif pid %d ignored SIGHUP, sending SIGKILL", pid_);
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0);
            break;
        }
        sleep_ms(50);
    }
    pid_ = -1;
}

// Port of IPCInterfaceImpl::fork_().
bool NetifSession::fork_(const std::string& path, std::string& err)
{
    if (access(path.c_str(), X_OK) != 0) {
        err = errstr(("es2netif not executable: " + path).c_str());
        return false;
    }

    int pipe_fd[2] = {-1, -1};
    if (pipe2(pipe_fd, O_CLOEXEC) < 0) {
        err = errstr("pipe");
        return false;
    }

    pid_ = fork();
    if (pid_ == 0) {
        // The parent blocks signals so it can use a signalfd, and blocked masks
        // survive exec. Unblock everything so es2netif starts with the same
        // disposition it would get under epsonscan2.
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        signal(SIGTERM, SIG_IGN);
        signal(SIGINT,  SIG_IGN);

        ::close(pipe_fd[0]);
        if (dup2(pipe_fd[1], STDOUT_FILENO) >= 0) {
            execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        }
        // Tell the parent we failed.
        const char msg[] = "-1\n";
        ssize_t ignored = ::write(pipe_fd[1], msg, sizeof(msg) - 1);
        (void)ignored;
        _exit(EXIT_FAILURE);
    }

    ::close(pipe_fd[1]);
    if (pid_ < 0) {
        err = errstr("fork");
        ::close(pipe_fd[0]);
        return false;
    }

    // Read the first line from the child with a timeout. The parent's copy of
    // the write end is already closed, so if the child dies we see EOF rather
    // than blocking.
    std::string line;
    struct pollfd pfd = {pipe_fd[0], POLLIN, 0};
    int remaining_ms = port_read_timeout_ms;
    bool done = false;
    while (!done && remaining_ms > 0) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = poll(&pfd, 1, remaining_ms);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        remaining_ms -= static_cast<int>((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        char c;
        ssize_t n = ::read(pipe_fd[0], &c, 1);
        if (n <= 0) break;
        if (c == '\n') done = true;
        else if (line.size() < 64) line += c;
    }
    ::close(pipe_fd[0]);

    port_ = -1;
    if (std::sscanf(line.c_str(), "%d", &port_) != 1) {
        port_ = -1;
    }
    if (port_ <= 0 || port_ > 65535) {
        int status = 0;
        if (waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = -1;
            err = "es2netif exited prematurely";
        } else {
            err = "es2netif did not report a valid port (got '" + line + "')";
        }
        port_ = -1;
        return false;
    }
    return true;
}

void NetifSession::set_timeout_(double t_sec)
{
    if (socket_ < 0) return;
    struct timeval t;
    t.tv_sec = static_cast<time_t>(t_sec);
    t.tv_usec = static_cast<suseconds_t>((t_sec - t.tv_sec) * 1000000);
    if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t)) < 0 ||
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t)) < 0) {
        LOG_DEBUG("setsockopt timeout: %s", std::strerror(errno));
    }
    int flag = 1;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

// Port of IPCInterfaceImpl::connect_().
bool NetifSession::connect_(std::string& err)
{
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
    socket_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_ < 0) {
        err = errstr("socket");
        return false;
    }
    set_timeout_(10 * seconds);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(socket_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = errstr("connect");
        ::close(socket_);
        socket_ = -1;
        return false;
    }
    return true;
}

// Port of IPCInterfaceImpl::recv_reply().
bool NetifSession::recv_reply_(uint32_t* token)
{
    ipc::ipc_header hdr{};
    ssize_t n = recv_all_(&hdr, sizeof(hdr));
    if (n < 0) return false;

    int32_t size = ipc::hdr_size(hdr);
    if (size > 0) {
        if (size > (1 << 20)) return false;
        std::unique_ptr<char[]> payload(new char[size]);
        if (recv_all_(payload.get(), static_cast<size_t>(size)) < 0) return false;
    }
    if (token) *token = ipc::hdr_token(hdr);
    if (ipc::hdr_error(hdr)) {
        LOG_DEBUG("es2netif reply error %u (token %u)", ipc::hdr_error(hdr), ipc::hdr_token(hdr));
        return false;
    }
    return true;
}

// Port of IPCInterfaceImpl::open_().
bool NetifSession::open_(const std::string& udi, key_t sem_key, std::string& err)
{
    ipc::ipc_header hdr{};
    ipc::hdr_type(hdr, ipc::ipc_hdr_type_open);
    if (sem_key > 0) {
        ipc::hdr_extension(hdr, static_cast<uint32_t>(sem_key));
    }
    ipc::hdr_size(hdr, static_cast<int32_t>(udi.length()));

    ssize_t n = send_message_(hdr, udi.c_str());
    if (n != static_cast<ssize_t>(udi.length())) {
        err = "failed to send open request to es2netif";
        return false;
    }
    uint32_t token = 0;
    if (!recv_reply_(&token) || token == 0) {
        err = "es2netif could not open " + udi;
        return false;
    }
    id_ = token;
    set_timeout_(default_timeout);
    return true;
}

// Port of IPCInterfaceImpl::get_status_(uint32_t, uint32_t&).
bool NetifSession::get_status_(uint32_t status_type, uint32_t& stat)
{
    set_timeout_(default_timeout);

    ipc::ipc_header hdr{};
    ipc::hdr_token(hdr, id_);
    ipc::hdr_type(hdr, ipc::ipc_hdr_type_status);
    ipc::hdr_extension(hdr, status_type);
    if (send_all_(&hdr, sizeof(hdr)) <= 0) return false;

    if (recv_all_(&hdr, sizeof(hdr)) <= 0) return false;
    if (ipc::hdr_error(hdr) || ipc::hdr_size(hdr) != static_cast<int32_t>(sizeof(stat))) return false;

    uint32_t raw = 0;
    if (recv_all_(&raw, sizeof(raw)) < 0) return false;
    stat = ntohl(raw);
    return true;
}

// Port of IPCInterfaceImpl::send_message_(ipc_header, const char*).
ssize_t NetifSession::send_message_(ipc::ipc_header hdr, const char* payload)
{
    if (send_all_(&hdr, sizeof(hdr)) <= 0) return -1;
    int32_t size = ipc::hdr_size(hdr);
    if (size <= 0) return 0;
    if (!payload) return -1;
    return send_all_(payload, static_cast<size_t>(size));
}

ssize_t NetifSession::send_all_(const void* data, size_t size)
{
    if (socket_ < 0 || size == 0) return -1;
    const char* p = static_cast<const char*>(data);
    size_t n = 0;
    while (n < size) {
        ssize_t t = ::send(socket_, p + n, size - n, MSG_NOSIGNAL);
        if (t < 0) {
            if (errno == EINTR) continue;
            LOG_DEBUG("send to es2netif: %s", std::strerror(errno));
            return -1;
        }
        n += static_cast<size_t>(t);
    }
    return static_cast<ssize_t>(n);
}

ssize_t NetifSession::recv_all_(void* data, size_t size)
{
    if (socket_ < 0 || size == 0) return -1;
    char* p = static_cast<char*>(data);
    size_t n = 0;
    while (n < size) {
        ssize_t t = ::recv(socket_, p + n, size - n, 0);
        if (t < 0) {
            if (errno == EINTR) continue;
            LOG_DEBUG("recv from es2netif: %s", std::strerror(errno));
            return -1;
        }
        if (t == 0) {
            LOG_DEBUG("es2netif closed the connection");
            return -1;
        }
        n += static_cast<size_t>(t);
    }
    return static_cast<ssize_t>(n);
}

bool NetifSession::probe_tcp(const std::string& host, int port, int timeout_ms, std::string& err)
{
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string service = std::to_string(port);
    int gai = getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (gai != 0) {
        err = std::string("resolve ") + host + ": " + gai_strerror(gai);
        return false;
    }

    bool ok = false;
    err = "no addresses";
    for (struct addrinfo* ai = res; ai && !ok; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) { err = errstr("socket"); continue; }
        int r = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            r = poll(&pfd, 1, timeout_ms);
            if (r == 1) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
                if (soerr == 0) ok = true;
                else err = std::string("connect: ") + std::strerror(soerr);
            } else {
                err = "connect: timed out";
            }
        } else {
            err = errstr("connect");
        }
        ::close(fd);
    }
    freeaddrinfo(res);
    return ok;
}

// Epson's Engine.cpp runs `killall -9 -q es2netif` before starting a session.
// This does the same without a shell, and then removes a shared-memory
// segment left behind by a crashed run. That segment would otherwise make
// shmget(IPC_EXCL) fail.
void NetifSession::cleanup_stale(const std::string& netif_path)
{
    char target[PATH_MAX];
    if (!realpath(netif_path.c_str(), target)) {
        std::strncpy(target, netif_path.c_str(), sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    }

    if (DIR* d = opendir("/proc")) {
        while (struct dirent* e = readdir(d)) {
            char* end = nullptr;
            long pid = std::strtol(e->d_name, &end, 10);
            if (*end != '\0' || pid <= 1 || pid == getpid() || pid == pid_) continue;

            char link[64], exe[PATH_MAX];
            std::snprintf(link, sizeof(link), "/proc/%ld/exe", pid);
            ssize_t n = readlink(link, exe, sizeof(exe) - 1);
            if (n <= 0) continue;
            exe[n] = '\0';
            if (std::strcmp(exe, target) != 0) continue;

            LOG_WARN("killing stale es2netif process %ld", pid);
            if (kill(static_cast<pid_t>(pid), SIGKILL) == 0) {
                // It's reapable only if it was our child from an earlier session.
                waitpid(static_cast<pid_t>(pid), nullptr, WNOHANG);
            }
        }
        closedir(d);
    }

    std::string dat = dat_file_path();
    key_t key = ftok(dat.c_str(), IPC_SHARED_ID);
    if (key != -1) {
        int shmid = shmget(key, 0, 0);
        if (shmid >= 0) {
            LOG_WARN("removing stale interrupt shared memory segment (id %d)", shmid);
            shmctl(shmid, IPC_RMID, nullptr);
        }
    }
}

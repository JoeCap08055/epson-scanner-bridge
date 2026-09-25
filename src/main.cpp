// es-bridge: publishes Epson network-scanner interrupt events (button
// presses and similar) as NDJSON on a Unix domain socket.

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.hpp"
#include "event_server.hpp"
#include "json.hpp"
#include "log.hpp"
#include "netif_session.hpp"

namespace {

std::string now_rfc3339()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char buf[64];
    size_t n = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    std::snprintf(buf + n, sizeof(buf) - n, ".%03ldZ", ts.tv_nsec / 1000000L);
    return buf;
}

std::string printable_cstr(const uint8_t* data, size_t max)
{
    std::string s;
    for (size_t i = 0; i < max && data[i]; ++i) {
        if (data[i] >= 0x20 && data[i] < 0x7f) s += static_cast<char>(data[i]);
    }
    return s;
}

std::string hex(const uint8_t* data, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < len; ++i) {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 0xf];
    }
    return s;
}

std::string describe_wait_status(int status)
{
    if (WIFEXITED(status)) return "exit status " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return std::string("killed by signal ") + strsignal(WTERMSIG(status));
    return "status " + std::to_string(status);
}

class App
{
public:
    App(std::string config_path, Config cfg)
    : config_path_(std::move(config_path))
    , cfg_(std::move(cfg))
    , session_(
          [this](const ipc::ipc_interrupt_event_data& ev) { enqueue_(ev); },
          [this] { return prevent_timeout_.load(); },
          [this](int err) { sem_error_ = err; wake_(); })
    {
        prevent_timeout_ = cfg_.prevent_timeout;
    }

    ~App()
    {
        if (event_fd_ >= 0) ::close(event_fd_);
        if (timer_fd_ >= 0) ::close(timer_fd_);
        if (signal_fd_ >= 0) ::close(signal_fd_);
    }

    int run()
    {
        if (!setup_fds_()) return 1;

        std::string err;
        if (!server_.start(cfg_.socket_path, cfg_.socket_mode, cfg_.socket_group, err)) {
            LOG_ERROR("%s", err.c_str());
            return 1;
        }

        attempt_connect_();

        while (!exit_) {
            enum { P_SIG, P_EVENT, P_TIMER, P_LISTEN, P_CLIENT, P_NETIF, P_COUNT };
            struct pollfd pfd[P_COUNT];
            pfd[P_SIG]    = {signal_fd_, POLLIN, 0};
            pfd[P_EVENT]  = {event_fd_, POLLIN, 0};
            pfd[P_TIMER]  = {timer_fd_, POLLIN, 0};
            pfd[P_LISTEN] = {server_.listen_fd(), POLLIN, 0};
            pfd[P_CLIENT] = {server_.client_fd(),
                             static_cast<short>(POLLIN | POLLRDHUP | (server_.wants_write() ? POLLOUT : 0)), 0};
            pfd[P_NETIF]  = {connected_ ? session_.socket_fd() : -1, static_cast<short>(POLLIN | POLLRDHUP), 0};

            int r = poll(pfd, P_COUNT, -1);
            if (r < 0) {
                if (errno == EINTR) continue;
                LOG_ERROR("poll: %s", std::strerror(errno));
                break;
            }

            if (pfd[P_SIG].revents)    handle_signals_();
            if (exit_) break;
            if (pfd[P_EVENT].revents)  handle_events_();
            if (pfd[P_TIMER].revents)  handle_timer_();
            if (pfd[P_LISTEN].revents && server_.listen_fd() >= 0) server_.on_listen_readable();
            if (pfd[P_CLIENT].revents && pfd[P_CLIENT].fd == server_.client_fd()) {
                server_.on_client_event(pfd[P_CLIENT].revents);
            }
            if (pfd[P_NETIF].revents && connected_ && pfd[P_NETIF].fd == session_.socket_fd()) {
                if (!session_.check_socket()) {
                    disconnect_("es2netif closed its connection");
                }
            }
        }

        LOG_INFO("shutting down");
        if (connected_) {
            emit_status_(event_("disconnected").add("reason", "shutdown"));
        }
        session_.close();
        connected_ = false;
        server_.stop();
        return 0;
    }

private:
    // ---- setup -----------------------------------------------------------

    bool setup_fds_()
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGHUP);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGCHLD);
        // Blocking here, before any thread exists, means every thread inherits
        // the mask. NetifSession unblocks signals in the child before exec.
        // The interrupt thread's wake signal is blocked here too, but left out
        // of the signalfd. The listener thread unblocks it for itself, so only
        // stop()'s pthread_kill ever delivers it.
        sigset_t blocked = mask;
        sigaddset(&blocked, ipc::ipc_interrupt::wake_signal());
        if (sigprocmask(SIG_BLOCK, &blocked, nullptr) != 0) {
            LOG_ERROR("sigprocmask: %s", std::strerror(errno));
            return false;
        }
        signal(SIGPIPE, SIG_IGN);

        signal_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        event_fd_  = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        timer_fd_  = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (signal_fd_ < 0 || event_fd_ < 0 || timer_fd_ < 0) {
            LOG_ERROR("creating signalfd/eventfd/timerfd: %s", std::strerror(errno));
            return false;
        }
        return true;
    }

    // ---- interrupt thread -> main loop -----------------------------------

    void enqueue_(const ipc::ipc_interrupt_event_data& ev)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            queue_.push_back(ev);
        }
        wake_();
    }

    void wake_()
    {
        uint64_t one = 1;
        ssize_t ignored = ::write(event_fd_, &one, sizeof(one));
        (void)ignored;
    }

    void handle_events_()
    {
        uint64_t v;
        while (::read(event_fd_, &v, sizeof(v)) > 0) {}

        std::deque<ipc::ipc_interrupt_event_data> batch;
        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            batch.swap(queue_);
        }
        for (auto& ev : batch) {
            dispatch_(ev);
        }

        int serr = sem_error_.exchange(0);
        if (serr != 0 && connected_) {
            disconnect_(std::string("interrupt semaphore error: ") + std::strerror(serr));
        }
    }

    void dispatch_(ipc::ipc_interrupt_event_data& ev)
    {
        using namespace ipc;
        uint32_t type = static_cast<uint32_t>(ev._type);
        LOG_DEBUG("interrupt event type %u", type);

        switch (type) {
        case event_reserved_by_host:
            emit_(event_("reserved_by_host").add("address", printable_cstr(ev._data, IPC_INTERRUPT_DATA_SIZE - 1)));
            break;
        case event_did_press_button:
            emit_(event_("button_press").add("button", static_cast<int>(ev._data[0])));
            break;
        case event_request_start_scanning:
            emit_(event_("request_start_scanning"));
            break;
        case event_request_stop_scanning:
            emit_(event_("request_stop_scanning"));
            break;
        case event_request_start_or_stop:
            emit_(event_("request_start_or_stop"));
            break;
        case event_request_stop:
            emit_(event_("request_stop"));
            break;
        case event_did_timeout:
            emit_(event_("timeout"));
            disconnect_("scanner session timed out");
            break;
        case event_did_disconnect:
            emit_(event_("disconnect"));
            disconnect_("scanner disconnected");
            break;
        case event_receive_server_err:
            emit_(event_("server_error"));
            disconnect_("es2netif reported a server error");
            break;
        case event_device_comunication_err: {
            uint32_t code;
            std::memcpy(&code, &ev._data[0], sizeof(code));
            code = ntohl(code);
            emit_(event_("device_communication_error").add("code", code));
            disconnect_("device communication error " + std::to_string(code));
            break;
        }
        case ask_is_should_prevent_timeout:
            // The interrupt thread already answered; ev carries the answer.
            emit_(event_("prevent_timeout_query").add("answer", ev._recv_result != 0));
            break;
        default:
            emit_(event_("unknown").add("type", type).add("data_hex", hex(ev._data, IPC_INTERRUPT_DATA_SIZE)));
            break;
        }
    }

    // ---- connection state machine -----------------------------------------

    void attempt_connect_()
    {
        ++attempt_;
        std::string err;

        if (cfg_.probe_port > 0 &&
            !NetifSession::probe_tcp(cfg_.scanner_address, cfg_.probe_port, 3000, err)) {
            LOG_INFO("scanner %s:%d not reachable (%s)", cfg_.scanner_address.c_str(), cfg_.probe_port, err.c_str());
            schedule_reconnect_();
            return;
        }

        LOG_INFO("connecting to scanner %s via es2netif", cfg_.scanner_address.c_str());
        if (!session_.open(cfg_, err)) {
            LOG_WARN("open failed: %s", err.c_str());
            schedule_reconnect_();
            return;
        }

        connected_ = true;
        attempt_ = 0;
        backoff_s_ = cfg_.reconnect_min_s;
        disarm_timer_();
        LOG_INFO("connected to %s (interrupts %s, extended transfer %s)",
                 cfg_.scanner_address.c_str(),
                 session_.interrupt_supported() ? "supported" : "NOT supported",
                 session_.extended_transfer_supported() ? "supported" : "not supported");
        emit_status_(event_("connected")
                         .add("interrupt_supported", session_.interrupt_supported())
                         .add("extended_transfer_supported", session_.extended_transfer_supported()));
    }

    void disconnect_(const std::string& reason)
    {
        if (!connected_) return;
        LOG_WARN("disconnected: %s", reason.c_str());
        connected_ = false;
        session_.close();
        emit_status_(event_("disconnected").add("reason", reason));
        backoff_s_ = cfg_.reconnect_min_s;
        schedule_reconnect_();
    }

    void schedule_reconnect_()
    {
        int delay = backoff_s_ > 0 ? backoff_s_ : cfg_.reconnect_min_s;
        backoff_s_ = std::min(delay * 2, cfg_.reconnect_max_s);

        struct itimerspec its{};
        its.it_value.tv_sec = delay;
        timerfd_settime(timer_fd_, 0, &its, nullptr);

        LOG_INFO("reconnecting in %d s (attempt %d)", delay, attempt_ + 1);
        emit_status_(event_("reconnecting").add("attempt", attempt_ + 1).add("next_retry_s", delay));
    }

    void disarm_timer_()
    {
        struct itimerspec its{};
        timerfd_settime(timer_fd_, 0, &its, nullptr);
    }

    void handle_timer_()
    {
        uint64_t expirations;
        if (::read(timer_fd_, &expirations, sizeof(expirations)) <= 0) return;
        if (!connected_) attempt_connect_();
    }

    // ---- signals ----------------------------------------------------------

    void handle_signals_()
    {
        struct signalfd_siginfo si;
        while (::read(signal_fd_, &si, sizeof(si)) == sizeof(si)) {
            switch (si.ssi_signo) {
            case SIGTERM:
            case SIGINT:
                LOG_INFO("received %s", strsignal(static_cast<int>(si.ssi_signo)));
                exit_ = true;
                break;
            case SIGHUP:
                reload_();
                break;
            case SIGCHLD:
                reap_children_();
                break;
            }
        }
    }

    void reap_children_()
    {
        int status = 0;
        if (session_.reap_if_exited(status)) {
            std::string why = "es2netif exited (" + describe_wait_status(status) + ")";
            if (connected_) {
                disconnect_(why);
            } else {
                LOG_DEBUG("%s", why.c_str());
            }
        }
    }

    void reload_()
    {
        LOG_INFO("SIGHUP: reloading %s", config_path_.c_str());
        Config next;
        std::string err;
        if (!load_config(config_path_, next, err)) {
            LOG_ERROR("reload failed, keeping current configuration: %s", err.c_str());
            return;
        }

        if (connected_) {
            emit_status_(event_("disconnected").add("reason", "configuration reload"));
        }
        connected_ = false;
        session_.close();
        disarm_timer_();

        bool socket_changed = next.socket_path != cfg_.socket_path ||
                              next.socket_mode != cfg_.socket_mode ||
                              next.socket_group != cfg_.socket_group;
        cfg_ = next;
        logx::set_level(cfg_.log_level);
        prevent_timeout_ = cfg_.prevent_timeout;

        // Rebind only when the socket settings changed, so a connected client
        // survives an ordinary reload.
        if (socket_changed || server_.listen_fd() < 0) {
            server_.stop();
            if (!server_.start(cfg_.socket_path, cfg_.socket_mode, cfg_.socket_group, err)) {
                LOG_ERROR("%s", err.c_str());
            }
        }

        emit_status_(event_("config_reloaded"));
        attempt_ = 0;
        backoff_s_ = cfg_.reconnect_min_s;
        attempt_connect_();
    }

    // ---- output -----------------------------------------------------------

    JsonObject event_(const char* name)
    {
        JsonObject o;
        o.add("ts", now_rfc3339()).add("event", name).add("scanner", cfg_.scanner_address);
        return o;
    }

    void emit_(const JsonObject& o)
    {
        std::string line = o.str();
        LOG_DEBUG("emit %s", line.c_str());
        server_.send_line(line);
    }

    void emit_status_(const JsonObject& o)
    {
        if (cfg_.emit_status_events) emit_(o);
    }

    std::string config_path_;
    Config cfg_;
    EventServer server_;
    NetifSession session_;

    int signal_fd_ = -1;
    int event_fd_ = -1;
    int timer_fd_ = -1;

    std::mutex queue_mtx_;
    std::deque<ipc::ipc_interrupt_event_data> queue_;
    std::atomic<int> sem_error_{0};
    std::atomic<bool> prevent_timeout_{true};

    bool connected_ = false;
    bool exit_ = false;
    int attempt_ = 0;
    int backoff_s_ = 0;
};

void usage(const char* argv0)
{
    std::fprintf(stderr,
                 "usage: %s [-c config] [-t]\n"
                 "  -c PATH  configuration file (default " DEFAULT_CONFIG_PATH ")\n"
                 "  -t       validate the configuration and exit\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv)
{
    std::string config_path = DEFAULT_CONFIG_PATH;
    bool test_only = false;

    int opt;
    while ((opt = getopt(argc, argv, "c:th")) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 't': test_only = true; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    Config cfg;
    std::string err;
    if (!load_config(config_path, cfg, err)) {
        LOG_ERROR("%s", err.c_str());
        return 1;
    }
    logx::set_level(cfg.log_level);

    if (test_only) {
        std::printf("%s: OK\n", config_path.c_str());
        return 0;
    }

    App app(config_path, cfg);
    return app.run();
}

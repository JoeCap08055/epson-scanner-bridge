// Test double for Epson's es2netif, for exercising es2netif-bridge without a
// scanner. It speaks the same IPC protocol as the real helper:
//   1. prints its TCP port on stdout and accepts one connection;
//   2. answers the open request (token 7) and both status queries
//      (interrupt supported = 1, extended transfer = 0);
//   3. attaches to the host's shared memory and semaphore and fires events
//      with es2netif's handshake: write data, semop(-1), then
//      semtimedop(wait for 0, +1), then read _recv_result.
//
// It fires button_press(3), ask_is_should_prevent_timeout and
// reserved_by_host("10.0.0.9"), then does whatever FAKE_MODE says:
//   disconnect (default)  fire event_did_disconnect, then wait
//   exit                  exit abruptly (simulates a crash)
//   eof                   close the TCP socket, then wait
//   hold                  just wait
// The default SIGHUP action terminates it, as the bridge expects.
//
// FAKE_PAUSE_US sets the pause before the first event and before the final
// action (default 1500000). test_semaphore_race uses whole seconds (1000000),
// which lined up with the 1 s timed semaphore wait the bridge used to have.
//
// Usage: set netif_path to this binary and probe_port = 0 in the bridge config.
//
// Test helper mode: `fake_netif --make-stale-shm` creates a segment that
// collides with the bridge's interrupt segment (the same ftok key on
// /tmp/epsonWork/interrupt.dat), prints its shmid and exits, leaving the
// segment behind the way a crashed run would. The bridge never passes
// arguments, so this can't be triggered by accident.
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ipc_header.hpp"
#include "shared_memory.hpp"

using namespace ipc;

namespace {

void read_all(int fd, void* p, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, static_cast<char*>(p) + got, n - got);
        if (r <= 0) exit(10);
        got += static_cast<size_t>(r);
    }
}

void write_all(int fd, const void* p, size_t n)
{
    if (write(fd, p, n) != static_cast<ssize_t>(n)) exit(11);
}

} // namespace

int make_stale_shm()
{
    const char* dat = "/tmp/epsonWork/" IPC_INTERRUPT_DATA_FILE;
    key_t key = ftok(dat, IPC_SHARED_ID);
    if (key == -1) { perror("fake: ftok"); return 1; }
    int id = shmget(key, 40, IPC_CREAT | IPC_EXCL | 0600);
    if (id < 0) { perror("fake: shmget"); return 1; }
    printf("%d\n", id);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1) {
        if (std::string(argv[1]) == "--make-stale-shm") return make_stale_shm();
        fprintf(stderr, "usage: %s [--make-stale-shm]\n", argv[0]);
        return 2;
    }

    const char* env = getenv("FAKE_MODE");
    std::string mode = env ? env : "disconnect";
    const char* pause_env = getenv("FAKE_PAUSE_US");
    useconds_t pause_us = pause_env ? static_cast<useconds_t>(strtoul(pause_env, nullptr, 10)) : 1500000;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(ls, 1) != 0 ||
        getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        perror("fake: listen");
        return 1;
    }
    printf("%d\n", ntohs(addr.sin_port));
    fflush(stdout);

    int c = accept(ls, nullptr, nullptr);
    if (c < 0) { perror("fake: accept"); return 1; }

    // Open request: header plus UDI payload; extension carries the semaphore key.
    ipc_header h;
    read_all(c, &h, sizeof(h));
    int sem_key = static_cast<int>(hdr_extension(h));
    std::string udi(static_cast<size_t>(hdr_size(h)), '\0');
    if (!udi.empty()) read_all(c, &udi[0], udi.size());
    fprintf(stderr, "fake: open %s sem_key=%d\n", udi.c_str(), sem_key);

    ipc_header reply{};
    hdr_token(reply, 7);
    write_all(c, &reply, sizeof(reply));

    // Two status queries.
    for (int i = 0; i < 2; ++i) {
        read_all(c, &h, sizeof(h));
        ipc_header s{};
        hdr_token(s, 7);
        hdr_size(s, 4);
        write_all(c, &s, sizeof(s));
        uint32_t v = htonl(hdr_extension(h) == status_interrupt_supported ? 1 : 0);
        write_all(c, &v, sizeof(v));
    }

    shared_memory<ipc_interrupt_event_data> shm("/tmp/epsonWork/" IPC_INTERRUPT_DATA_FILE, IPC_SHARED_ID, false);
    int sem = semget(sem_key, 1, 0);
    if (sem < 0) { perror("fake: semget"); return 12; }

    auto fire = [&](interrupt_event_type type, const void* data, size_t n) -> uint32_t {
        ipc_interrupt_event_data& ev = shm.data();
        memset(&ev, 0, sizeof(ev));
        ev._type = type;
        if (data) memcpy(ev._data, data, n);
        sembuf release{0, -1, 0};
        semop(sem, &release, 1);
        sembuf reacquire[2] = {{0, 0, 0}, {0, 1, 0}};
        timespec ts{5, 0};
        if (semtimedop(sem, reacquire, 2, &ts) != 0) { perror("fake: semtimedop"); exit(13); }
        return ev._recv_result;
    };

    // Pause FAKE_PAUSE_US (see the header comment). Whole seconds used to line
    // up with the bridge's 1 s timed semaphore wait and reproduce Epson's
    // lost-event race. The bridge now waits with no timeout.
    usleep(pause_us);
    uint8_t button = 3;
    fire(event_did_press_button, &button, 1);
    uint32_t answer = fire(ask_is_should_prevent_timeout, nullptr, 0);
    fprintf(stderr, "fake: prevent_timeout answer=%u\n", answer);
    const char host[] = "10.0.0.9";
    fire(event_reserved_by_host, host, sizeof(host));
    usleep(pause_us);

    if (mode == "exit") {
        fprintf(stderr, "fake: crashing\n");
        _exit(3);
    }
    if (mode == "eof") {
        fprintf(stderr, "fake: closing socket\n");
        close(c);
    } else if (mode == "disconnect") {
        fire(event_did_disconnect, nullptr, 0);
    }
    for (;;) pause();
}

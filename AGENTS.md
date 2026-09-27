# AGENTS.md — es-bridge

Guidance for anyone, human or AI, changing this project. The README is written for users; this file records the design and the rules we decided to keep.

## Purpose

es-bridge is a standalone Linux daemon that:

1. **Launches `es2netif`** the same way epsonscan2 does and sets up its IPC. `es2netif` is Epson's proprietary helper for talking to network scanners.
2. **Receives scanner interrupt events** (scan button, session events) over the IPC channel.
3. **Publishes each event as one JSON line (NDJSON)** on a Unix domain socket, with the daemon acting as the server.
4. **Goes into reconnect mode** after any disconnect or failure, retrying with a growing delay.
5. **Takes its settings from an INI config file**, and **re-reads it and re-initializes on SIGHUP**.

The daemon never scans. It only relays events.

## Origin

- The upstream source is epsonscan2 6.7.92.0-1, in the sibling directory `../epsonscan2-6.7.92.0-1`.
- The protocol logic is a port of `src/ES2Command/Src/Interface/ipc/ipcInterfaceImpl.cpp`.
- The installed binary is `/usr/lib/x86_64-linux-gnu/epsonscan2/non-free-exec/es2netif`. It is not stripped, so `objdump -d -C` is useful for checking behavior.

## Architecture

```
src/main.cpp           App: poll loop, signalfd, reconnect state machine, event -> JSON dispatch
src/netif_session.*    One es2netif session: fork/exec, TCP connect, open, status, close, stale cleanup
src/event_server.*     Unix socket listener, single client, non-blocking buffered writes
src/config.*           INI parser and validation (load_config)
src/json.*             Minimal flat JSON object builder
src/log.*              Leveled stderr logging; adds sd-daemon <N> prefixes only when JOURNAL_STREAM is set
tests/fake_netif.cpp   Test double for es2netif (see Testing)
tests/run_tests.sh     End-to-end test suite, registered with CTest
tests/test_scan_trigger.sh  Tests for es-scan-trigger against a mock socket (CTest: scan_trigger)
contrib/scan-trigger/  es-scan-trigger: bash + socat client that runs a NAPS2 scan on request_start_scanning
vendor/epson/          Vendored Epson IPC headers (see "Vendored code")
```

### Threads

There are exactly two threads:

- **Main thread.** Everything except the semaphore wait runs here. `poll()` watches six fds:
  - the signalfd, for SIGHUP, SIGTERM, SIGINT and SIGCHLD;
  - an eventfd that the interrupt thread uses to wake the main loop;
  - a timerfd for reconnects;
  - the listening socket;
  - the client socket;
  - the TCP socket to es2netif, watched for EOF.
- **Interrupt thread**, owned by `ipc::ipc_interrupt`. It waits on the SysV semaphore and reads the shared-memory struct. It pushes each event into a mutex-protected queue and writes to the eventfd. It does nothing else.
  - It waits in `semop()` with **no timeout**. `stop()` wakes it with `SIGUSR1` (`ipc_interrupt::wake_signal()`) using `pthread_kill`.
  - The main thread blocks that signal before any thread starts, and it is not in the signalfd. Only the interrupt thread unblocks it, so a stray `kill -USR1` to the daemon only makes the listener loop once, which is harmless.

### Session lifecycle

`NetifSession::open()` follows these steps, in order:

1. Make sure `interrupt.dat` exists.
2. Clean up stale state (optional).
3. `fork_`: pipe, fork, then exec es2netif with stdout on the pipe, and read the port it prints.
4. `connect_`: connect to 127.0.0.1:port, up to 5 tries, 1 s apart.
5. Create the shared memory and semaphore, then start the interrupt thread.
6. `open_`: send an open request with the UDI `//<scanner_address>` and the semaphore key.
7. `get_status_`: ask whether interrupts are supported, then whether extended transfer is supported.

`close()` undoes this:

1. Send the close request.
2. Stop the interrupt thread, which removes the shared memory and semaphore.
3. Close the socket.
4. Send SIGHUP to es2netif, wait up to 3 s, then SIGKILL.
5. Unlink `interrupt.dat`.

### Connection states

There are two states: **connected** and **reconnecting**.

Any of these causes `disconnect_()`, which runs `close()`, emits `disconnected` and schedules a reconnect:

- es2netif events `disconnect`, `timeout`, `server_error` or `device_communication_error`;
- EOF on the es2netif socket;
- SIGCHLD for the es2netif child;
- a semaphore error.

A reconnect attempt first runs the TCP probe of `probe_port` (optional). If the probe passes, it runs `open()`. The delay starts at `reconnect_min_s` and doubles up to `reconnect_max_s`. It resets after a successful connect.

## Invariants: do not break these

### Protocol and IPC

- **The work path is fixed.** es2netif has `/tmp/epsonWork/interrupt.dat` compiled in (`NETIF_WORK_PATH`), so it must never become a config option. One consequence: **only one instance can run per host**, and it cannot run alongside an epsonscan2 scan session.
- **The shared-memory key is `ftok(interrupt.dat, 30)`. The semaphore starts at key 30**, and the vendored `semaphore` class bumps the key if 30 is taken. The actual key is sent to es2netif in the open header's `extension` field.
- **The semaphore handshake must match Epson's exactly.** The host creates the semaphore with value 1. The loop is `wait_and_lock` (wait for 0, then +1), handle the event, then `unlock` (-1).
- **Answer `ask_is_should_prevent_timeout` (type 200) inside the critical section.** `_recv_result` has to be written to shared memory **before** `unlock()`, because es2netif reads it straight after re-acquiring the semaphore. This is the only event handled on the interrupt thread; all others are just queued.
- **Wire format.** Header fields are big-endian (`htonl`/`ntohl`) and the structs are `#pragma pack(1)`. The `get_status_` value is a network-order uint32 that follows the reply header.
- **Match Epson's child setup.**
  - The child must reset its signal mask to empty before `execl`, because the parent blocks signals for the signalfd and a blocked mask survives exec.
  - It then sets SIGTERM and SIGINT to `SIG_IGN`, as Epson does.
  - If exec fails, it writes `-1\n` to the pipe.
- **The parent closes its copy of the pipe's write end before reading the port.** Otherwise a child that dies would make the read block forever. The port read uses a poll timeout of 10 s.
- **Every fd the daemon creates is `CLOEXEC`,** so es2netif doesn't inherit the daemon's sockets. The only exception is the pipe end that is dup2'd onto the child's stdout.
- **All sends use `MSG_NOSIGNAL`, and SIGPIPE is ignored.**

### Stale-state handling

- **Create `interrupt.dat` before stale cleanup.** A recreated file often reuses the old inode, so it produces the same ftok key as a leftover segment. Cleanup has to look up that key on the current file. This was a real bug and has been fixed.
- **Only unlink `interrupt.dat` if this session used it** (`dat_owned_`). An early `close()` must not delete the file before cleanup runs.
- **`cleanup_stale` kills other es2netif processes by matching `/proc/*/exe`.** There is no shell-out. It also removes a leftover shared-memory segment. It never touches the daemon's own child.
- **Never put a timeout back on the semaphore wait.** es2netif does `semop(-1)` and then immediately `semtimedop(wait for 0, +1)` (confirmed by disassembly).
  - While our thread is queued in `semop()`, the kernel completes our wait inside es2netif's release, so es2netif can't take the semaphore back first.
  - Epson's loop waited in 1 s timed steps. A release that landed between two waits was re-acquired by es2netif itself, and the event was lost. epsonscan2 still has this race.
  - We fixed it by waiting with no timeout and waking the thread with a signal on `stop()`.
  - Any exit path from the loop must still `unlock()` after an acquire, so es2netif isn't left waiting.
  - `test_semaphore_race` guards this: the fake pauses exactly 1 s, which lined up with the old timeout. The race window is only microseconds, so on an idle machine the test could still pass with the old code. It is a smoke test, not proof.
- **es2netif crashes unless it can append to `/tmp/test.txt`** (`NETIF_DEBUG_LOG`). Its `main()` does `fopen("/tmp/test.txt", "a")` and then `fprintf` without a NULL check (confirmed by disassembly; the kernel logs `segfault at c0 … in libc.so.6`). The bridge only sees "es2netif exited prematurely".
  - Ubuntu sets `fs.protected_regular = 2`. With that, an `O_CREAT` open of another user's file in `/tmp` fails with `EACCES`, even for root. So whoever ran es2netif first owns the trace, and everyone else's es2netif crashes. The one exception is a file owned by `/tmp`'s owner, root, which any user with write permission can open.
  - `prepare_debug_log()` does the same open before forking:
    - **As root:** if blocked and `cleanup_stale` is on, it removes the file and recreates it. It then makes the root-owned file `0666`, which makes it usable by every user.
    - **Otherwise:** it fails the open attempt with an error naming the file, and the fix when the file belongs to another uid.
  - Guard the `fchmod` with `fstat` on the open fd, `S_ISREG`, owner root and one link, and open with `O_NOFOLLOW`, so it can never be steered at another file.
  - The trace grows without bound (mostly `CheckEvent` lines). The bridge doesn't truncate it.
- **es2netif may ignore SIGHUP after the scanner disconnects** (seen on real hardware). Keep the SIGKILL fallback after the grace period.

### Output and socket

- **One client at a time.** A new connection replaces the old one. Anything the client sends is read and discarded.
- **Writes never block the main loop.** If more than 64 KiB is waiting to be written, the client is dropped.
- **Every JSON line has `ts` (UTC, RFC 3339 with milliseconds), `event` and `scanner`.**
  - Treat event names and fields as a stable API. Add new ones, but don't rename existing ones.
  - Status events (`connected`, `disconnected`, `reconnecting`, `config_reloaded`) can be turned off with `emit_status_events`. Scanner events are always emitted.
- **When unlinking a stale socket path, only remove it if it really is a socket.**

### Config and reload

- **The INI format.** It is `key = value`. `#` and `;` start a comment, either at the start of a line or after whitespace. `[section]` lines are ignored.
- **Unknown keys produce a warning; bad values are errors.** `scanner_address` is required. `-t` checks the file and exits.
- **SIGHUP loads the new file into a separate object first.** If it fails to parse, log the error and keep running on the old config, untouched.
- **A successful reload always restarts the session.** It **rebinds the socket only when `socket_path`, `socket_mode` or `socket_group` changed**, so a connected client survives an ordinary reload.
- **`prevent_timeout` defaults to true.** The interrupt thread reads it through an atomic, so a reload changes the answer without a race.

### es-scan-trigger (`contrib/scan-trigger/`)

- **It's a separate client of the socket.** It depends only on the JSON event format, never on es-bridge internals. It needs bash, socat and **jq**.
  - **Parse events with jq, not by position.** `parse_event()` makes one jq call per line and returns `event`, `ts` and `scanner` joined with `@tsv`, which escapes tabs and newlines inside values, so splitting on tabs is safe. Field order, spacing and extra fields don't matter.
  - Invalid JSON or a non-object is logged and skipped.
  - Lines not starting with `{` are treated as socat's own messages and never parsed.
- **It's a system service, not per-user,** and it only runs NAPS2. It runs as the account `es-scan` with `SupplementaryGroups=scanner` for socket access, or as root through a drop-in.
  - NAPS2 is always called with `--noprofile`, so no GUI profile or home directory of a real user is involved. Every scan option comes from the config (`NAPS2_*`).
  - NAPS2 still needs a writable `$HOME` for its own files. The unit sets `HOME` to the `StateDirectory` (`/var/lib/es-scan-trigger`). For root services systemd sets no `HOME` by default.
- **Build the NAPS2 command as an argument array, never a string for `bash -c`.** Empty settings leave their option out. `NAPS2_EXTRA_ARGS` is a bash array. `naps2_args()` prints the arguments NUL-separated and `mapfile -d ''` reads them back, so values with spaces survive.
- **Validate settings against NAPS2's documented values** (`--source glass|feeder|duplex`, `--bitdepth color|gray|bw`, `--driver sane|escl|wia|twain|apple`, a numeric `--dpi`). A bad value fails at startup, or is rejected on reload.
- **NAPS2 exits 0 even when it scanned nothing,** so success means a non-empty output file exists.
- **`AIRSCAN_URL` gives NAPS2 a private SANE configuration.** `setup_sane()` writes `$RUNTIME_DIRECTORY/sane/` (the unit sets `RuntimeDirectory=es-scan-trigger`) and exports `SANE_CONFIG_DIR` for NAPS2 only:
  - `dll.conf` contains just `airscan`;
  - `airscan.conf` declares `NAPS2_DEVICE` at the URL, with `discovery = disable`.

  Why:
  - Discovery was unreliable on real hardware (2026-09-25): the scanner's WSD metadata reply sometimes missed SANE's discovery window, so NAPS2 said "offline" or "could not be found". The static entry found it 3 times out of 3.
  - It also hides the epsonscan2 backend.

  Keep these rules:
  - **No trailing `:` on `SANE_CONFIG_DIR`.** That appends the system directories, whose `dll.d` brings epsonscan2 back.
  - **The directory must be complete.** A missing one means SANE finds no scanners.
  - **Never modify `/etc/sane.d`.**
- **Use `--device` (a partial match) carefully.** A name that matches the epsonscan2 device ("network scanner (epsonscan2)") would make NAPS2 open it through epsonscan2, which kills es-bridge's `es2netif`.
- **Scan commands must not use the epsonscan2 driver.** epsonscan2 runs `killall -9 es2netif` when it starts (`Controller/Src/Scanner/Engine.cpp:275`), which kills es-bridge's helper. NAPS2 via SANE `airscan`/eSCL is fine.
- **Read with `read -t 1`, keeping any partial line in `partial`.** bash 5.2 doesn't interrupt `read` for a trapped signal, so without the timeout a SIGHUP would wait for the next event.
- **Read from a process substitution (`exec {fd}< <(socat …)`), not a `coproc`.** Bash closes a coprocess's fds as soon as it exits, which can drop the last lines.
- **Run each scan under `setsid`, and stop it with `kill -- -PGID`.** Otherwise SIGTERM kills only the `bash -c` and orphans NAPS2.
- **Presses made during a scan are ignored,** not queued. An event whose `ts` is earlier than the end of the last scan is dropped; the timestamps compare as strings, because both are UTC RFC 3339 with milliseconds.
- **A failed reload keeps the old settings.** The script takes a `declare -p` snapshot and replays it with `eval`, after rewriting `declare` to `declare -g`. Without `-g`, replaying inside a function would create locals and leave the rejected values in place. This was a real bug.
- **It takes the bridge's single client slot.** A manual `socat` and the trigger will keep knocking each other off.

### Project rules

- **Language and dependencies:** C++17 and CMake ≥ 3.13, with no third-party dependencies. The only libraries used are libc, libstdc++ and pthreads.
- **Build warning-free with `-Wall -Wextra`.** The vendored headers are added as a `SYSTEM` include directory, so their warnings are suppressed there instead of by editing them.
- **License:** LGPL-2.1, because the vendored Epson code is LGPL-2.1.

## Vendored code (`vendor/epson/`)

- `ipc_header.hpp`, `shared_memory.hpp` and `semaphore.hpp` are **verbatim copies. Do not edit them.** Work around their problems in our own code.
- `ipc_interrupt.hpp` is **modified**:
  - it uses `std::function` callbacks instead of `IInterfaceDelegate`;
  - the logging calls are removed;
  - there is no button queue;
  - it waits with no timeout, and `stop()` wakes it with a signal;
  - `EINTR` is retried.

  If you change it further, record the change in `vendor/epson/README.md`.

## Behavior seen on real hardware

Tested on 2026-09-25 against a scanner at 192.168.1.122:

- The scanner reports interrupt and extended-transfer support.
- **The scan button sends `request_start_scanning` (type 3), not `button_press`.** A second press that follows quickly can be dropped by the scanner; this is not a bridge bug.
- Powering off the scanner makes es2netif report `disconnect` (type 101) about 60 s later. After that it ignores SIGHUP, and the daemon's SIGKILL fallback handles it.
- After the scanner powers back on, the port probe fails with "connection refused" until it has finished booting. The next retry then connects.
- **A connection made while the scanner is still booting may not deliver the first press.** In one case the bridge connected about 5 s after "connection refused". The first scan-button press lit the scanner's error light, and es2netif never touched the semaphore: the sempid stayed the bridge's, and both processes were attached to the segment. The light cleared by itself, and the next press came through normally. This is scanner or es2netif behavior, not the bridge. If it becomes a problem, add a settle delay after the probe first succeeds.
- The no-timeout semaphore wait was checked on real hardware (2026-09-25): the scan button was delivered, and shutdown took about 350 ms, most of it es2netif exiting.
- The one intermittent test failure we saw (a lost final `disconnect` from the fake) matched the semaphore race described above. It led to the no-timeout wait. With the timed wait gone, shutdown takes about 60 ms, down from up to 1 s.

## Testing

- **Build:**
  ```sh
  cmake -S . -B build && cmake --build build
  ```
  The build must produce no warnings. Build out of tree, in `build/`.
- **Check the config:**
  ```sh
  build/es-bridge -t -c <file>
  ```
- **Automated tests (run these after every change):**
  ```sh
  ctest --test-dir build --output-on-failure    # or: tests/run_tests.sh build
  tests/run_tests.sh build sighup single_client # run selected tests (function name without test_)
  ```
  `tests/run_tests.sh` runs the bridge against the fake `es2netif` described below and takes about 45 s. It covers:
  - config validation;
  - every scanner event type the fake sends;
  - each of the fake's failure modes, and reconnecting afterwards;
  - `prevent_timeout = false` and `emit_status_events = false`;
  - the semaphore race: `RACE_CYCLES` (default 5) connect/disconnect cycles with a whole-second pause;
  - SIGHUP reload: a new value applied, a broken config rejected, and a changed `socket_path` rebound;
  - a second client replacing the first;
  - stale-segment cleanup, with `cleanup_stale` on and off;
  - an unwritable `/tmp/test.txt`: a clear error, and es2netif is not started (the root-only repair path can't be tested unprivileged);
  - no leftover process, socket or `interrupt.dat` after shutdown.

  `tests/test_scan_trigger.sh` (CTest `scan_trigger`) tests `es-scan-trigger` against a socat mock server. It needs no bridge or `/tmp/epsonWork`, so it runs even while the real service is active. It covers:
  - the exact NAPS2 argument list built from the config, including extra args with spaces, and omitting empty settings;
  - invalid NAPS2 and `AIRSCAN_*` settings rejected at startup;
  - `AIRSCAN_URL` producing the private SANE config and `SANE_CONFIG_DIR`, and removing it on reload going back to the system config;
  - "no pages" (exit 0 without a file) and a failing NAPS2, both logged without stopping the service;
  - non-trigger events ignored, and the `SCANNER` filter;
  - JSON parsing: reordered, spaced-out events with extra fields still trigger, and malformed or non-object lines are logged and skipped;
  - a `TRIGGER_EVENTS` list, and `-2` name suffixes;
  - presses during a scan ignored, while later presses still scan;
  - reconnecting after the socket disappears;
  - SIGHUP reload: new settings applied, and a broken config rejected with the old values really kept;
  - SIGTERM stopping a running scan's whole process group (the stub records its PID, and the test checks that process group is empty).

  It needs bash, socat and jq, and uses a stub `naps2` that records its arguments. Don't check for processes with `pgrep -f <path>`: the checker's own command line matches.

  **Requirements:** bash, **socat** (the socket client), pgrep and ipcs. There is no Python. The script uses `fake_netif --make-stale-shm` to create the colliding shared-memory segment. It refuses to run if a real bridge or `es2netif` is running. Failed runs keep their logs in `/tmp/esb-test.*`. **When you add or change behavior, add a matching test to the script.**
- **Without hardware:** use `tests/fake_netif.cpp`, which builds as `build/fake_netif` by default and can be turned off with `-DESB_BUILD_TESTS=OFF`. It plays the part of `es2netif`:
  - prints a port and accepts the TCP connection;
  - answers the open and status requests;
  - attaches to the shared memory and semaphore;
  - fires `button_press`, `request_start_scanning`, `ask_is_should_prevent_timeout` and `reserved_by_host`, using the `semop(-1)` then `semtimedop(wait 0, +1)` sequence.

  `FAKE_MODE` then picks the ending:

  | `FAKE_MODE` | What happens next |
  |---|---|
  | `disconnect` (default) | Reports `event_did_disconnect` |
  | `exit` | Crashes |
  | `eof` | Closes the TCP socket |
  | `hold` | Waits |

  To use it, set `netif_path` to `build/fake_netif` and `probe_port = 0`, and export `FAKE_MODE` before starting the bridge. The child inherits it. Besides the endings above, it has also been used to check:
  - stale-segment cleanup;
  - a second client replacing the first;
  - SIGHUP with a changed config and with a broken one.

  `FAKE_PAUSE_US` sets the fake's pause before its first event and before its final action (default 1500000). Only the race test uses whole seconds.

  If the protocol handling changes, keep the fake in step with it.
- **With hardware:** run with `log_level = debug`, watch the socket with `socat - UNIX-CONNECT:<socket_path>`, and check `ipcs -m -s` and `pgrep es2netif`.
  - After SIGTERM, there must be no leftover es2netif process, shared memory, semaphore, `interrupt.dat` or socket file.
  - `/run/es-bridge/` is owned by root, so for testing as your own user, point `socket_path` somewhere writable.

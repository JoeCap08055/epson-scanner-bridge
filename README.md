# es-bridge

A small daemon that publishes events from an Epson **network** scanner (panel button presses, session events) as newline-delimited JSON on a Unix domain socket.

It starts Epson's proprietary helper, `es2netif`, the same way `epsonscan2` does. It opens the scanner, then listens on the helper's shared-memory/semaphore interrupt channel. Nothing is scanned: the daemon only relays events.

## Build

```sh
cmake -S . -B build
cmake --build build
sudo cmake --install build        # installs to /usr/local/sbin by default
```

Requirements: a C++17 compiler, CMake ≥ 3.13, Linux, and the `epsonscan2` package, which provides `es2netif`. There are no other dependencies.

## Run

```sh
cp es-bridge.conf.example /etc/es-bridge.conf    # set scanner_address
es-bridge -t -c /etc/es-bridge.conf               # validate only
es-bridge -c /etc/es-bridge.conf
socat - UNIX-CONNECT:/run/es-bridge/events.sock   # watch events
```

## Install as a systemd service

`systemd/es-bridge.service` runs the daemon at boot. The unit:
- starts `/usr/local/sbin/es-bridge -c /etc/es-bridge.conf` once the network is up;
- creates `/run/es-bridge/` for the socket;
- restarts the daemon if it exits with an error;
- maps `systemctl reload` to SIGHUP.

1. **Build and install the binary** to `/usr/local/sbin/es-bridge`:

   ```sh
   cmake -S . -B build
   cmake --build build
   sudo cmake --install build
   ```

   If you install somewhere else (for example with `-DCMAKE_INSTALL_PREFIX=/usr`), change the `ExecStart=` path in the unit to match.

2. **Create the config** and set `scanner_address`:

   ```sh
   sudo install -m 0644 es-bridge.conf.example /etc/es-bridge.conf
   sudoedit /etc/es-bridge.conf
   sudo /usr/local/sbin/es-bridge -t -c /etc/es-bridge.conf   # must print "OK"
   ```

   The unit runs as root, so the socket is owned by `root:root` with mode `0660`, and only root can read events. To let other users read them, create a group, add those users to it, and set `socket_group` in the config:

   ```sh
   sudo groupadd --system scanner
   sudo usermod -aG scanner "$USER"          # log out and back in afterwards
   # then in /etc/es-bridge.conf:  socket_group = scanner
   ```

3. **Install and start the unit:**

   ```sh
   sudo install -m 0644 systemd/es-bridge.service /etc/systemd/system/es-bridge.service
   sudo systemctl daemon-reload
   sudo systemctl enable --now es-bridge
   ```

4. **Check that it's working:**

   ```sh
   systemctl status es-bridge
   journalctl -u es-bridge -f                        # log; set log_level = debug for more detail
   socat - UNIX-CONNECT:/run/es-bridge/events.sock   # press the scanner's scan button
   ```

   Log lines carry journald priorities, so `journalctl -u es-bridge -p warning` shows only problems.

Day-to-day management:

| Task | Command |
|------|---------|
| Apply config changes without a restart | `sudo systemctl reload es-bridge` (sends SIGHUP; an invalid config is rejected and the old one kept) |
| Restart | `sudo systemctl restart es-bridge` |
| Stop and disable | `sudo systemctl disable --now es-bridge` |
| Uninstall | disable it, then `sudo rm /etc/systemd/system/es-bridge.service /usr/local/sbin/es-bridge && sudo systemctl daemon-reload` |

Notes:
- **Run as a regular user (optional).** Use `sudo systemctl edit es-bridge` to add a `User=` and `Group=` under `[Service]`. `/run/es-bridge/` is then created for that user.
  - Running as root creates `/tmp/epsonWork/` owned by root, and other users can't write to it. That breaks a later non-root run of es-bridge or `epsonscan2`.
  - If you switch after running as root, stop the service and run `sudo rm -rf /tmp/epsonWork` once.
- **Don't enable `PrivateTmp`.** `es2netif` insists on `/tmp/epsonWork`, which has to be the real `/tmp`.
- **Don't run the service alongside a manual `es-bridge` or an `epsonscan2` scan.** Only one of them can use the scanner's IPC channel at a time (see [Caveats](#caveats)).
- **A config that fails to load at startup never lets the service come up.** The daemon exits with an error, and systemd restarts it every 5 s, indefinitely, logging the error each time. Always check the config with `-t` first.

Signals:

| Signal | Effect |
|--------|--------|
| `SIGHUP` | Re-reads the config. If the new file is invalid, the daemon logs the error and keeps the current config. If it is valid, the scanner session is torn down and re-opened with the new settings. The Unix socket is rebound only when `socket_*` settings changed, so a connected client stays connected. |
| `SIGTERM` / `SIGINT` | Clean shutdown: sends the close request, terminates es2netif, removes the shm/semaphore, `interrupt.dat` and the socket. |

## Output

The socket serves **one client at a time**; a new connection replaces the old one. Anything the client sends is ignored. Each line is one JSON object:

```json
{"ts":"2026-09-24T20:42:15.548Z","event":"button_press","scanner":"192.168.1.50","button":3}
```

Every event has `ts` (UTC, RFC 3339), `event` and `scanner`. Some events add fields.

Scanner events, relayed from es2netif:

| `event` | Extra fields | Notes |
|---------|--------------|-------|
| `button_press` | `button` (int) | Panel button number |
| `request_start_scanning`, `request_stop_scanning`, `request_start_or_stop`, `request_stop` | | Scanner-initiated requests |
| `reserved_by_host` | `address` | Another host holds the scanner |
| `prevent_timeout_query` | `answer` (bool) | es2netif asked whether to keep the session alive, and the daemon replied `prevent_timeout` |
| `timeout` | | Session timed out, so the daemon reconnects |
| `disconnect` | | Scanner went away, so the daemon reconnects |
| `server_error` | | es2netif server error, so the daemon reconnects |
| `device_communication_error` | `code` | ESErrorCode, so the daemon reconnects |
| `unknown` | `type`, `data_hex` | Unrecognised event type |

Daemon status events. Turn them off with `emit_status_events = false`.

| `event` | Extra fields |
|---------|--------------|
| `connected` | `interrupt_supported`, `extended_transfer_supported` |
| `disconnected` | `reason` |
| `reconnecting` | `attempt`, `next_retry_s` |
| `config_reloaded` | |

## Reconnection

The session is torn down and the daemon goes into reconnect mode when any of these happens:
- es2netif reports a disconnect, timeout or error;
- es2netif closes its TCP connection;
- the es2netif process exits;
- an open attempt fails.

Retries back off from `reconnect_min_s`, doubling up to `reconnect_max_s`. Before each attempt, a TCP probe to `scanner_address:probe_port` (default 1865) avoids starting es2netif while the scanner is offline.

## Caveats

- **Only one instance per host.** es2netif hard-codes `/tmp/epsonWork/interrupt.dat` as the key file for its shared memory. So this daemon can't run twice, and it can't run next to an `epsonscan2` scan session on the same machine. With `cleanup_stale = true` (the default), it kills stray `es2netif` processes before connecting, as Epson's own software does.
- **Session ownership.** While connected, this host holds an open session with the scanner, and `prevent_timeout = true` keeps it open. Scanning from other hosts or apps may be blocked until the daemon disconnects. Set `prevent_timeout = false` if that's a problem.
- `/tmp/epsonWork/` must be writable by the user the daemon runs as. Don't use systemd `PrivateTmp`.
- Only network scanners are supported: this uses `es2netif` with a `//<address>` UDI. USB devices use a different path in epsonscan2.

## Layout

- `vendor/epson/`: IPC structures and helpers vendored from epsonscan2 6.7.92.0-1 (LGPL-2.1). See `vendor/epson/README.md` for what changed.
- `src/netif_session.*`: port of epsonscan2's `IPCInterfaceImpl` (launch, connect, open, status, close).
- `src/event_server.*`: Unix socket, single client.
- `src/main.cpp`: poll loop, signal handling (signalfd), reconnect state machine.
- `tests/fake_netif.cpp`: fake `es2netif` for testing without a scanner (see AGENTS.md).
- `tests/run_tests.sh`: end-to-end tests against the fake. Run them with `ctest --test-dir build --output-on-failure`. They need `socat`.

## License

LGPL-2.1, the same as the vendored Epson code. See `LICENSE`.

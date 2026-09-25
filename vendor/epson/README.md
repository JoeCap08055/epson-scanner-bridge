# Vendored Epson IPC code

These files come from **epsonscan2 6.7.92.0-1**. Epson distributes that code under the
GNU Lesser General Public License v2.1, and a copy of the license is in `../../LICENSE`.

| File | Upstream path | Status |
|------|---------------|--------|
| `ipc_header.hpp`    | `src/ES2Command/Src/Interface/ipc/ipc_header.hpp` | verbatim |
| `shared_memory.hpp` | `src/ES2Command/Src/Utils/shared_memory.hpp`      | verbatim |
| `semaphore.hpp`     | `src/ES2Command/Src/Utils/semaphore.hpp`          | verbatim |
| `ipc_interrupt.hpp` | `src/ES2Command/Src/Interface/ipc/ipc_interrupt.hpp` | modified |

## Changes in `ipc_interrupt.hpp`

- **Callbacks:** `IInterfaceDelegate` is gone. Two `std::function` callbacks replace it: one for events and one for the prevent-timeout question. A third, optional callback reports semaphore errors.
- **Logging:** the `ES_*` logging macros are removed.
- **No button queue:** upstream queued button events until the ESCI layer polled `ReceiveInterruptEvent()`. Here every event goes straight to the callback.
- **Wait with no timeout:** the event loop calls `semop()` directly, with the same two operations as `semaphore::wait_and_lock()` but no timeout. `stop()` interrupts the wait by sending the thread `SIGUSR1` (`wake_signal()`) with `pthread_kill`, repeating until the thread reports it has exited. The handler does nothing and has no `SA_RESTART`.
  - **Why:** upstream waited in 1 s timed steps. es2netif releases the semaphore and immediately re-acquires it, so a release that lands between two timed waits can be taken back by es2netif and the event is lost. See the comment on `event_loop_()`.
  - **Consequences:** `stop()` no longer waits out a timeout, so it returns in milliseconds. The constructor no longer takes `interval_msec`. `EINTR` is retried.
- **Unchanged:** the semaphore handshake itself, including when `_recv_result` gets written for `ask_is_should_prevent_timeout`.

The protocol logic in `src/netif_session.cpp` is a port of
`src/ES2Command/Src/Interface/ipc/ipcInterfaceImpl.cpp` from the same release.

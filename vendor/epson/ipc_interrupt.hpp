// Adapted from epsonscan2 6.7.92.0-1:
//   src/ES2Command/Src/Interface/ipc/ipc_interrupt.hpp
// Licensed under the GNU LGPL v2.1 (see LICENSE at the project root).
//
// Modifications (see vendor/epson/README.md):
//   - IInterfaceDelegate replaced by two std::function callbacks.
//   - ES_* logging macros removed.
//   - Button events are no longer queued for later polling; every event is
//     handed to the callback as soon as it arrives.
//   - Per-event dispatch moved out to the caller (main loop), except for
//     ask_is_should_prevent_timeout, which must be answered in-place before
//     the semaphore is released.
//   - The semaphore wait has no timeout; stop() interrupts it with a signal.
//     See the comment on event_loop_() for why.
#pragma once

#include <atomic>
#include <cerrno>
#include <csignal>
#include <functional>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <time.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#include "ipc_header.hpp"
#include "shared_memory.hpp"
#include "semaphore.hpp"

namespace ipc {

class ipc_interrupt
{
public:
  typedef shared_memory<struct ipc_interrupt_event_data> ipc_shared_memory;
  typedef std::function<void(const ipc_interrupt_event_data&)> event_callback;
  typedef std::function<bool()> prevent_timeout_callback;
  typedef std::function<void(int)> error_callback;

  // stop() sends this signal to the listener thread to break it out of
  // semop(). The thread unblocks it for itself. Other threads should keep it
  // blocked so that a stray kill(2) to the process lands nowhere harmful.
  static int wake_signal() { return SIGUSR1; }

public:
  ipc_interrupt(event_callback on_event,
                prevent_timeout_callback should_prevent_timeout,
                error_callback on_error,
                const std::string shared_memory_file, int shared_memory_id,
                int semaphore_key)
  : on_event_(std::move(on_event))
  , should_prevent_timeout_(std::move(should_prevent_timeout))
  , on_error_(std::move(on_error))
  , is_exit_(false)
  , is_finished_(false)
  , shared_memory_(new ipc_shared_memory(shared_memory_file.c_str(), shared_memory_id, true))
  , semaphore_(new semaphore(semaphore_key, true, true))
  {
    if (!shared_memory_ || !semaphore_){
      shared_memory_ = nullptr;
      semaphore_ = nullptr;

      throw std::runtime_error("ipc_interrupt initialize failed");
    }
  }
  virtual ~ipc_interrupt()
  {
    stop();
  }

  void start()
  {
    install_wake_handler_();
    is_exit_ = false;
    is_finished_ = false;
    task_ = std::thread([this] { event_loop_(); });
  }

  void stop()
  {
    try{
      if (task_.joinable()){
        is_exit_ = true;
        // A signal sent just before the thread enters semop() would be lost,
        // so keep sending until the thread reports that it has left the loop.
        while (!is_finished_){
          pthread_kill(task_.native_handle(), wake_signal());
          struct timespec t = {0, 5 * 1000 * 1000};
          nanosleep(&t, nullptr);
        }
        task_.join();
      }
      shared_memory_ = nullptr;
      semaphore_ = nullptr;
    }catch(...){
    }
  }

  key_t sem_key() const { return (semaphore_ ? semaphore_->key() : -1);}

protected:
  static void wake_handler_(int) {}

  static void install_wake_handler_()
  {
    // No SA_RESTART, so semop() fails with EINTR instead of resuming.
    struct sigaction sa = {};
    sa.sa_handler = wake_handler_;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(wake_signal(), &sa, nullptr);
  }

  // Upstream waited with semaphore::wait_and_lock(timeout) in a loop so it
  // could notice is_exit_. es2netif signals an event with semop(-1) and then
  // at once re-acquires with semtimedop(wait for 0, +1), as confirmed by
  // disassembly. When our thread is already blocked, the kernel completes our
  // pending (wait for 0, +1) inside es2netif's semop(-1), before es2netif can
  // re-acquire. But if the release lands between one timed wait expiring and
  // the next starting, es2netif takes the semaphore back and the event is
  // lost. Waiting with no timeout means the thread is always queued in
  // semop(), except while it is handling an event, and es2netif can't send
  // another event until we unlock.
  void event_loop_()
  {
    sigset_t wake;
    sigemptyset(&wake);
    sigaddset(&wake, wake_signal());
    pthread_sigmask(SIG_UNBLOCK, &wake, nullptr);

    const int sem_id = semaphore_ ? semaphore_->sem_id() : -1;
    while(!is_exit_ && sem_id >= 0 && shared_memory_)
    {
      // Same operations as semaphore::wait_and_lock().
      sembuf operations[2];
      operations[0].sem_num = 0;
      operations[0].sem_op = WAIT;
      operations[0].sem_flg = SEM_UNDO;
      operations[1].sem_num = 0;
      operations[1].sem_op = LOCK;
      operations[1].sem_flg = SEM_UNDO;

      if (semop(sem_id, operations, 2) == -1){
        int err = errno;
        if (err == EINTR){
          continue;              // woken by stop(), or a stray signal
        } else if (err == EIDRM){
          break;
        } else {
          if (on_error_) on_error_(err);
          break;
        }
      }

      if (!is_exit_ && shared_memory_)
      {
        DealInterruptEvent(shared_memory_->data());
      }
      // Always release, even when stopping, so es2netif isn't left waiting.
      semaphore_->unlock();
    }
    is_finished_ = true;
  }

  void DealInterruptEvent(struct ipc_interrupt_event_data &event_data)
  {
    // es2netif reads _recv_result after we release the semaphore, so the
    // answer has to be written while we still hold it.
    if (event_data._type == ask_is_should_prevent_timeout){
      bool ret = should_prevent_timeout_ ? should_prevent_timeout_() : false;
      event_data._recv_result = static_cast<uint32_t>(ret);
    }
    if (on_event_){
      on_event_(event_data);
    }
  }

private:
  event_callback on_event_;
  prevent_timeout_callback should_prevent_timeout_;
  error_callback on_error_;
  std::atomic_bool is_exit_;
  std::atomic_bool is_finished_;

  std::shared_ptr<ipc_shared_memory> shared_memory_;
  std::shared_ptr<semaphore> semaphore_;

  std::thread task_;
};

}; // namespace ipc

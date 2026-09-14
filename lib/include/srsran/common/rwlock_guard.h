/**
 * Copyright 2013-2023 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#ifndef SRSRAN_RWLOCK_GUARD_H
#define SRSRAN_RWLOCK_GUARD_H

#include <pthread.h>

namespace srsran {

class rwlock_write_guard
{
public:
  rwlock_write_guard(pthread_rwlock_t& rwlock_) : rwlock(&rwlock_) { pthread_rwlock_wrlock(rwlock); }
  rwlock_write_guard(const rwlock_write_guard&) = delete;
  rwlock_write_guard(rwlock_write_guard&&)      = delete;
  rwlock_write_guard& operator=(const rwlock_write_guard&) = delete;
  rwlock_write_guard& operator=(rwlock_write_guard&&) = delete;
  ~rwlock_write_guard() { pthread_rwlock_unlock(rwlock); }

private:
  pthread_rwlock_t* rwlock;
};

// Shared lock guard that automatically unlocks rwlock on exit.
//
// Re-entrant for reads: if this thread already holds `rwlock` for reading, a
// nested guard only bumps a per-thread depth counter and does not call
// pthread_rwlock_rdlock() again. The code base takes nested read locks in a
// few places, e.g. mac::get_dl_sched() -> RLC -> mac::rlc_buffer_state().
// glibc lets a reader re-lock even when a writer is queued; macOS blocks it
// ("writers are favored over readers", pthread_rwlock_rdlock(3)), so the
// nested read deadlocks against e.g. mac::allocate_ue() on the stack thread.
// Read-then-write on the same thread is still a deadlock, as before.
class rwlock_read_guard
{
public:
  rwlock_read_guard(pthread_rwlock_t& rwlock_) : rwlock(&rwlock_)
  {
    held_read_locks& held = thread_held();
    idx                   = held.find(rwlock);
    if (idx >= 0) {
      held.depth[idx]++;
      return;
    }
    pthread_rwlock_rdlock(rwlock);
    idx = held.free_slot();
    if (idx >= 0) {
      held.lock[idx]  = rwlock;
      held.depth[idx] = 1;
    }
    // idx < 0: table full, this guard is a plain non-reentrant read lock.
  }
  rwlock_read_guard(const rwlock_read_guard&) = delete;
  rwlock_read_guard(rwlock_read_guard&&)      = delete;
  rwlock_read_guard& operator=(const rwlock_read_guard&) = delete;
  rwlock_read_guard& operator=(rwlock_read_guard&&) = delete;
  ~rwlock_read_guard()
  {
    if (idx >= 0) {
      held_read_locks& held = thread_held();
      if (--held.depth[idx] > 0) {
        return; // an outer guard on this thread still holds the lock
      }
      held.lock[idx] = nullptr;
    }
    pthread_rwlock_unlock(rwlock);
  }

private:
  // rwlocks this thread currently holds for reading. A thread holds at most a
  // handful of distinct rwlocks at once (MAC, RLC, bearer manager).
  struct held_read_locks {
    static constexpr int capacity     = 8;
    pthread_rwlock_t*    lock[capacity]  = {};
    int                  depth[capacity] = {};

    int find(const pthread_rwlock_t* l) const
    {
      for (int i = 0; i < capacity; i++) {
        if (lock[i] == l) {
          return i;
        }
      }
      return -1;
    }
    int free_slot() const { return find(nullptr); }
  };
  static held_read_locks& thread_held()
  {
    static thread_local held_read_locks held;
    return held;
  }

  pthread_rwlock_t* rwlock;
  int               idx = -1;
};

} // namespace srsran

#endif // SRSRAN_RWLOCK_GUARD_H

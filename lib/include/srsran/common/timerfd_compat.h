/**
 * Minimal Linux timerfd shim for macOS.
 *
 * macOS has no <sys/timerfd.h>. srsRAN's periodic_thread (used only by the
 * low-rate metrics hub) creates a periodic timerfd and read()s it to pace
 * itself. This shim backs a "timerfd" with a pipe whose write end is fed by a
 * helper thread once per period, so read()/select()/close() behave like Linux.
 *
 * It is intentionally simple (millisecond-ish granularity) — adequate for the
 * metrics cadence, not intended for PHY timing.
 */

#ifndef SRSRAN_TIMERFD_COMPAT_H
#define SRSRAN_TIMERFD_COMPAT_H

#ifdef __APPLE__

#include <time.h> // struct timespec, CLOCK_MONOTONIC

// macOS lacks POSIX interval timers, so struct itimerspec is not declared.
#ifndef __itimerspec_defined
#define __itimerspec_defined 1
struct itimerspec {
  struct timespec it_interval; // timer period
  struct timespec it_value;    // initial expiration
};
#endif

#ifndef TFD_TIMER_ABSTIME
#define TFD_TIMER_ABSTIME 1
#endif
#ifndef TFD_CLOEXEC
#define TFD_CLOEXEC 0
#endif
#ifndef TFD_NONBLOCK
#define TFD_NONBLOCK 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Returns a readable fd (pipe read end). clockid/flags are accepted and ignored.
int timerfd_create(int clockid, int flags);

// Starts (or restarts) periodic expirations using new_value->it_interval.
// flags and old_value are accepted and ignored.
int timerfd_settime(int fd, int flags, const struct itimerspec* new_value, struct itimerspec* old_value);

#ifdef __cplusplus
}
#endif

#endif // __APPLE__

#endif // SRSRAN_TIMERFD_COMPAT_H

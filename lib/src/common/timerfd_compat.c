/**
 * macOS timerfd shim implementation. See timerfd_compat.h.
 *
 * A "timerfd" is the read end of a pipe. timerfd_settime() spawns a detached
 * thread that sleeps for the configured interval and then writes a uint64
 * expiration count (always 1) to the write end. The consumer's read() therefore
 * unblocks once per period, mirroring Linux timerfd_create + read() semantics.
 */

#ifdef __APPLE__

#include "srsran/common/timerfd_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SRSRAN_MAX_TIMERFD 64

struct tfd_entry {
  int       used;
  int       rfd;
  int       wfd;
  uint64_t  period_ns;
  pthread_t thread;
  int       started;
};

static struct tfd_entry g_entries[SRSRAN_MAX_TIMERFD];
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;

static void* tfd_thread(void* arg)
{
  struct tfd_entry* e = (struct tfd_entry*)arg;
  for (;;) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(e->period_ns / 1000000000ULL);
    ts.tv_nsec = (long)(e->period_ns % 1000000000ULL);
    nanosleep(&ts, NULL);

    uint64_t one = 1;
    ssize_t  n   = write(e->wfd, &one, sizeof(one));
    if (n != (ssize_t)sizeof(one)) {
      // Reader closed its end (EPIPE) or fatal error -> terminate this timer.
      break;
    }
  }
  return NULL;
}

int timerfd_create(int clockid, int flags)
{
  (void)clockid;
  (void)flags;
  int fds[2];
  if (pipe(fds) != 0) {
    return -1;
  }

  pthread_mutex_lock(&g_lock);
  int slot = -1;
  for (int i = 0; i < SRSRAN_MAX_TIMERFD; i++) {
    if (!g_entries[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    pthread_mutex_unlock(&g_lock);
    close(fds[0]);
    close(fds[1]);
    errno = EMFILE;
    return -1;
  }
  memset(&g_entries[slot], 0, sizeof(g_entries[slot]));
  g_entries[slot].used = 1;
  g_entries[slot].rfd  = fds[0];
  g_entries[slot].wfd  = fds[1];
  pthread_mutex_unlock(&g_lock);

  return fds[0];
}

int timerfd_settime(int fd, int flags, const struct itimerspec* new_value, struct itimerspec* old_value)
{
  (void)flags;
  (void)old_value;
  if (new_value == NULL) {
    errno = EINVAL;
    return -1;
  }

  pthread_mutex_lock(&g_lock);
  struct tfd_entry* e = NULL;
  for (int i = 0; i < SRSRAN_MAX_TIMERFD; i++) {
    if (g_entries[i].used && g_entries[i].rfd == fd) {
      e = &g_entries[i];
      break;
    }
  }
  if (e == NULL) {
    pthread_mutex_unlock(&g_lock);
    errno = EINVAL;
    return -1;
  }

  e->period_ns = (uint64_t)new_value->it_interval.tv_sec * 1000000000ULL + (uint64_t)new_value->it_interval.tv_nsec;
  if (e->period_ns == 0) {
    // Disarm: nothing to do for this simple shim (no one-shot use in srsRAN).
    pthread_mutex_unlock(&g_lock);
    return 0;
  }

  if (!e->started) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc      = pthread_create(&e->thread, &attr, tfd_thread, e);
    e->started  = (rc == 0);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
      pthread_mutex_unlock(&g_lock);
      errno = rc;
      return -1;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

#endif // __APPLE__

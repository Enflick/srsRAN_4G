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

/**
 * SCTP portability header.
 *
 * Include this instead of <netinet/sctp.h>. It exposes the subset of the Linux
 * lksctp API that srsRAN uses, plus a set of srsran_sctp_* socket wrappers, on
 * top of whichever SCTP implementation CMake found:
 *
 *  - Kernel SCTP (Linux): <netinet/sctp.h> is included directly and the
 *    wrappers are inline forwarders to the corresponding syscalls.
 *
 *  - libusrsctp (HAVE_USRSCTP, e.g. macOS, which has no in-kernel SCTP): the
 *    wrappers are implemented in sctp_compat.cc against the userland stack.
 *    Native SCTP over raw IP is used, NOT SCTP-over-UDP encapsulation, so the
 *    packets on the wire are ordinary IPPROTO_SCTP and interoperate with a
 *    normal MME. Raw sockets mean the process must run as root.
 *
 *    The userland stack works with `struct socket *` handles, so the wrappers
 *    hand out integer "pseudo-fds" (the read end of an internal pipe) instead,
 *    keeping srsRAN's select()-based socket_manager and its int-fd plumbing
 *    working unchanged. See sctp_compat.cc for the bridge details.
 *
 * The wrappers accept any fd: those that are not SCTP pseudo-fds fall through
 * to the matching syscall, so TCP/UDP/pipe fds are unaffected.
 */

#ifndef SRSRAN_SCTP_COMPAT_H
#define SRSRAN_SCTP_COMPAT_H

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifndef HAVE_USRSCTP

/* ------------------------------- kernel SCTP ------------------------------ */

#include <netinet/sctp.h>
#include <unistd.h>

static inline void srsran_sctp_init(void) {}

static inline int srsran_sctp_socket(int domain, int type, int protocol)
{
  return socket(domain, type, protocol);
}
static inline int srsran_sctp_bind(int fd, const struct sockaddr* addr, socklen_t addrlen)
{
  return bind(fd, addr, addrlen);
}
static inline int srsran_sctp_connect(int fd, const struct sockaddr* addr, socklen_t addrlen)
{
  return connect(fd, addr, addrlen);
}
static inline int srsran_sctp_listen(int fd, int backlog)
{
  return listen(fd, backlog);
}
static inline int srsran_sctp_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen)
{
  return setsockopt(fd, level, optname, optval, optlen);
}
static inline int srsran_sctp_getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen)
{
  return getsockopt(fd, level, optname, optval, optlen);
}
static inline int srsran_sctp_close(int fd)
{
  return close(fd);
}

#else // HAVE_USRSCTP

/* ------------------------------ userland SCTP ----------------------------- */

// usrsctp provides union sctp_notification, struct sctp_event_subscribe,
// struct sctp_rtoinfo, struct sctp_initmsg, struct sctp_paddrparams,
// struct sctp_assocparams, the SCTP_* event constants, MSG_NOTIFICATION and
// IPPROTO_SCTP.
#include <usrsctp.h>

// Not provided by usrsctp.h but used by srsRAN as the setsockopt "level".
#ifndef SOL_SCTP
#define SOL_SCTP IPPROTO_SCTP
#endif

// Legacy one-shot event-subscribe sockopt (takes a struct sctp_event_subscribe).
// usrsctp only ships the newer per-event SCTP_EVENT; the shim intercepts this
// value and translates it into per-event subscriptions.
#ifndef SCTP_EVENTS
#define SCTP_EVENTS 0x0000000b
#endif

// Legacy combined send/recv metadata struct (usrsctp uses sndinfo/rcvinfo).
// Field names match lksctp for the members srsRAN reads.
struct sctp_sndrcvinfo {
  uint16_t     sinfo_stream;
  uint16_t     sinfo_ssn;
  uint16_t     sinfo_flags;
  uint32_t     sinfo_ppid;
  uint32_t     sinfo_context;
  uint32_t     sinfo_timetolive;
  uint32_t     sinfo_tsn;
  uint32_t     sinfo_cumtsn;
  sctp_assoc_t sinfo_assoc_id;
};

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the userland SCTP stack exactly once (idempotent). Safe to call
// from multiple threads. Uses raw IP sockets (native SCTP) -> needs root.
void srsran_sctp_init(void);

// Open a userland SCTP socket and return a pseudo-fd. SCTP only.
int srsran_sctp_socket(int domain, int type, int protocol);

int srsran_sctp_bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
int srsran_sctp_connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
int srsran_sctp_listen(int fd, int backlog);
int srsran_sctp_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen);
int srsran_sctp_getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen);
int srsran_sctp_close(int fd);

// Drop-in replacements for the libsctp convenience functions. Same names and
// signatures as lksctp so the srsRAN call sites compile unchanged.
ssize_t sctp_recvmsg(int                     fd,
                     void*                   msg,
                     size_t                  len,
                     struct sockaddr*        from,
                     socklen_t*              fromlen,
                     struct sctp_sndrcvinfo* sinfo,
                     int*                    msg_flags);

ssize_t sctp_sendmsg(int                    fd,
                     const void*            msg,
                     size_t                 len,
                     struct sockaddr*       to,
                     socklen_t              tolen,
                     uint32_t               ppid,
                     uint32_t               flags,
                     uint16_t               stream_no,
                     uint32_t               timetolive,
                     uint32_t               context);

#ifdef __cplusplus
}
#endif

#endif // HAVE_USRSCTP

#endif // SRSRAN_SCTP_COMPAT_H

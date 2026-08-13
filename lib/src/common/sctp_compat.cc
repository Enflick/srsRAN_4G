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
 * SCTP implementation over libusrsctp, for platforms without kernel SCTP.
 * See sctp_compat.h for the rationale; this file is only compiled when CMake
 * selected the userland stack.
 *
 * Bridge model
 * ------------
 * libusrsctp works with `struct socket *` handles and delivers readability via
 * an upcall on its own internal thread. srsRAN works with integer fds and a
 * select() loop. For each SCTP socket we therefore keep:
 *   - the usrsctp `struct socket *`
 *   - an OS pipe; its read end is the integer "pseudo-fd" handed to srsRAN and
 *     placed into select(). It holds at most one byte: present iff the rx queue
 *     is non-empty (level-triggered "data ready").
 *   - a queue of fully-received SCTP messages (data + translated metadata).
 *
 * The usrsctp upcall drains the association into the queue (non-blocking) and
 * flips the pipe to "readable"; srsRAN's select() then calls our sctp_recvmsg()
 * once per message until the queue empties and the pipe is cleared.
 */

#include "srsran/common/sctp_compat.h"

#ifdef HAVE_USRSCTP

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <unistd.h>
#include <vector>

// Control-path diagnostics, off unless SRSRAN_SCTP_DEBUG=1. These run on the
// usrsctp internal threads, from which srslog is not reachable.
#define SCTP_SHIM_LOG(...)                                                                                             \
  do {                                                                                                                 \
    const char* dbg = getenv("SRSRAN_SCTP_DEBUG");                                                                     \
    if (dbg != nullptr && dbg[0] == '1') {                                                                             \
      fprintf(stderr, "[sctp_compat] " __VA_ARGS__);                                                                   \
      fprintf(stderr, "\n");                                                                                           \
    }                                                                                                                  \
  } while (0)

namespace {

struct rx_msg {
  std::vector<uint8_t>   data;
  struct sockaddr_in     from  = {};
  struct sctp_sndrcvinfo sri   = {};
  int                    flags = 0;
  bool                   eof   = false; // association closed (recvv returned 0)
};

struct sctp_conn {
  struct socket*     so       = nullptr;
  int                pipe_rd  = -1;
  int                pipe_wr  = -1;
  int                socktype = SOCK_STREAM;
  bool               closing  = false; // set under mtx by close(); upcall stops touching fds
  std::mutex         mtx;
  std::deque<rx_msg> rxq;
};

std::mutex                                           g_mtx;
std::map<int, std::shared_ptr<sctp_conn> >           g_conns;   // keyed by pseudo-fd (pipe read end)
std::map<struct socket*, std::shared_ptr<sctp_conn> > g_by_sock; // keyed by usrsctp socket (for upcalls)
std::once_flag                                       g_init_flag;

std::shared_ptr<sctp_conn> find_conn(int fd)
{
  std::lock_guard<std::mutex> lock(g_mtx);
  auto                        it = g_conns.find(fd);
  return it == g_conns.end() ? nullptr : it->second;
}

std::shared_ptr<sctp_conn> find_conn_by_sock(struct socket* so)
{
  std::lock_guard<std::mutex> lock(g_mtx);
  auto                        it = g_by_sock.find(so);
  return it == g_by_sock.end() ? nullptr : it->second;
}

// Enqueue a received message and, on the empty->non-empty transition, make the
// pseudo-fd readable. Holds c->mtx across the pipe write so the single-byte
// "data ready" invariant stays consistent with sctp_recvmsg(), and so it never
// writes to a pipe that close() is tearing down.
void push_msg(sctp_conn* c, rx_msg&& m)
{
  std::lock_guard<std::mutex> lock(c->mtx);
  if (c->closing || c->pipe_wr < 0) {
    return;
  }
  bool was_empty = c->rxq.empty();
  c->rxq.push_back(std::move(m));
  if (was_empty) {
    uint8_t b = 1;
    ssize_t r = ::write(c->pipe_wr, &b, 1);
    (void)r;
  }
}

// usrsctp read upcall: drain the association into the rx queue (non-blocking).
// The connection is looked up by socket and held alive via shared_ptr for the
// duration of the upcall, so a close() racing on another thread cannot free it
// underneath us. If the socket is already gone from the registry, bail out
// before touching usrsctp (the socket may be closing).
void upcall_cb(struct socket* so, void* /*arg*/, int /*flags*/)
{
  std::shared_ptr<sctp_conn> c_sp = find_conn_by_sock(so);
  if (c_sp == nullptr) {
    return;
  }
  sctp_conn* c = c_sp.get();
  for (;;) {
    char                    buf[65536];
    struct sockaddr_storage from     = {};
    socklen_t               fromlen  = sizeof(from);
    struct sctp_rcvinfo     rcv      = {};
    socklen_t               infolen  = sizeof(rcv);
    unsigned int            infotype = 0;
    int                     msgflags = 0;

    ssize_t n = usrsctp_recvv(
        so, buf, sizeof(buf), (struct sockaddr*)&from, &fromlen, &rcv, &infolen, &infotype, &msgflags);

    if (n > 0) {
      rx_msg m;
      m.data.assign(buf, buf + n);
      if (from.ss_family == AF_INET) {
        memcpy(&m.from, &from, sizeof(struct sockaddr_in));
      }
      if (infotype == SCTP_RECVV_RCVINFO) {
        m.sri.sinfo_stream   = rcv.rcv_sid;
        m.sri.sinfo_ssn      = rcv.rcv_ssn;
        m.sri.sinfo_flags    = rcv.rcv_flags;
        m.sri.sinfo_ppid     = rcv.rcv_ppid;
        m.sri.sinfo_context  = rcv.rcv_context;
        m.sri.sinfo_tsn      = rcv.rcv_tsn;
        m.sri.sinfo_cumtsn   = rcv.rcv_cumtsn;
        m.sri.sinfo_assoc_id = rcv.rcv_assoc_id;
      }
      m.flags = msgflags;
      push_msg(c, std::move(m));
      continue;
    }
    if (n == 0) {
      // Peer closed the association -> surface as a 0-byte read.
      SCTP_SHIM_LOG("usrsctp_recvv returned 0 (EOF) -> surfacing association close to srsRAN");
      rx_msg m;
      m.eof = true;
      push_msg(c, std::move(m));
      break;
    }
    // n < 0
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break; // no more data queued
    }
    break; // other error: stop draining
  }
}

} // namespace

extern "C" {

void srsran_sctp_init(void)
{
  std::call_once(g_init_flag, []() {
    // port 0 + no conn_output callback => native SCTP over raw IP sockets.
    // Requires root. usrsctp spawns its own iterator/receive threads.
    usrsctp_init(0, nullptr, nullptr);
    usrsctp_sysctl_set_sctp_blackhole(2);
    SCTP_SHIM_LOG("usrsctp_init done (native raw-IP SCTP; needs root)");
  });
}

int srsran_sctp_socket(int domain, int type, int /*protocol*/)
{
  srsran_sctp_init();

  // Force one-to-one (SOCK_STREAM) regardless of the requested type. srsRAN asks
  // for SOCK_SEQPACKET (one-to-many), but on a one-to-many socket usrsctp_connect()
  // returns immediately instead of blocking until COMM_UP, so srsRAN's first
  // S1AP send races the association setup and fails. The eNB uses a single MME
  // association, for which one-to-one is equivalent on the wire and gives the
  // blocking-connect semantics srsRAN expects.
  const int usrsctp_type = SOCK_STREAM;
  struct socket* so = usrsctp_socket(domain, usrsctp_type, IPPROTO_SCTP, nullptr, nullptr, 0, nullptr);
  if (so == nullptr) {
    SCTP_SHIM_LOG("usrsctp_socket(domain=%d,type=%d->STREAM) FAILED: %s", domain, type, strerror(errno));
    return -1;
  }
  SCTP_SHIM_LOG("usrsctp_socket ok (domain=%d, requested type=%d, using SOCK_STREAM one-to-one)", domain, type);

  // Allow the fixed S1AP source port to be re-bound immediately on reconnect.
  int reuse = 1;
  usrsctp_setsockopt(so, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  int fds[2];
  if (::pipe(fds) != 0) {
    usrsctp_close(so);
    return -1;
  }

  auto c      = std::make_shared<sctp_conn>();
  c->so       = so;
  c->pipe_rd  = fds[0];
  c->pipe_wr  = fds[1];
  c->socktype = type;

  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_conns[c->pipe_rd] = c;
    g_by_sock[so]       = c;
  }
  return c->pipe_rd; // pseudo-fd
}

int srsran_sctp_bind(int fd, const struct sockaddr* addr, socklen_t addrlen)
{
  auto c = find_conn(fd);
  if (c == nullptr) {
    return ::bind(fd, addr, addrlen);
  }
  int ret = usrsctp_bind(c->so, const_cast<struct sockaddr*>(addr), addrlen);
  if (addr->sa_family == AF_INET) {
    const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
    char                      ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
    SCTP_SHIM_LOG("usrsctp_bind %s:%d -> ret=%d %s", ip, ntohs(in->sin_port), ret, ret ? strerror(errno) : "ok");
  }
  return ret;
}

int srsran_sctp_connect(int fd, const struct sockaddr* addr, socklen_t addrlen)
{
  auto c = find_conn(fd);
  if (c == nullptr) {
    return ::connect(fd, addr, addrlen);
  }

  char ip[INET_ADDRSTRLEN] = {};
  int  port                = 0;
  if (addr->sa_family == AF_INET) {
    const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
    inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
    port = ntohs(in->sin_port);
  }
  SCTP_SHIM_LOG("usrsctp_connect -> %s:%d (non-blocking; sending INIT via raw IP)...", ip, port);

  // Non-blocking connect + bounded poll for establishment. A plain blocking
  // usrsctp_connect() would sit on srsRAN's background worker for the full
  // SCTP_INITMSG timeout (~15s) if the peer is slow, which is longer than
  // srsRAN's 5s shutdown watchdog and hangs exit. Bounding the wait below that
  // window keeps teardown responsive; a normal connect completes in ~30ms.
  usrsctp_set_non_blocking(c->so, 1);
  int ret = usrsctp_connect(c->so, const_cast<struct sockaddr*>(addr), addrlen);
  int e   = errno;
  if (ret != 0 && e != EINPROGRESS && e != EISCONN) {
    SCTP_SHIM_LOG("usrsctp_connect to %s:%d FAILED: ret=%d errno=%d (%s)", ip, port, ret, e, strerror(e));
    return -1;
  }

  const int max_wait_ms = 3000; // stay under the 5s SIGALRM shutdown watchdog
  const int poll_us     = 20000;
  bool      established = false;
  for (int waited_ms = 0; waited_ms < max_wait_ms; waited_ms += poll_us / 1000) {
    struct sctp_status st    = {};
    socklen_t          stlen = sizeof(st);
    if (usrsctp_getsockopt(c->so, IPPROTO_SCTP, SCTP_STATUS, &st, &stlen) == 0 &&
        st.sstat_state == SCTP_ESTABLISHED) {
      established = true;
      break;
    }
    usleep(poll_us);
  }
  if (!established) {
    SCTP_SHIM_LOG("usrsctp_connect to %s:%d not established within %dms", ip, port, max_wait_ms);
    return -1;
  }
  SCTP_SHIM_LOG("usrsctp_connect to %s:%d SUCCEEDED (assoc up)", ip, port);

  // Start delivering data through the upcall bridge (socket already non-blocking).
  usrsctp_set_upcall(c->so, upcall_cb, c.get());
  // Drain anything already buffered before the upcall was registered.
  upcall_cb(c->so, c.get(), 0);
  return 0;
}

int srsran_sctp_listen(int fd, int backlog)
{
  auto c = find_conn(fd);
  if (c == nullptr) {
    return ::listen(fd, backlog);
  }
  return usrsctp_listen(c->so, backlog);
}

int srsran_sctp_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen)
{
  auto c = find_conn(fd);
  if (c == nullptr) {
    return ::setsockopt(fd, level, optname, optval, optlen);
  }

  // Translate the legacy SCTP_EVENTS subscription into the per-event API plus
  // rcvinfo delivery (so recvv returns stream/ppid/assoc metadata).
  if (level == SOL_SCTP && optname == SCTP_EVENTS) {
    const struct sctp_event_subscribe* ev = static_cast<const struct sctp_event_subscribe*>(optval);

    int on = 1;
    usrsctp_setsockopt(c->so, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on, sizeof(on));

    auto subscribe = [&](uint16_t type, uint8_t enabled) {
      struct sctp_event se = {};
      se.se_assoc_id       = SCTP_ALL_ASSOC;
      se.se_type           = type;
      se.se_on             = enabled ? 1 : 0;
      usrsctp_setsockopt(c->so, IPPROTO_SCTP, SCTP_EVENT, &se, sizeof(se));
    };
    subscribe(SCTP_ASSOC_CHANGE, ev->sctp_association_event);
    subscribe(SCTP_PEER_ADDR_CHANGE, ev->sctp_address_event);
    subscribe(SCTP_SHUTDOWN_EVENT, ev->sctp_shutdown_event);
    subscribe(SCTP_REMOTE_ERROR, ev->sctp_peer_error_event);
    return 0;
  }

  return usrsctp_setsockopt(c->so, level, optname, const_cast<void*>(optval), optlen);
}

int srsran_sctp_getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen)
{
  auto c = find_conn(fd);
  if (c == nullptr) {
    return ::getsockopt(fd, level, optname, optval, optlen);
  }

  // SO_TYPE on the pseudo-fd would query the pipe; answer from our record.
  if (level == SOL_SOCKET && optname == SO_TYPE) {
    if (optlen == nullptr || *optlen < (socklen_t)sizeof(int)) {
      errno = EINVAL;
      return -1;
    }
    *static_cast<int*>(optval) = c->socktype;
    *optlen                    = sizeof(int);
    return 0;
  }

  return usrsctp_getsockopt(c->so, level, optname, optval, optlen);
}

int srsran_sctp_close(int fd)
{
  std::shared_ptr<sctp_conn> c;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    auto                        it = g_conns.find(fd);
    if (it == g_conns.end()) {
      return ::close(fd);
    }
    c = it->second;
    g_conns.erase(it);
    g_by_sock.erase(c->so); // new upcalls for this socket now no-op (find_conn_by_sock fails)
  }

  // Mark closing under the conn lock so any upcall already past the registry
  // lookup stops writing to the pipe we are about to close.
  {
    std::lock_guard<std::mutex> lock(c->mtx);
    c->closing = true;
  }

  // Stop usrsctp from invoking new upcalls for this socket.
  usrsctp_set_upcall(c->so, nullptr, nullptr);

  // Abort (don't gracefully shut down) the association on close so the local
  // SCTP port is released immediately. A graceful shutdown lingers the port,
  // which makes srsRAN's fast S1 reconnect fail with EADDRINUSE.
  struct linger lo = {};
  lo.l_onoff       = 1;
  lo.l_linger      = 0;
  usrsctp_setsockopt(c->so, SOL_SOCKET, SO_LINGER, &lo, sizeof(lo));

  // usrsctp_close serializes with any in-flight upcall via the socket lock, so
  // once it returns no upcall is running or will run for this socket.
  usrsctp_close(c->so);

  {
    std::lock_guard<std::mutex> lock(c->mtx);
    ::close(c->pipe_rd);
    ::close(c->pipe_wr);
    c->pipe_rd = c->pipe_wr = -1;
  }
  return 0;
}

ssize_t sctp_recvmsg(int                     fd,
                     void*                   msg,
                     size_t                  len,
                     struct sockaddr*        from,
                     socklen_t*              fromlen,
                     struct sctp_sndrcvinfo* sinfo,
                     int*                    msg_flags)
{
  auto c = find_conn(fd);
  if (c == nullptr) {
    errno = EBADF;
    return -1;
  }

  std::lock_guard<std::mutex> lock(c->mtx);
  if (c->closing || c->pipe_rd < 0) {
    errno = EBADF;
    return -1;
  }
  if (c->rxq.empty()) {
    errno = EAGAIN;
    return -1;
  }

  rx_msg m = std::move(c->rxq.front());
  c->rxq.pop_front();
  if (c->rxq.empty()) {
    uint8_t b;
    ssize_t r = ::read(c->pipe_rd, &b, 1); // clear "data ready"
    (void)r;
  }

  if (sinfo != nullptr) {
    *sinfo = m.sri;
  }
  if (msg_flags != nullptr) {
    *msg_flags = m.flags;
  }
  if (m.eof) {
    return 0;
  }

  size_t n = std::min(len, m.data.size());
  memcpy(msg, m.data.data(), n);
  if (from != nullptr && fromlen != nullptr) {
    socklen_t cp = std::min(*fromlen, (socklen_t)sizeof(struct sockaddr_in));
    memcpy(from, &m.from, cp);
    *fromlen = sizeof(struct sockaddr_in);
  }
  return (ssize_t)n;
}

ssize_t sctp_sendmsg(int                    fd,
                     const void*            msg,
                     size_t                 len,
                     struct sockaddr*       to,
                     socklen_t              tolen,
                     uint32_t               ppid,
                     uint32_t               flags,
                     uint16_t               stream_no,
                     uint32_t               timetolive,
                     uint32_t               context)
{
  (void)flags;
  (void)timetolive;
  auto c = find_conn(fd);
  if (c == nullptr) {
    errno = EBADF;
    return -1;
  }

  struct sctp_sndinfo info = {};
  info.snd_sid             = stream_no;
  info.snd_ppid            = ppid; // already network byte order from caller
  info.snd_context         = context;

  // One association per srsenb socket: addr is implied by the connect().
  (void)to;
  (void)tolen;
  ssize_t n =
      usrsctp_sendv(c->so, msg, len, nullptr, 0, &info, sizeof(info), SCTP_SENDV_SNDINFO, 0);
  return n;
}

} // extern "C"

#endif // HAVE_USRSCTP

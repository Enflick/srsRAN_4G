/**
 * Linux IP/UDP header compatibility for macOS.
 *
 * srsRAN uses the Linux <linux/ip.h> / <linux/udp.h> definitions of
 * struct iphdr and struct udphdr (with fields like ->version, ->saddr,
 * ->source). macOS only ships the BSD <netinet/ip.h> (struct ip) and a
 * differently-named struct udphdr, so provide the Linux-layout structs here.
 */

#ifndef SRSRAN_IP_COMPAT_H
#define SRSRAN_IP_COMPAT_H

#ifdef __APPLE__

#include <machine/endian.h>
#include <stdint.h>

// Linux-layout IPv4 header (struct ip in BSD has different field names).
struct iphdr {
#if BYTE_ORDER == LITTLE_ENDIAN
  uint8_t ihl : 4;
  uint8_t version : 4;
#else
  uint8_t version : 4;
  uint8_t ihl : 4;
#endif
  uint8_t  tos;
  uint16_t tot_len;
  uint16_t id;
  uint16_t frag_off;
  uint8_t  ttl;
  uint8_t  protocol;
  uint16_t check;
  uint32_t saddr;
  uint32_t daddr;
};

// Linux-layout UDP header. Guard against BSD's struct udphdr from netinet/udp.h.
#ifndef _NETINET_UDP_H_
#define _NETINET_UDP_H_
struct udphdr {
  uint16_t source;
  uint16_t dest;
  uint16_t len;
  uint16_t check;
};
#endif

#else // !__APPLE__

#include <linux/ip.h>
#include <linux/udp.h>

#endif // __APPLE__

#endif // SRSRAN_IP_COMPAT_H

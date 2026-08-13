/**
 * Endian conversion compatibility for macOS.
 *
 * Linux <endian.h> provides htole16/32/64, le16/32/64toh, and the big-endian
 * equivalents. macOS provides the same operations under different names in
 * <libkern/OSByteOrder.h>. Map the Linux names onto them.
 */

#ifndef SRSRAN_ENDIAN_COMPAT_H
#define SRSRAN_ENDIAN_COMPAT_H

#if defined(__APPLE__)

#include <libkern/OSByteOrder.h>

#define htole16(x) OSSwapHostToLittleInt16(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
#define htole64(x) OSSwapHostToLittleInt64(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)
#define le64toh(x) OSSwapLittleToHostInt64(x)

#define htobe16(x) OSSwapHostToBigInt16(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define htobe64(x) OSSwapHostToBigInt64(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define be64toh(x) OSSwapBigToHostInt64(x)

#else

#include <endian.h>

#endif

#endif // SRSRAN_ENDIAN_COMPAT_H

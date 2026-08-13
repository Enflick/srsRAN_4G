#
# Copyright 2013-2023 Software Radio Systems Limited
#
# This file is part of srsRAN
#
# srsRAN is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version.
#
# srsRAN is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# A copy of the GNU Affero General Public License can be found in
# the LICENSE file in the top-level directory of this distribution
# and at http://www.gnu.org/licenses/.
#

# - Try to find an SCTP implementation
#
# Once done this will define
#  SCTP_FOUND        - System has SCTP
#  SCTP_INCLUDE_DIRS - The SCTP include directories
#  SCTP_LIBRARIES    - The SCTP library
#
# On Linux this is kernel SCTP (libsctp, <netinet/sctp.h>).
# On macOS, which has no kernel SCTP, we fall back to libusrsctp (userland SCTP)
# together with srsRAN's sctp_compat shim (see lib/common/sctp_compat.*).

FIND_PACKAGE(PkgConfig REQUIRED)

# The two searches use separate cache variables, and SCTP_INCLUDE_DIRS /
# SCTP_LIBRARIES / SCTP_USERLAND are (re)derived from them on every configure.
# Caching those directly would make the userland fallback - and with it the
# HAVE_USRSCTP definition - be skipped on any re-configure.

# --- Try native kernel SCTP first (Linux) ---
PKG_CHECK_MODULES(PC_SCTP sctp)
FIND_PATH(
    SCTP_KERNEL_INCLUDE_DIRS
    NAMES netinet/sctp.h
    HINTS ${PC_SCTP_INCLUDEDIR}
          ${CMAKE_INSTALL_PREFIX}/include
    PATHS /usr/local/include
          /usr/include
)
FIND_LIBRARY(
    SCTP_KERNEL_LIBRARIES
    NAMES sctp
    HINTS ${PC_SCTP_LIBDIR}
          ${CMAKE_INSTALL_PREFIX}/lib
          ${CMAKE_INSTALL_PREFIX}/lib64
    PATHS /usr/local/lib
          /usr/local/lib64
          /usr/lib
          /usr/lib64
          /usr/lib/x86_64-linux-gnu/
)

unset(SCTP_INCLUDE_DIRS)
unset(SCTP_LIBRARIES)
set(SCTP_USERLAND FALSE)

if(SCTP_KERNEL_INCLUDE_DIRS AND SCTP_KERNEL_LIBRARIES)
    set(SCTP_INCLUDE_DIRS ${SCTP_KERNEL_INCLUDE_DIRS})
    set(SCTP_LIBRARIES    ${SCTP_KERNEL_LIBRARIES})
else()
    # --- Fall back to userland SCTP (libusrsctp), e.g. on macOS ---
    PKG_CHECK_MODULES(PC_USRSCTP usrsctp)
    FIND_PATH(
        SCTP_USERLAND_INCLUDE_DIRS
        NAMES usrsctp.h
        HINTS ${PC_USRSCTP_INCLUDEDIR}
              /usr/local/opt/libusrsctp/include
              /opt/homebrew/opt/libusrsctp/include
              ${CMAKE_INSTALL_PREFIX}/include
        PATHS /usr/local/include
              /usr/include
    )
    FIND_LIBRARY(
        SCTP_USERLAND_LIBRARIES
        NAMES usrsctp
        HINTS ${PC_USRSCTP_LIBDIR}
              /usr/local/opt/libusrsctp/lib
              /opt/homebrew/opt/libusrsctp/lib
              ${CMAKE_INSTALL_PREFIX}/lib
        PATHS /usr/local/lib
              /usr/lib
    )
    if(SCTP_USERLAND_INCLUDE_DIRS AND SCTP_USERLAND_LIBRARIES)
        set(SCTP_INCLUDE_DIRS ${SCTP_USERLAND_INCLUDE_DIRS})
        set(SCTP_LIBRARIES    ${SCTP_USERLAND_LIBRARIES})
        set(SCTP_USERLAND TRUE)
        # Selects the libusrsctp-backed implementation in sctp_compat.{h,cc}.
        add_definitions(-DHAVE_USRSCTP)
        message(STATUS "SCTP: no kernel SCTP found, using userland libusrsctp")
    endif()
endif()

message(STATUS "SCTP LIBRARIES: " ${SCTP_LIBRARIES})
message(STATUS "SCTP INCLUDE DIRS: " ${SCTP_INCLUDE_DIRS})

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(SCTP DEFAULT_MSG SCTP_LIBRARIES SCTP_INCLUDE_DIRS)
MARK_AS_ADVANCED(SCTP_KERNEL_LIBRARIES SCTP_KERNEL_INCLUDE_DIRS
                 SCTP_USERLAND_LIBRARIES SCTP_USERLAND_INCLUDE_DIRS)

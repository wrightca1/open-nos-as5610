# PPC32 big-endian toolchain for open-nos-as5610 (PowerPC e500v2 / P2020)
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=tools/ppc32-toolchain.cmake -B build .
#
# Prereq: apt-get install gcc-powerpc-linux-gnu
#
# COMPATIBILITY: This toolchain uses powerpc-linux-gnu-gcc which is a GLIBC
# toolchain. The rootfs MUST use glibc (Void Linux powerpc, not powerpc-musl).
# If you switch the rootfs to musl, you must also switch the compiler to a
# musl cross-toolchain (e.g. built via musl-cross-make with TARGET=powerpc-linux-musl).
#
# SYSROOT (recommended):
#   Set PPC32_SYSROOT env var to the rootfs staging directory so the linker
#   uses the exact same libc/headers as the target, not the build host's
#   cross-compilation defaults.
#
#   Example (build rootfs first, then):
#     PPC32_SYSROOT=/path/to/rootfs/staging cmake \
#       -DCMAKE_TOOLCHAIN_FILE=tools/ppc32-toolchain.cmake -B build .
#
# VERSION MATRIX (must all be built together):
#   nos_kernel_bde.ko  →  must match running kernel (same KERNEL_SRC tree)
#   nos_user_bde.ko    →  must match running kernel (same KERNEL_SRC tree)
#   libbcm56846.so     →  must link against rootfs libc (glibc powerpc)
#   nos-switchd        →  must link against rootfs libc (glibc powerpc)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

set(CMAKE_C_COMPILER   powerpc-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER powerpc-linux-gnu-g++)
set(CMAKE_AR           powerpc-linux-gnu-ar   CACHE FILEPATH "ar")
set(CMAKE_RANLIB       powerpc-linux-gnu-ranlib CACHE FILEPATH "ranlib")
set(CMAKE_STRIP        powerpc-linux-gnu-strip  CACHE FILEPATH "strip")

# CPU flags: P2020 is e500v2 core.
# NOTE: -mabi=spe / -mspe / -mfloat-gprs are embedded-ABI flags not supported
# by the powerpc-linux-gnu glibc toolchain. Use -mcpu=8548 only (correct for P2020;
# enables e500v2 ISA including SPE without changing the Linux syscall ABI).
set(CMAKE_C_FLAGS_INIT   "-mcpu=8548")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=8548")

# Sysroot: point at the Void Linux rootfs staging dir so we link against the
# exact libc that will be on the target (not the build host's cross headers).
# Set via env: PPC32_SYSROOT=/path/to/staging  OR  cmake -DPPC32_SYSROOT=...
if(DEFINED ENV{PPC32_SYSROOT} AND NOT DEFINED PPC32_SYSROOT)
    set(PPC32_SYSROOT "$ENV{PPC32_SYSROOT}")
endif()
if(DEFINED PPC32_SYSROOT AND NOT PPC32_SYSROOT STREQUAL "")
    set(CMAKE_SYSROOT "${PPC32_SYSROOT}")
    message(STATUS "PPC32 sysroot: ${PPC32_SYSROOT}")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

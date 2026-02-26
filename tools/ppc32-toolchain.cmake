# PPC32 big-endian toolchain for open-nos-as5610 (PowerPC e500v2 / P2020)
# Use: cmake -DCMAKE_TOOLCHAIN_FILE=tools/ppc32-toolchain.cmake -B build sdk/
# Prereq: apt-get install gcc-powerpc-linux-gnu

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

set(CMAKE_C_COMPILER powerpc-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER powerpc-linux-gnu-g++)
set(CMAKE_AR powerpc-linux-gnu-ar CACHE FILEPATH "ar")
set(CMAKE_RANLIB powerpc-linux-gnu-ranlib CACHE FILEPATH "ranlib")

# Optional: sysroot for Debian PPC32 rootfs (when building against target libs)
# set(CMAKE_SYSROOT /path/to/ppc32-sysroot)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

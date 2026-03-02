cmake_minimum_required(VERSION 3.10)
project(nos-switchd C)

set(CMAKE_C_STANDARD 99)

# When built via top-level CMake, bcm56846 target is already defined
set(SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk")
include_directories(${SDK_DIR}/include)

add_executable(nos-switchd
  src/main.c
  src/port_config.c
  src/tun.c
  src/netlink.c
  src/link_state.c
  src/tx_rx.c
)
# Link statically to avoid glibc version mismatch between build host
# (bookworm, glibc 2.34) and target (jessie, glibc 2.19).
# -static links everything including libc/libpthread statically;
# bcm56846_static is the static archive defined in sdk/CMakeLists.txt.
target_link_options(nos-switchd PRIVATE -static)
target_link_libraries(nos-switchd PRIVATE bcm56846_static pthread)

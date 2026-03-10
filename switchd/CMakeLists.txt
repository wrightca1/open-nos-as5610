cmake_minimum_required(VERSION 3.10)
project(nos-switchd C)

set(CMAKE_C_STANDARD 99)

# SDK location — when built standalone, use pre-built static library
set(SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk")
include_directories(${SDK_DIR}/include)
link_directories(${SDK_DIR}/build)

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
# libbcm56846.a is the static archive (OUTPUT_NAME bcm56846 in sdk/CMakeLists.txt).
target_link_options(nos-switchd PRIVATE -static)
target_link_libraries(nos-switchd PRIVATE bcm56846_static pthread)

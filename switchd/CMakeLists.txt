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
target_link_libraries(nos-switchd PRIVATE bcm56846)
target_link_libraries(nos-switchd PRIVATE pthread)

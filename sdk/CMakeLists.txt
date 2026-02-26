cmake_minimum_required(VERSION 3.10)
project(libbcm56846 C)

set(CMAKE_C_STANDARD 99)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

set(SDK_SOURCES
  src/bde_ioctl.c
  src/attach.c
  src/config.c
  src/soc.c
  src/schan.c
  src/reg.c
  src/port.c
  src/l2.c
  src/l3.c
  src/ecmp.c
  src/vlan.c
  src/pktio.c
  src/stats.c
  src/serdes.c
)

add_library(bcm56846 SHARED ${SDK_SOURCES})
set_target_properties(bcm56846 PROPERTIES
  VERSION 1.0.0
  SOVERSION 1
  PUBLIC_HEADER "include/bcm56846.h;include/bcm56846_types.h;include/bcm56846_regs.h"
)

add_library(bcm56846_static STATIC ${SDK_SOURCES})
set_target_properties(bcm56846_static PROPERTIES OUTPUT_NAME bcm56846)

cmake_minimum_required(VERSION 3.10)
project(tests C)

set(CMAKE_C_STANDARD 99)

# BDE validation test (Phase 1d) — no SDK link, just ioctl/mmap
add_executable(bde_validate bde_validate.c)
target_include_directories(bde_validate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include)

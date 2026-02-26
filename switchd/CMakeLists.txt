cmake_minimum_required(VERSION 3.10)
project(nos-switchd C)

set(CMAKE_C_STANDARD 99)

# When built via top-level CMake, bcm56846 target is already defined
set(SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk")
include_directories(${SDK_DIR}/include)

add_executable(nos-switchd src/main.c)
target_link_libraries(nos-switchd PRIVATE bcm56846)

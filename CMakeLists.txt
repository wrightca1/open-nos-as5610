# Top-level: build SDK then switchd (switchd links to libbcm56846)
cmake_minimum_required(VERSION 3.10)
project(open-nos-as5610 C CXX)

set(CMAKE_C_STANDARD 99)

add_subdirectory(sdk)
add_subdirectory(switchd)
add_subdirectory(tests)

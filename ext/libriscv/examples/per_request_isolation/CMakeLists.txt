cmake_minimum_required(VERSION 3.14)
project(per_request_isolation LANGUAGES C CXX)

# Fast-fork requires virtual paging (CoW pages).
# RISCV_FLAT_RW_ARENA must be OFF — flat arenas cannot be forked.
option(RISCV_FLAT_RW_ARENA "" OFF)
option(RISCV_32I           "" OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../c libriscv_capi)

add_executable(http_server server/main.c)
target_link_libraries(http_server riscv_capi)

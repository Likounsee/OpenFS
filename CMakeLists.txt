cmake_minimum_required(VERSION 3.20)
project(OpenFS VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(OPENFS_BUILD_TESTS "Build OpenFS tests" ON)

add_library(openfs STATIC
    src/crc32c.c
    src/format.c
)

target_include_directories(openfs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(openfs PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)

if(OPENFS_BUILD_TESTS)
    enable_testing()
    add_executable(openfs-format-test tests/test_format.c)
    target_link_libraries(openfs-format-test PRIVATE openfs)
    target_compile_options(openfs-format-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    add_test(NAME openfs-format COMMAND openfs-format-test)
endif()

cmake_minimum_required(VERSION 3.20)
project(OpenFS VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(OPENFS_BUILD_TESTS "Build OpenFS tests" ON)

add_library(openfs STATIC
    src/crc32c.c
    src/format.c
    src/inode.c
    src/inode_alloc.c
    src/dir.c
    src/mount.c
    src/bitmap.c
    src/allocator.c
    src/extent.c
    src/file.c
    src/path.c
    src/link.c
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
    add_executable(openfs-inode-test tests/test_inode.c)
    target_link_libraries(openfs-inode-test PRIVATE openfs)
    target_compile_options(openfs-inode-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    add_test(NAME openfs-inode COMMAND openfs-inode-test)
    add_executable(openfs-allocator-test tests/test_allocator.c)
    target_link_libraries(openfs-allocator-test PRIVATE openfs)
    target_compile_options(openfs-allocator-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    add_test(NAME openfs-allocator COMMAND openfs-allocator-test)
    add_executable(openfs-extent-test tests/test_extent.c)
    target_link_libraries(openfs-extent-test PRIVATE openfs)
    target_compile_options(openfs-extent-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    add_test(NAME openfs-extent COMMAND openfs-extent-test)
    add_executable(openfs-path-test tests/test_path.c)
    target_link_libraries(openfs-path-test PRIVATE openfs)
    target_compile_options(openfs-path-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    add_test(NAME openfs-path COMMAND openfs-path-test)
    add_executable(openfs-dir-test tests/test_dir.c)
    target_link_libraries(openfs-dir-test PRIVATE openfs)
    target_compile_options(openfs-dir-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    add_test(NAME openfs-dir COMMAND openfs-dir-test)
    add_executable(openfs-mount-test tests/test_mount.c)
    target_link_libraries(openfs-mount-test PRIVATE openfs)
    target_compile_options(openfs-mount-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    add_test(NAME openfs-mount COMMAND openfs-mount-test)
    add_executable(openfs-file-test tests/test_file.c)
    target_link_libraries(openfs-file-test PRIVATE openfs)
    target_compile_options(openfs-file-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    add_test(NAME openfs-file COMMAND openfs-file-test)
endif()

cmake_minimum_required(VERSION 3.20)
project(OpenFS VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(OPENFS_BUILD_TESTS "Build OpenFS tests" ON)

add_library(openfs STATIC
    src/crc32c.c
    src/time.c
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
    src/fsck.c
    src/journal.c
    src/transaction.c
)

target_include_directories(openfs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

function(openfs_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
endfunction()
openfs_enable_warnings(openfs)

add_subdirectory(adapters)

if(OPENFS_BUILD_TESTS)
    enable_testing()
    add_executable(openfs-timestamp-test tests/test_timestamp.c)
    target_link_libraries(openfs-timestamp-test PRIVATE openfs)
    add_test(NAME openfs-timestamp COMMAND openfs-timestamp-test)
    add_executable(openfs-format-test tests/test_format.c)
    target_link_libraries(openfs-format-test PRIVATE openfs)
    openfs_enable_warnings(openfs-format-test)
    add_test(NAME openfs-format COMMAND openfs-format-test)
    add_executable(openfs-inode-test tests/test_inode.c)
    target_link_libraries(openfs-inode-test PRIVATE openfs)
    openfs_enable_warnings(openfs-inode-test)
    add_test(NAME openfs-inode COMMAND openfs-inode-test)
    add_executable(openfs-allocator-test tests/test_allocator.c)
    target_link_libraries(openfs-allocator-test PRIVATE openfs)
    openfs_enable_warnings(openfs-allocator-test)
    add_test(NAME openfs-allocator COMMAND openfs-allocator-test)
    add_test(NAME openfs-extent COMMAND openfs-extent-test)
    add_executable(openfs-extent-test tests/test_extent.c)
    target_link_libraries(openfs-extent-test PRIVATE openfs)
    openfs_enable_warnings(openfs-extent-test)
    add_executable(openfs-transaction-test tests/test_transaction.c)
    target_link_libraries(openfs-transaction-test PRIVATE openfs)
    openfs_enable_warnings(openfs-transaction-test)
    add_test(NAME openfs-transaction COMMAND openfs-transaction-test)
    add_executable(openfs-journal-test tests/test_journal.c)
    target_link_libraries(openfs-journal-test PRIVATE openfs)
    openfs_enable_warnings(openfs-journal-test)
    add_test(NAME openfs-journal COMMAND openfs-journal-test)
    add_executable(openfs-fsck-test tests/test_fsck.c)
    target_link_libraries(openfs-fsck-test PRIVATE openfs)
    openfs_enable_warnings(openfs-fsck-test)
    add_test(NAME openfs-fsck COMMAND openfs-fsck-test)
    add_executable(openfs-link-test tests/test_link.c)
    target_link_libraries(openfs-link-test PRIVATE openfs)
    openfs_enable_warnings(openfs-link-test)
    add_test(NAME openfs-link COMMAND openfs-link-test)
    add_executable(openfs-path-test tests/test_path.c)
    target_link_libraries(openfs-path-test PRIVATE openfs)
    openfs_enable_warnings(openfs-path-test)
    add_test(NAME openfs-path COMMAND openfs-path-test)
    add_executable(openfs-dir-test tests/test_dir.c)
    target_link_libraries(openfs-dir-test PRIVATE openfs)
    openfs_enable_warnings(openfs-dir-test)
    add_test(NAME openfs-dir COMMAND openfs-dir-test)
    add_executable(openfs-mount-test tests/test_mount.c)
    target_link_libraries(openfs-mount-test PRIVATE openfs)
    openfs_enable_warnings(openfs-mount-test)
    add_test(NAME openfs-mount COMMAND openfs-mount-test)
    add_executable(openfs-file-test tests/test_file.c)
    target_link_libraries(openfs-file-test PRIVATE openfs)
    openfs_enable_warnings(openfs-file-test)
    add_test(NAME openfs-file COMMAND openfs-file-test)
    add_executable(openfs-crash-cut-test tests/test_crash_cut.c)
    target_link_libraries(openfs-crash-cut-test PRIVATE openfs)
    openfs_enable_warnings(openfs-crash-cut-test)
    add_test(NAME openfs-crash-cut COMMAND openfs-crash-cut-test)
    add_executable(openfs-namespace-crash-test tests/test_namespace_crash.c)
    target_link_libraries(openfs-namespace-crash-test PRIVATE openfs)
    openfs_enable_warnings(openfs-namespace-crash-test)
    add_test(NAME openfs-namespace-crash COMMAND openfs-namespace-crash-test)
endif()

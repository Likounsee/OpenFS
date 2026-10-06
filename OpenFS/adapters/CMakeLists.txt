cmake_minimum_required(VERSION 3.20)

if(UNIX AND NOT APPLE)
    add_library(openfs-linux-adapter STATIC linux/openfs_linux_adapter.c)
    target_include_directories(openfs-linux-adapter PUBLIC linux ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_link_libraries(openfs-linux-adapter PUBLIC openfs)
    openfs_enable_warnings(openfs-linux-adapter)

    if(OPENFS_BUILD_TESTS)
        add_executable(openfs-linux-adapter-test tests/test_linux_adapter.c)
        target_include_directories(openfs-linux-adapter-test PRIVATE linux)
        target_link_libraries(openfs-linux-adapter-test PRIVATE openfs-linux-adapter)
        openfs_enable_warnings(openfs-linux-adapter-test)
        add_test(NAME openfs-linux-adapter COMMAND openfs-linux-adapter-test)
    endif()
endif()

if(WIN32)
    add_library(openfs-windows-adapter STATIC windows/openfs_windows_adapter.c)
    target_include_directories(openfs-windows-adapter PUBLIC windows ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_link_libraries(openfs-windows-adapter PUBLIC openfs)
    openfs_enable_warnings(openfs-windows-adapter)
    add_executable(openfs-format-image ../tools/openfs_format_image.c)
    add_executable(openfs-format-device ../tools/openfs_format_device.c)
    add_executable(openfs-fsck-device ../tools/openfs_fsck_device.c)
    target_include_directories(openfs-format-device PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/windows
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    target_link_libraries(openfs-format-device PRIVATE openfs-windows-adapter)
    openfs_enable_warnings(openfs-format-device)
    target_include_directories(openfs-fsck-device PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/windows
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    target_link_libraries(openfs-fsck-device PRIVATE openfs-windows-adapter)
    openfs_enable_warnings(openfs-fsck-device)
    target_include_directories(openfs-format-image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_link_libraries(openfs-format-image PRIVATE openfs)
    openfs_enable_warnings(openfs-format-image)
    add_executable(openfs-usb-test ../tools/openfs_usb_test.c)
    target_include_directories(openfs-usb-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/windows
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    target_link_libraries(openfs-usb-test PRIVATE openfs-windows-adapter)
    openfs_enable_warnings(openfs-usb-test)

    if(OPENFS_BUILD_TESTS)
        add_executable(openfs-windows-adapter-test tests/test_windows_adapter.c)
        target_include_directories(openfs-windows-adapter-test PRIVATE windows)
        target_link_libraries(openfs-windows-adapter-test PRIVATE openfs-windows-adapter)
        openfs_enable_warnings(openfs-windows-adapter-test)
        add_test(NAME openfs-windows-adapter COMMAND openfs-windows-adapter-test)

        set(OPENFS_WINDOWS_IMAGE_TEST
            ${CMAKE_CURRENT_BINARY_DIR}/openfs-ci-image.img
        )
        add_test(
            NAME openfs-windows-image-format
            COMMAND openfs-format-image "${OPENFS_WINDOWS_IMAGE_TEST}" 33554432 65536
        )
        add_test(
            NAME openfs-windows-image-device
            COMMAND openfs-usb-test "${OPENFS_WINDOWS_IMAGE_TEST}" 65536
        )
        add_test(
            NAME openfs-windows-image-cleanup
            COMMAND ${CMAKE_COMMAND} -E rm -f "${OPENFS_WINDOWS_IMAGE_TEST}"
        )
        set_tests_properties(openfs-windows-image-cleanup PROPERTIES
            FIXTURES_CLEANUP openfs-windows-image
        )
        set_tests_properties(openfs-windows-image-format PROPERTIES
            FIXTURES_SETUP openfs-windows-image
        )
        set_tests_properties(openfs-windows-image-device PROPERTIES
            FIXTURES_REQUIRED openfs-windows-image
        )
        set(OPENFS_WINDOWS_FORMAT_DEVICE_TEST
            ${CMAKE_CURRENT_BINARY_DIR}/openfs-ci-device.img
        )
        add_test(
            NAME openfs-windows-device-format-image
            COMMAND openfs-format-image "${OPENFS_WINDOWS_FORMAT_DEVICE_TEST}" 8388608 65536
        )
        add_test(
            NAME openfs-windows-device-format
            COMMAND openfs-format-device "${OPENFS_WINDOWS_FORMAT_DEVICE_TEST}" 65536
        )
        add_test(
            NAME openfs-windows-device-format-cleanup
            COMMAND ${CMAKE_COMMAND} -E rm -f "${OPENFS_WINDOWS_FORMAT_DEVICE_TEST}"
        )
        set_tests_properties(openfs-windows-device-format-cleanup PROPERTIES
            FIXTURES_CLEANUP openfs-windows-device-format
        )
        set_tests_properties(openfs-windows-device-format-image PROPERTIES
            FIXTURES_SETUP openfs-windows-device-format
        )
        set_tests_properties(openfs-windows-device-format PROPERTIES
            FIXTURES_REQUIRED openfs-windows-device-format
        )
    endif()
endif()

add_library(openfs-archiaos-adapter STATIC archiaos/openfs_archiaos_adapter.c)
target_include_directories(openfs-archiaos-adapter PUBLIC archiaos ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(openfs-archiaos-adapter PUBLIC openfs)
openfs_enable_warnings(openfs-archiaos-adapter)

if(OPENFS_BUILD_TESTS)
    add_executable(openfs-archiaos-adapter-test tests/test_archiaos_adapter.c)
    target_include_directories(openfs-archiaos-adapter-test PRIVATE archiaos)
    target_link_libraries(openfs-archiaos-adapter-test PRIVATE openfs-archiaos-adapter)
    openfs_enable_warnings(openfs-archiaos-adapter-test)
    add_test(NAME openfs-archiaos-adapter COMMAND openfs-archiaos-adapter-test)
endif()

cmake_minimum_required(VERSION 3.20)

# Driver files install into the Wine arch layout under this root. The default
# follows the install prefix; set an absolute path when the target Wine keeps
# its libraries elsewhere (Debian/Ubuntu: /usr/lib/x86_64-linux-gnu/wine,
# WineHQ: /opt/wine-<branch>/lib/wine).
# Validated before project(): the guard must fire even where toolchain probing
# cannot run (the wine_install_root_relative CTest configures in a bare
# environment, so no generator or compiler may exist yet).
# STRING, not PATH: CMake would normalize a relative PATH value to absolute
# against the cwd, hiding the mistake from the IS_ABSOLUTE guard below.
set(PIPEASIO_WINE_INSTALL_ROOT "" CACHE STRING
    "absolute install root for driver files (default <prefix>/lib/wine)")
if(PIPEASIO_WINE_INSTALL_ROOT)
    # A relative value would split the install: install(FILES) would resolve
    # it under the prefix while install(CODE) resolved it against the install
    # process, landing symlinks and manifest entries elsewhere.
    if(NOT IS_ABSOLUTE "${PIPEASIO_WINE_INSTALL_ROOT}")
        message(FATAL_ERROR
            "PIPEASIO_WINE_INSTALL_ROOT must be an absolute path "
            "(got '${PIPEASIO_WINE_INSTALL_ROOT}'; e.g. "
            "/usr/lib/x86_64-linux-gnu/wine).")
    endif()
    set(PA_WINE_DEST "${PIPEASIO_WINE_INSTALL_ROOT}")
    set(PA_WINE_DEST_ABS "${PIPEASIO_WINE_INSTALL_ROOT}")
else()
    set(PA_WINE_DEST "lib/wine")
    # Stays a deferred reference: install(CODE) expands it at install time.
    set(PA_WINE_DEST_ABS "\${CMAKE_INSTALL_PREFIX}/lib/wine")
endif()
project(pipeasio C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# PipeWire headers compile into driver objects before winegcc links them.
# Steam Runtime 4 ships the supported 1.4.2 floor.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=1.4.2)
message(STATUS "Found libpipewire-0.3 ${PIPEWIRE_VERSION} (minimum 1.4.2)")

option(BUILD_WOW64_32 "Build the experimental 32-bit WoW64 PE front end + unixlib" OFF)
include(WineSdk)

# The driver: PE front end + unixlib (cmake/PeDriver.cmake).
# - <arch>-windows/pipeasio64.dll: the 64-bit PE front end per architecture
# - i386-windows/pipeasio32.dll:   the 32-bit front end (opt-in)
# - <host>-unix/pipeasio<N>.so:    the unixlib, one per module name
include(PeDriver)
pipeasio_add_pe_driver(NAME pipeasio64 PE_ARCH x86_64)
if(BUILD_WOW64_32)
    pipeasio_add_pe_driver(NAME pipeasio32 PE_ARCH i386)
endif()

# ARM64 front ends: aarch64 for native ARM64 Windows hosts, arm64ec for x86_64
# hosts under FEX.  Both need clang + lld and Wine's <arch>-windows import
# libraries; an ARM64 host's Wine ships those, an x86_64 host can only
# cross-build the PE halves (the unixlib is the host's).
option(BUILD_ARM64 "Build the aarch64 and arm64ec PE front ends when the toolchain allows" ON)
if(BUILD_ARM64)
    foreach(_arch aarch64 arm64ec)
        pipeasio_pe_arch_available(${_arch} _have)
        if(_have)
            pipeasio_add_pe_driver(NAME pipeasio64 PE_ARCH ${_arch} TARGET pipeasio64_${_arch})
            message(STATUS "ARM64 front end: ${_arch}")
        else()
            message(STATUS "ARM64 front end: ${_arch} skipped (no clang+lld or no "
                           "${WINE_LIB_ROOT}/${_arch}-windows/libwinecrt0.a)")
        endif()
    endforeach()
endif()

include(GNUInstallDirs)

install(PROGRAMS ${CMAKE_SOURCE_DIR}/pipeasio-register
        DESTINATION ${CMAKE_INSTALL_BINDIR})

# Optional native Qt6 settings panel; not used inside Wine/Proton.
option(BUILD_SETTINGS_PANEL "Build the Qt6 settings panel" ON)
if(BUILD_SETTINGS_PANEL)
    include(CheckLanguage)
    check_language(CXX)
    if(CMAKE_CXX_COMPILER)
        enable_language(CXX)
        set(CMAKE_CXX_STANDARD 17)
        find_package(Qt6 QUIET COMPONENTS Widgets)
        if(Qt6_FOUND)
            add_subdirectory(gui)
        else()
            message(WARNING "Qt6 Widgets not found - skipping the settings panel "
                            "(set -DBUILD_SETTINGS_PANEL=OFF to silence).")
        endif()
    else()
        message(WARNING "No C++ compiler - skipping the settings panel.")
    endif()
endif()

# Test rig: standalone Wine probe host + Linux-native unit tests.
option(BUILD_TESTS "Build the asio_probe Wine host and unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/asio_probe)
    add_subdirectory(tests/asio_loopback)
    add_subdirectory(tests/unit)
    add_subdirectory(tests/pw_probe)
    add_subdirectory(tests/unixlib)

    # pipeasio-register discovery/warning logic; self-contained (stubs wine).
    find_program(BASH_EXECUTABLE bash REQUIRED)
    add_test(NAME register_script
             COMMAND ${BASH_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/tests/register/run.sh)
    set_tests_properties(register_script PROPERTIES
        TIMEOUT 30
        SKIP_RETURN_CODE 77)

    # A relative PIPEASIO_WINE_INSTALL_ROOT must fail at configure time.
    add_test(NAME wine_install_root_relative
             COMMAND ${CMAKE_COMMAND}
                     -S ${CMAKE_SOURCE_DIR}
                     -B ${CMAKE_BINARY_DIR}/test-wine-root-relative
                     -DPIPEASIO_WINE_INSTALL_ROOT=lib64/wine)
    set_tests_properties(wine_install_root_relative PROPERTIES
        TIMEOUT 60
        PASS_REGULAR_EXPRESSION "PIPEASIO_WINE_INSTALL_ROOT must be an absolute")
    if(BUILD_SETTINGS_PANEL AND Qt6_FOUND)
        add_subdirectory(tests/gui)
    elseif(BUILD_SETTINGS_PANEL)
        add_test(NAME test_panel_prerequisite
                 COMMAND ${CMAKE_COMMAND} -P
                         ${CMAKE_SOURCE_DIR}/tests/gui/check_prerequisite.cmake)
        set_tests_properties(test_panel_prerequisite PROPERTIES
            TIMEOUT 10
            SKIP_REGULAR_EXPRESSION "SKIP:")
    endif()
    if(PIPEASIO_ASAN)
        find_program(BASH_EXECUTABLE bash REQUIRED)
        set(_pipeasio_sanitizer_artifacts
            ${CMAKE_BINARY_DIR}/${PIPEASIO_UNIX_ARCH}-unix/pipeasio64.so
            ${CMAKE_BINARY_DIR}/${PIPEASIO_UNIX_ARCH}-unix/pipeasio64_stop_test.so)
        if(BUILD_WOW64_32)
            list(APPEND _pipeasio_sanitizer_artifacts ${CMAKE_BINARY_DIR}/${PIPEASIO_UNIX_ARCH}-unix/pipeasio32.so)
        endif()
        foreach(_target test_offsets test_config test_admission_gate
                         test_pw_buffer_region test_handle_table pw_filter_probe
                         pw_delivery_probe pw_default_probe pipeasio-settings test_panel)
            if(TARGET ${_target})
                list(APPEND _pipeasio_sanitizer_artifacts $<TARGET_FILE:${_target}>)
            endif()
        endforeach()
        add_test(NAME sanitizer_artifacts
                 COMMAND ${BASH_EXECUTABLE}
                         ${CMAKE_SOURCE_DIR}/tests/check_sanitizer_artifacts.sh
                         ${_pipeasio_sanitizer_artifacts})
        set_tests_properties(sanitizer_artifacts PROPERTIES
            TIMEOUT 10
            LABELS "pipeasio-sanitize"
            FIXTURES_SETUP sanitizer_artifacts)
    endif()
endif()

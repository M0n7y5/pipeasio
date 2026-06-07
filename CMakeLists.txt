cmake_minimum_required(VERSION 3.20)
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

# libpipewire-0.3 backs src/audio.c — its headers feed the OBJECT lib
# compile step via INCLUDES, and -lpipewire-0.3 reaches the winegcc link
# step via LIBS.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
message(STATUS "Found libpipewire-0.3 ${PIPEWIRE_VERSION}")

include(WineDLL)

add_wine_dll(
    NAME      pipeasio64
    SPEC      ${CMAKE_SOURCE_DIR}/pipeasio.dll.spec
    SOURCES   src/asio.c
              src/audio.c
              src/config.c
              src/main.c
              src/regsvr.c
    INCLUDES  ${CMAKE_SOURCE_DIR}/include
              ${PIPEWIRE_INCLUDE_DIRS}
    LIBS      odbc32 ole32 uuid winmm pipewire-0.3
)

include(GNUInstallDirs)

install(PROGRAMS ${CMAKE_SOURCE_DIR}/pipeasio-register
        DESTINATION ${CMAKE_INSTALL_BINDIR})

# Native Qt6 settings panel (pipeasio-settings), launched by ControlPanel on the
# user's Linux desktop.  Optional and self-contained: the core Wine driver builds
# fine without it.  Skips gracefully if no C++ compiler or Qt6 Widgets is present.
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
            message(WARNING "Qt6 Widgets not found — skipping the settings panel "
                            "(set -DBUILD_SETTINGS_PANEL=OFF to silence).")
        endif()
    else()
        message(WARNING "No C++ compiler — skipping the settings panel.")
    endif()
endif()

# Test rig: standalone Wine probe host + Linux-native unit tests.
option(BUILD_TESTS "Build the asio_probe Wine host and unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/asio_probe)
    add_subdirectory(tests/unit)
    add_subdirectory(tests/pw_probe)
    if(BUILD_SETTINGS_PANEL AND Qt6_FOUND)
        add_subdirectory(tests/gui)
    endif()
endif()

cmake_minimum_required(VERSION 3.20)
project(wineasio C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# libpipewire-0.3 is required at configure time so the dev environment is
# verified up-front, even though it is not linked into the DLL yet.  Phase 3
# replaces src/audio.c's body with real PipeWire code; src/audio.c will then
# add target_link_libraries(wineasio64_objs PRIVATE ${PIPEWIRE_LIBRARIES}).
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
message(STATUS "Found libpipewire-0.3 ${PIPEWIRE_VERSION}")

include(WineDLL)

add_wine_dll(
    NAME      wineasio64
    SPEC      ${CMAKE_SOURCE_DIR}/wineasio.dll.spec
    SOURCES   src/asio.c
              src/audio.c
              src/jackbridge.c
              src/main.c
              src/regsvr.c
    INCLUDES  ${CMAKE_SOURCE_DIR}/include
    LIBS      odbc32 ole32 uuid winmm
)

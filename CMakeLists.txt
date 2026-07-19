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

# PipeWire is linked through winegcc, but its headers compile into the OBJECT
# library first.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
message(STATUS "Found libpipewire-0.3 ${PIPEWIRE_VERSION}")

# Build identification: build_info.h is regenerated on every build when the
# git state moves (number = commit count, hash, -dirty marker).
add_custom_target(build_info ALL
    ${CMAKE_COMMAND}
        -DSRC=${CMAKE_SOURCE_DIR}
        -DOUT=${CMAKE_BINARY_DIR}/generated/build_info.h
        -P ${CMAKE_SOURCE_DIR}/cmake/GenBuildInfo.cmake
    VERBATIM)

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
              ${CMAKE_BINARY_DIR}/generated
    LIBS      odbc32 ole32 uuid winmm user32 pipewire-0.3
)
add_dependencies(pipeasio64_objs build_info)
# build_info.h is included by these files; make it a REAL dependency (the
# target-level add_dependencies above is order-only and would not rebuild
# them when the header is rewritten with a new build id).
set_source_files_properties(src/audio.c src/asio.c PROPERTIES
    OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/generated/build_info.h")

# Optional 32-bit WoW64 front end.
# - pipeasio32.dll: i386 PE stub, installed under lib/wine/i386-windows
# - pipeasio32.so:  x86_64 unixlib, installed under lib/wine/x86_64-unix
option(BUILD_WOW64_32 "Build the experimental 32-bit WoW64 PE front end + unixlib" OFF)
if(BUILD_WOW64_32)
    find_program(I686_W64_MINGW_GCC i686-w64-mingw32-gcc)
    find_program(I686_W64_MINGW_RANLIB i686-w64-mingw32-ranlib)
    set(WINE_LIB_ROOT "/usr/lib/wine" CACHE PATH
        "Wine lib/wine directory holding the i386-windows import libraries")
    if(NOT I686_W64_MINGW_GCC OR NOT I686_W64_MINGW_RANLIB)
        message(FATAL_ERROR "BUILD_WOW64_32=ON requires i686-w64-mingw32-gcc and "
                            "i686-w64-mingw32-ranlib (install mingw-w64-gcc).")
    endif()
    if(NOT EXISTS "${WINE_LIB_ROOT}/i386-windows/libwinecrt0.a")
        message(FATAL_ERROR "BUILD_WOW64_32=ON: ${WINE_LIB_ROOT}/i386-windows/libwinecrt0.a "
                            "not found; pass -DWINE_LIB_ROOT=<lib/wine>.")
    endif()

    set(PA32_DLL  "${CMAKE_BINARY_DIR}/pipeasio32.dll")
    set(PA32_SO   "${CMAKE_BINARY_DIR}/pipeasio32.so")
    set(PA32_DEF  "${CMAKE_SOURCE_DIR}/src/wow64/pipeasio32.def")
    set(PA32_SPEC "${CMAKE_SOURCE_DIR}/src/wow64/pipeasio32_unixlib.spec")

    # The i386 Wine import libs must be copied, made writable, and re-indexed
    # with the i686 ranlib before the mingw linker accepts them.
    set(_pa32_imp_names winecrt0 ntdll ole32 uuid)
    set(_pa32_imp_libs "")
    set(_pa32_imp_cmds "")
    set(_pa32_imp_deps "")
    foreach(_n ${_pa32_imp_names})
        set(_src "${WINE_LIB_ROOT}/i386-windows/lib${_n}.a")
        set(_dst "${CMAKE_BINARY_DIR}/lib${_n}-i386.a")
        list(APPEND _pa32_imp_libs "${_dst}")
        list(APPEND _pa32_imp_deps "${_src}")
        list(APPEND _pa32_imp_cmds
            COMMAND ${CMAKE_COMMAND} -E copy "${_src}" "${_dst}"
            COMMAND chmod u+w "${_dst}"
            COMMAND "${I686_W64_MINGW_RANLIB}" "${_dst}")
    endforeach()
    add_custom_command(
        OUTPUT  ${_pa32_imp_libs}
        ${_pa32_imp_cmds}
        DEPENDS ${_pa32_imp_deps}
        VERBATIM
        COMMENT "index i386 Wine import libraries")

    set(_pa32_inc -I "${CMAKE_SOURCE_DIR}/include" -I "${CMAKE_SOURCE_DIR}/src/wow64")
    foreach(_d ${WINE_INCLUDE_DIRS})
        list(APPEND _pa32_inc -I "${_d}")
    endforeach()
    set(_pa32_cflags -Wall -Wextra -Werror=implicit-function-declaration -msse3
        $<$<CONFIG:Release>:-O2> $<$<CONFIG:Release>:-DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:-O2> $<$<CONFIG:RelWithDebInfo>:-g>
        $<$<CONFIG:RelWithDebInfo>:-DNDEBUG>
        $<$<CONFIG:Debug>:-O0> $<$<CONFIG:Debug>:-g3> $<$<CONFIG:Debug>:-DDEBUG>
        $<$<CONFIG:Debug>:-fno-omit-frame-pointer>)

    # i386 PE front end.  The shared ASIO sources link against the WoW64
    # proxy instead of src/audio.c.
    add_custom_command(
        OUTPUT  "${PA32_DLL}"
        COMMAND "${I686_W64_MINGW_GCC}" -shared
                "${CMAKE_SOURCE_DIR}/src/asio.c"
                "${CMAKE_SOURCE_DIR}/src/main.c"
                "${CMAKE_SOURCE_DIR}/src/regsvr.c"
                "${CMAKE_SOURCE_DIR}/src/config.c"
                "${CMAKE_SOURCE_DIR}/src/wow64/audio_proxy.c"
                "${PA32_DEF}"
                "${CMAKE_BINARY_DIR}/libwinecrt0-i386.a"
                -DPIPEASIO_WOW64_PE
                ${_pa32_inc} ${_pa32_cflags}
                -o "${PA32_DLL}"
                "${CMAKE_BINARY_DIR}/libntdll-i386.a"
                "${CMAKE_BINARY_DIR}/libole32-i386.a"
                "${CMAKE_BINARY_DIR}/libuuid-i386.a"
        COMMAND "${WINEBUILD}" -m32 --builtin "${PA32_DLL}"
        DEPENDS "${CMAKE_SOURCE_DIR}/src/asio.c"
                "${CMAKE_SOURCE_DIR}/src/main.c"
                "${CMAKE_SOURCE_DIR}/src/regsvr.c"
                "${CMAKE_SOURCE_DIR}/src/config.c"
                "${CMAKE_SOURCE_DIR}/src/wow64/audio_proxy.c"
                "${PA32_DEF}" ${_pa32_imp_libs}
        VERBATIM COMMAND_EXPAND_LISTS
        COMMENT "mingw pipeasio32.dll (32-bit WoW64 PE front end)")

    # 64-bit unixlib hosting the real PipeWire backend.
    add_library(pipeasio32_unix_objs OBJECT
        src/wow64/audio_unix.c src/audio.c src/config.c)
    set_target_properties(pipeasio32_unix_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(pipeasio32_unix_objs PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/wow64
        ${PIPEWIRE_INCLUDE_DIRS} ${WINE_INCLUDE_DIRS})
    target_compile_options(pipeasio32_unix_objs PRIVATE
        -DPIPEASIO_AUDIO_UNIXLIB -D_REENTRANT
        -Wall -fno-strict-aliasing -Werror=implicit-function-declaration
        $<$<CONFIG:Release>:-O2> $<$<CONFIG:Release>:-DNDEBUG>
        $<$<CONFIG:Release>:-fvisibility=hidden>
        $<$<CONFIG:RelWithDebInfo>:-O2> $<$<CONFIG:RelWithDebInfo>:-g>
        $<$<CONFIG:RelWithDebInfo>:-DNDEBUG> $<$<CONFIG:RelWithDebInfo>:-fvisibility=hidden>
        $<$<CONFIG:Debug>:-O0> $<$<CONFIG:Debug>:-g3> $<$<CONFIG:Debug>:-DDEBUG>
        $<$<CONFIG:Debug>:-fno-omit-frame-pointer>)
    add_custom_command(
        OUTPUT  "${PA32_SO}"
        COMMAND "${WINEGCC}" -shared
                "${PA32_SPEC}"
                $<TARGET_OBJECTS:pipeasio32_unix_objs>
                -lpthread -ldl ${PIPEWIRE_LDFLAGS}
                -o "${PA32_SO}"
        DEPENDS pipeasio32_unix_objs "${PA32_SPEC}"
                $<TARGET_OBJECTS:pipeasio32_unix_objs>
        VERBATIM COMMAND_EXPAND_LISTS
        COMMENT "winegcc pipeasio32.so (WoW64 unixlib)")

    add_custom_target(pipeasio32_unix ALL DEPENDS "${PA32_SO}")
    add_custom_target(pipeasio32 ALL DEPENDS "${PA32_DLL}")
    add_dependencies(pipeasio32 pipeasio32_unix)

    install(FILES "${PA32_DLL}" DESTINATION lib/wine/i386-windows)
    install(FILES "${PA32_SO}" DESTINATION lib/wine/x86_64-unix
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                        GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
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
    add_subdirectory(tests/wow64)
    if(BUILD_SETTINGS_PANEL AND Qt6_FOUND)
        add_subdirectory(tests/gui)
    endif()
endif()

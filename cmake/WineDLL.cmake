# WineDLL.cmake - build a Wine DLL (PE fake + ELF .so) from one source set.
#
# Usage:
#   add_wine_dll(
#       NAME      pipeasio64
#       SPEC      ${CMAKE_SOURCE_DIR}/pipeasio.dll.spec
#       SOURCES   src/foo.c src/bar.c
#       INCLUDES  ${CMAKE_SOURCE_DIR}/include
#       LIBS      odbc32 ole32 uuid winmm
#   )
#
# Produces:
#   ${CMAKE_BINARY_DIR}/${NAME}.dll       (Wine fake PE DLL, via winebuild)
#   ${CMAKE_BINARY_DIR}/${NAME}.dll.so    (ELF shared object, via winegcc)
#
# Install layout (under CMAKE_INSTALL_PREFIX):
#   lib/wine/x86_64-windows/${NAME}.dll  + pipeasio.dll  symlink
#   lib/wine/x86_64-unix/${NAME}.dll.so  + pipeasio.dll.so  symlink

find_program(WINEBUILD winebuild REQUIRED)
find_program(WINEGCC   winegcc   REQUIRED)

# Probe for Wine include directories.  We mirror the fallback list the legacy
# Makefile.mk used, but filter to those that actually exist on this host so
# clangd doesn't choke on dangling -I flags.
if(NOT WINE_INCLUDE_DIRS)
    set(_wine_inc_candidates
        /usr/include/wine
        /usr/include/wine/windows
        /usr/include/wine-development
        /usr/include/wine-development/wine/windows
        /opt/wine-stable/include
        /opt/wine-stable/include/wine/windows
        /opt/wine-staging/include
        /opt/wine-staging/include/wine/windows)
    set(WINE_INCLUDE_DIRS "")
    foreach(_d ${_wine_inc_candidates})
        if(IS_DIRECTORY "${_d}")
            list(APPEND WINE_INCLUDE_DIRS "${_d}")
        endif()
    endforeach()
endif()
if(NOT WINE_INCLUDE_DIRS)
    message(FATAL_ERROR "No Wine SDK include directory found. Install wine-devel / wine-dev / winehq-stable-dev.")
endif()
message(STATUS "Wine include dirs: ${WINE_INCLUDE_DIRS}")

function(add_wine_dll)
    set(_options "")
    set(_one     NAME SPEC)
    set(_multi   SOURCES INCLUDES LIBS)
    cmake_parse_arguments(WDL "${_options}" "${_one}" "${_multi}" ${ARGN})

    if(NOT WDL_NAME OR NOT WDL_SPEC OR NOT WDL_SOURCES)
        message(FATAL_ERROR "add_wine_dll: NAME, SPEC, and SOURCES are required.")
    endif()

    # Compile sources to PIC .o files with the host gcc.  These objects are
    # consumed by both the winebuild (PE fake) and winegcc (ELF .so) steps.
    set(_objlib ${WDL_NAME}_objs)
    add_library(${_objlib} OBJECT ${WDL_SOURCES})
    set_target_properties(${_objlib} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${_objlib} PRIVATE
        ${WDL_INCLUDES}
        ${WINE_INCLUDE_DIRS})
    target_compile_options(${_objlib} PRIVATE
        -D_REENTRANT
        -Wall -pipe
        -fno-strict-aliasing
        -Wwrite-strings
        -Wpointer-arith
        -Werror=implicit-function-declaration
        $<$<CONFIG:Release>:-O2 -DNDEBUG -fvisibility=hidden>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG -fvisibility=hidden>
        $<$<CONFIG:Debug>:-O0 -g3 -DDEBUG -fno-omit-frame-pointer -fstack-protector-all>)

    # Debug builds always get -O0 -g3 (from the CONFIG:Debug block above).
    # AddressSanitizer is opt-in via PIPEASIO_ASAN=ON because Wine's
    # dynamic loader can't reliably make libasan first in the library
    # list, which makes the runtime refuse to start. When enabled,
    # winegcc filters -fsanitize from its gcc link invocation, so we
    # request the libraries explicitly with -lasan/-lubsan.
    set(_winegcc_extra_flags "")
    option(PIPEASIO_ASAN "Build .so half with -fsanitize=address,undefined" OFF)
    if(PIPEASIO_ASAN)
        target_compile_options(${_objlib} PRIVATE
            -fsanitize=address -fsanitize=undefined)
        list(APPEND _winegcc_extra_flags -lasan -lubsan)
    endif()

    set(_pe "${CMAKE_BINARY_DIR}/${WDL_NAME}.dll")
    set(_so "${CMAKE_BINARY_DIR}/${WDL_NAME}.dll.so")

    # Step 1: Wine fake PE DLL via winebuild.
    add_custom_command(
        OUTPUT  ${_pe}
        COMMAND ${WINEBUILD} -m64 --dll --fake-module
                -E ${WDL_SPEC}
                $<TARGET_OBJECTS:${_objlib}>
                -o ${_pe}
        DEPENDS ${_objlib} ${WDL_SPEC} $<TARGET_OBJECTS:${_objlib}>
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "winebuild ${WDL_NAME}.dll (fake PE module)")

    # Step 2: ELF .so via winegcc.
    set(_lflags "")
    foreach(_l ${WDL_LIBS})
        list(APPEND _lflags -l${_l})
    endforeach()
    add_custom_command(
        OUTPUT  ${_so}
        COMMAND ${WINEGCC} -shared
                ${WDL_SPEC}
                $<TARGET_OBJECTS:${_objlib}>
                ${_winegcc_extra_flags}
                ${_lflags}
                -o ${_so}
        DEPENDS ${_objlib} ${WDL_SPEC} $<TARGET_OBJECTS:${_objlib}>
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "winegcc ${WDL_NAME}.dll.so (ELF shared object)")

    add_custom_target(${WDL_NAME} ALL DEPENDS ${_pe} ${_so})

    # Install into the Wine arch layout, plus the unified-name symlinks that
    # Wine 10+ looks up.
    install(FILES ${_pe}
            DESTINATION lib/wine/x86_64-windows)
    install(FILES ${_so}
            DESTINATION lib/wine/x86_64-unix
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                        GROUP_READ GROUP_EXECUTE
                        WORLD_READ WORLD_EXECUTE)
    # $ENV{DESTDIR} keeps staged installs (DESTDIR=pkg cmake --install) from
    # writing symlinks into the live prefix; install(CODE) does not apply
    # DESTDIR automatically the way install(FILES) does.
    install(CODE "
        file(CREATE_LINK ${WDL_NAME}.dll
             \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/lib/wine/x86_64-windows/pipeasio.dll
             SYMBOLIC)
        file(CREATE_LINK ${WDL_NAME}.dll.so
             \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/lib/wine/x86_64-unix/pipeasio.dll.so
             SYMBOLIC)")
endfunction()

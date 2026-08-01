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

# Probe for Wine include directories.  An SDK root R holds "wine/debug.h" plus
# the Win32 headers, under "R/wine/windows" (Arch, Fedora, winehq, upstream
# "make install") or "R/windows" (Debian's libwine-dev); packagings differ only
# in where R sits.  winebuild at <root>/bin/winebuild implies <root>/include, so
# that root is tried first: it is the only SDK guaranteed to match the Wine we
# build against.  The first complete root wins, never a mix of two Wines.
if(NOT WINE_INCLUDE_DIRS)
    get_filename_component(_wine_bindir "${WINEBUILD}" DIRECTORY)
    get_filename_component(_wine_root "${_wine_bindir}" DIRECTORY)
    set(_wine_sdk_roots
        "${_wine_root}/include"
        /usr/include             # Arch, Fedora
        /usr/include/wine        # Debian/Ubuntu libwine-dev
        /usr/include/wine-development
        /opt/wine-devel/include
        /opt/wine-stable/include
        /opt/wine-staging/include)
    set(WINE_INCLUDE_DIRS "")
    set(_wine_chosen_root "")
    foreach(_r ${_wine_sdk_roots})
        if(NOT EXISTS "${_r}/wine/debug.h")
            continue()
        endif()
        if(NOT EXISTS "${_r}/wine/windows/windows.h"
           AND NOT EXISTS "${_r}/windows/windows.h")
            continue()
        endif()
        # The root plus its header subtrees: wine/ holds unixlib.h, the Win32
        # SDK sits in wine/windows or windows/.  /usr/include is already on the
        # compiler's path, and as -I it would drag glibc into the mingw compile.
        foreach(_sub "" /wine /wine/windows /windows)
            if("${_r}${_sub}" STREQUAL "/usr/include")
                continue()
            endif()
            if(IS_DIRECTORY "${_r}${_sub}")
                list(APPEND WINE_INCLUDE_DIRS "${_r}${_sub}")
            endif()
        endforeach()
        set(_wine_chosen_root "${_r}")
        break()
    endforeach()
    list(REMOVE_DUPLICATES WINE_INCLUDE_DIRS)

    # WineHQ splits each branch: wine-<branch> ships bin/winebuild, the
    # wine-<branch>-devel companion ships include/wine/.  Falling back to
    # another prefix's headers is a silent Wine version mismatch.
    if(_wine_chosen_root AND NOT "${_wine_root}/include" STREQUAL "${_wine_chosen_root}"
       AND NOT "${_wine_root}" STREQUAL "/usr")
        message(WARNING
            "${WINEBUILD} belongs to ${_wine_root}, but ${_wine_root}/include "
            "holds no Wine SDK, so ${_wine_chosen_root} is used instead - those "
            "headers may not match that Wine.\n"
            "Install the SDK next to it (WineHQ: wine-devel-devel / "
            "wine-stable-devel / wine-staging-devel, matching the branch).")
    endif()
endif()

# Validate the result, probed or user-supplied: a wrong -DWINE_INCLUDE_DIRS (the
# install root instead of its include dirs) otherwise configures cleanly and
# fails later on "wine/debug.h: No such file".  Only wine/debug.h may come from
# the compiler's own /usr/include; windows.h and unixlib.h must be reachable
# through -I, since the WoW64 PE half is built by the mingw cross compiler.
set(_wine_hdr_checks wine/debug.h windows.h)
if(BUILD_WOW64_32)
    list(APPEND _wine_hdr_checks unixlib.h)
endif()
set(_wine_hdr_missing "")
foreach(_h ${_wine_hdr_checks})
    set(_wine_hdr_search ${WINE_INCLUDE_DIRS})
    if("${_h}" STREQUAL "wine/debug.h")
        list(APPEND _wine_hdr_search /usr/include)
    endif()
    set(_wine_hdr_found FALSE)
    foreach(_d ${_wine_hdr_search})
        if(EXISTS "${_d}/${_h}")
            set(_wine_hdr_found TRUE)
            break()
        endif()
    endforeach()
    if(NOT _wine_hdr_found)
        list(APPEND _wine_hdr_missing "${_h}")
    endif()
endforeach()
if(_wine_hdr_missing)
    message(FATAL_ERROR
        "Wine SDK headers not found (missing: ${_wine_hdr_missing}).\n"
        "Install your distro's Wine development package:\n"
        "  Arch / CachyOS:  pacman -S wine\n"
        "  Fedora:          dnf install wine-devel\n"
        "  Debian / Ubuntu: apt install libwine-dev wine64-tools\n"
        "From the WineHQ repositories the SDK is a separate package beside the "
        "branch, which ships only bin/: wine-devel-devel (Fedora) or "
        "wine-devel-dev (Debian), matching your branch.\n"
        "Or point at an existing SDK with the include directories themselves "
        "(not the install root) - the root, its wine/ subdirectory, and the "
        "Win32 headers, e.g.\n"
        "  -DWINE_INCLUDE_DIRS=\"/opt/wine-devel/include;"
        "/opt/wine-devel/include/wine;/opt/wine-devel/include/wine/windows\"")
endif()
message(STATUS "Wine include dirs: ${WINE_INCLUDE_DIRS}")

function(add_wine_dll)
    set(_options NO_INSTALL)
    set(_one     NAME SPEC)
    set(_multi   SOURCES INCLUDES LIBS LDFLAGS DEFINES)
    cmake_parse_arguments(WDL "${_options}" "${_one}" "${_multi}" ${ARGN})

    if(NOT WDL_NAME OR NOT WDL_SPEC OR NOT WDL_SOURCES)
        message(FATAL_ERROR "add_wine_dll: NAME, SPEC, and SOURCES are required.")
    endif()

    # Compile sources to PIC .o files with the host gcc.  These objects are
    # consumed by both the winebuild (PE fake) and winegcc (ELF .so) steps.
    # Force -fno-lto: distro CFLAGS often inject -flto=auto (Fedora); objects
    # still show Dll* in nm, but winebuild's ld -r partial link leaves those
    # symbols undefined so the .spec export scan fails ("function
    # 'DllRegisterServer' not defined"). LTO bytecode is useless to
    # winebuild/winegcc; objects need a real ELF symtab (issue #6).
    set(_objlib ${WDL_NAME}_objs)
    add_library(${_objlib} OBJECT ${WDL_SOURCES})
    set_target_properties(${_objlib} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${_objlib} PRIVATE
        ${WDL_INCLUDES}
        ${WINE_INCLUDE_DIRS})
    target_compile_definitions(${_objlib} PRIVATE ${WDL_DEFINES})
    target_compile_options(${_objlib} PRIVATE
        -D_REENTRANT
        -Wall -pipe
        -fno-strict-aliasing
        -Wwrite-strings
        -Wpointer-arith
        -Werror=implicit-function-declaration
        -fno-lto
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
    # LIBS are bare names (-lfoo) resolved through winegcc's own search path,
    # which is where the Win32 import libraries live.  LDFLAGS is appended
    # verbatim and is meant for absolute library paths (pkg-config's
    # <pkg>_LINK_LIBRARIES) so a PipeWire outside /usr is honoured.  Do NOT
    # feed raw -L here: a -L/usr/lib ahead of winegcc's own directories makes
    # -luuid resolve to util-linux's libuuid instead of Wine's import library,
    # and the link dies on an undefined IID_IUnknown.
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
                ${WDL_LDFLAGS}
                -o ${_so}
        DEPENDS ${_objlib} ${WDL_SPEC} $<TARGET_OBJECTS:${_objlib}>
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "winegcc ${WDL_NAME}.dll.so (ELF shared object)")

    add_custom_target(${WDL_NAME} ALL DEPENDS ${_pe} ${_so})

    if(NOT WDL_NO_INSTALL)
        # Install into the Wine arch layout, plus the unified-name symlinks
        # that Wine 10+ looks up.
        install(FILES ${_pe}
                DESTINATION "${PA_WINE_DEST}/x86_64-windows")
        install(FILES ${_so}
                DESTINATION "${PA_WINE_DEST}/x86_64-unix"
                PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                            GROUP_READ GROUP_EXECUTE
                            WORLD_READ WORLD_EXECUTE)
        # $ENV{DESTDIR} keeps staged installs from writing symlinks into the
        # live prefix; install(CODE) does not apply it like install(FILES).
        install(CODE "
            file(CREATE_LINK ${WDL_NAME}.dll
                 \$ENV{DESTDIR}${PA_WINE_DEST_ABS}/x86_64-windows/pipeasio.dll
                 SYMBOLIC)
            file(CREATE_LINK ${WDL_NAME}.dll.so
                 \$ENV{DESTDIR}${PA_WINE_DEST_ABS}/x86_64-unix/pipeasio.dll.so
                 SYMBOLIC)
            # file(CREATE_LINK) does not feed the manifest like file(INSTALL),
            # so record the links by hand or the documented uninstall leaves
            # them behind.
            list(APPEND CMAKE_INSTALL_MANIFEST_FILES
                 \"${PA_WINE_DEST_ABS}/x86_64-windows/pipeasio.dll\"
                 \"${PA_WINE_DEST_ABS}/x86_64-unix/pipeasio.dll.so\")")
    endif()
endfunction()

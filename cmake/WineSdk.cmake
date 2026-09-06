# WineSdk.cmake - locate winebuild, winegcc and the Wine SDK headers.
# Sets WINEBUILD, WINEGCC and WINE_INCLUDE_DIRS for every Wine-facing target.

# Debian ships the tools as winebuild-stable / winegcc-stable on PATH (the
# unsuffixed names sit off-PATH in /usr/lib/wine); Debian's wine-development
# branch suffixes -development; WineHQ /opt packages keep unsuffixed tools in
# their own prefix.  Plain names win when several are present (issue #20
# follow-up: Debian could not even configure without hand edits).
set(_wine_tool_paths /usr/lib/wine
    /opt/wine-devel/bin /opt/wine-stable/bin /opt/wine-staging/bin)
find_program(WINEBUILD
    NAMES winebuild winebuild-stable winebuild-development winebuild-staging
    PATHS ${_wine_tool_paths} REQUIRED)
find_program(WINEGCC
    NAMES winegcc winegcc-stable winegcc-development winegcc-staging
    PATHS ${_wine_tool_paths} REQUIRED)

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
# the compiler's own /usr/include; windows.h must be reachable through -I,
# since the PE half is built by a cross compiler.  unixlib.h is checked
# separately: Debian and Ubuntu do not ship it, so a bundled copy stands in.
set(_wine_hdr_checks wine/debug.h windows.h)
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

# Arch and Fedora install unixlib.h beside debug.h (found through the same
# dirs, or the wine/ subdirectory on a Wine-default layout); Debian's
# libwine-dev omits it.  WINE_UNIXLIB_INCLUDE_DIR is what a target adds to
# reach it as <unixlib.h>.
set(WINE_UNIXLIB_INCLUDE_DIR "")
foreach(_d ${WINE_INCLUDE_DIRS} /usr/include/wine)
    if(EXISTS "${_d}/unixlib.h")
        set(WINE_UNIXLIB_INCLUDE_DIR "${_d}")
        break()
    elseif(EXISTS "${_d}/wine/unixlib.h")
        set(WINE_UNIXLIB_INCLUDE_DIR "${_d}/wine")
        break()
    endif()
endforeach()
if(NOT WINE_UNIXLIB_INCLUDE_DIR)
    set(WINE_UNIXLIB_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include/compat")
    message(STATUS "Wine SDK ships no unixlib.h; using the bundled copy")
endif()

# WineIncludes.cmake - locate the Wine SDK include directories.
#
# The driver sources include both Wine-internal headers (`wine/debug.h`,
# `wine/unicode.h`) and Windows SDK headers (`windows.h`, `objbase.h`).
# Those live in different places depending on the distro:
#
#   <prefix>/include/wine/debug.h        -> need <prefix>/include
#   <prefix>/include/wine/windows/...    -> need <prefix>/include/wine/windows
#
# On a system install <prefix>/include is the compiler default, which is why
# the historical list of bare /usr/include/wine* paths worked.  For Wine
# installed under /opt (Fedora wine-devel, winehq packages) the parent
# <prefix>/include must be listed explicitly or `wine/debug.h` is not found
# (issue #14).
#
# The candidate roots can be overridden with PIPEASIO_WINE_PREFIX_ROOTS
# (a ;-list); tests use that to inject a fake prefix.

function(pipeasio_detect_wine_includes out_var)
    if(DEFINED PIPEASIO_WINE_PREFIX_ROOTS)
        # Full override (used by tests): no legacy fallbacks.
        set(_roots ${PIPEASIO_WINE_PREFIX_ROOTS})
        set(_legacy "")
    else()
        set(_roots
            /usr
            /usr/local
            /opt/wine-devel
            /opt/wine-stable
            /opt/wine-staging
            /opt/wine)
        # Legacy locations that are not under one of the roots above.
        set(_legacy
            /usr/include/wine
            /usr/include/wine/windows
            /usr/include/wine/wine
            /usr/include/wine/wine/windows
            /usr/include/wine-development
            /usr/include/wine-development/wine/windows
            /opt/wine-stable/include
            /opt/wine-stable/include/wine/windows
            /opt/wine-staging/include
            /opt/wine-staging/include/wine/windows)
    endif()

    set(_candidates "")
    foreach(_root ${_roots})
        list(APPEND _candidates
            "${_root}/include"
            "${_root}/include/wine"
            "${_root}/include/wine/windows"
            "${_root}/include/wine/wine"
            "${_root}/include/wine/wine/windows")
    endforeach()
    list(APPEND _candidates ${_legacy})

    # Keep only directories that actually hold Wine headers; a bare include/
    # from an unrelated package must not turn into a dangling -I flag.
    set(_found "")
    foreach(_d ${_candidates})
        if(IS_DIRECTORY "${_d}"
           AND (EXISTS "${_d}/wine/debug.h" OR EXISTS "${_d}/debug.h" OR EXISTS "${_d}/windows.h"))
            list(APPEND _found "${_d}")
        endif()
    endforeach()
    if(_found)
        list(REMOVE_DUPLICATES _found)
    endif()
    set(${out_var} "${_found}" PARENT_SCOPE)
endfunction()

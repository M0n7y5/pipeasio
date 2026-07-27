# test_wine_includes.cmake - verify pipeasio_detect_wine_includes() finds the
# Wine SDK layout used by /opt installs (issue #14) and ignores headerless
# include/ dirs.
#
# Run via CTest:
#   cmake -DPIPEASIO_MODULE_DIR=<dir> -DFAKE_WINE_PREFIX=<dir> \
#         -P tests/cmake/test_wine_includes.cmake

if(NOT PIPEASIO_MODULE_DIR OR NOT FAKE_WINE_PREFIX)
    message(FATAL_ERROR "PIPEASIO_MODULE_DIR and FAKE_WINE_PREFIX are required")
endif()

list(APPEND CMAKE_MODULE_PATH "${PIPEASIO_MODULE_DIR}")
include(WineIncludes)

# Fake /opt/wine-devel-style prefix: <prefix>/include/wine/debug.h plus
# <prefix>/include/wine/windows/windows.h.
file(REMOVE_RECURSE "${FAKE_WINE_PREFIX}")
file(MAKE_DIRECTORY "${FAKE_WINE_PREFIX}/include/wine/windows")
file(WRITE "${FAKE_WINE_PREFIX}/include/wine/debug.h" "/* fake wine/debug.h */\n")
file(WRITE "${FAKE_WINE_PREFIX}/include/wine/windows/windows.h" "/* fake windows.h */\n")

set(PIPEASIO_WINE_PREFIX_ROOTS "${FAKE_WINE_PREFIX}")
pipeasio_detect_wine_includes(detected)

foreach(expect
        "${FAKE_WINE_PREFIX}/include"
        "${FAKE_WINE_PREFIX}/include/wine"
        "${FAKE_WINE_PREFIX}/include/wine/windows")
    if(NOT "${expect}" IN_LIST detected)
        message(FATAL_ERROR "missing ${expect} in detected includes: ${detected}")
    endif()
endforeach()

# A prefix whose include/ has no Wine headers must contribute nothing.
file(REMOVE_RECURSE "${FAKE_WINE_PREFIX}-empty")
file(MAKE_DIRECTORY "${FAKE_WINE_PREFIX}-empty/include")
set(PIPEASIO_WINE_PREFIX_ROOTS "${FAKE_WINE_PREFIX}-empty")
pipeasio_detect_wine_includes(detected_empty)
if(detected_empty)
    message(FATAL_ERROR "expected no includes for a headerless prefix, got: ${detected_empty}")
endif()

message(STATUS "wine include detection test passed: ${detected}")

# Wine PE loopback analyzer: plays a frame counter out of PipeASIO's
# output ports and verifies it on the inputs after an external PipeWire
# loopback (see run.sh).  Checks bit-exactness, sample continuity,
# channel mapping, and measured vs reported round-trip latency.

# Same tool-name variants as cmake/WineSdk.cmake (cache-shared when in-tree).
find_program(WINEGCC
    NAMES winegcc winegcc-stable winegcc-development winegcc-staging
    PATHS /usr/lib/wine /opt/wine-devel/bin /opt/wine-stable/bin /opt/wine-staging/bin
    REQUIRED)

# Same Wine SDK include propagation as tests/asio_probe: bare winegcc cannot
# compile windows.h on Debian/WineHQ header layouts.
set(_loop_wine_includes "")
foreach(_dir IN LISTS WINE_INCLUDE_DIRS)
    list(APPEND _loop_wine_includes "-I${_dir}")
endforeach()

set(LOOP_SRC  "${CMAKE_CURRENT_SOURCE_DIR}/asio_loopback.c")
set(LOOP_OUT  "${CMAKE_CURRENT_BINARY_DIR}/asio_loopback.exe.so")

add_custom_command(
    OUTPUT  "${LOOP_OUT}"
    COMMAND ${WINEGCC} -m64 -mno-cygwin -mconsole
                       ${_loop_wine_includes}
                       -O0 -g -Wall -Wextra
                       "${LOOP_SRC}"
                       -lole32
                       -o "${LOOP_OUT}"
    DEPENDS "${LOOP_SRC}"
    COMMENT "winegcc asio_loopback (Wine PE)"
    VERBATIM
)

add_custom_target(asio_loopback ALL DEPENDS "${LOOP_OUT}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/run.sh"
    "${CMAKE_CURRENT_BINARY_DIR}/run.sh"
    @ONLY
)

# Integration test: digital loopback analyzer (bit-exactness, continuity,
# channel mapping, RTL) under Wine against a live PipeWire.  run.sh exits 77
# (CTest SKIP) without wine/pw-cli/pw-link/a daemon/an installed driver.
add_test(NAME asio_loopback COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run.sh")
set_tests_properties(asio_loopback PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 300
    RESOURCE_LOCK wineprefix
    LABELS "integration;wine;pipewire")
if(PIPEASIO_ASAN)
    set_property(TEST asio_loopback APPEND PROPERTY LABELS pipeasio-sanitize)
    set_tests_properties(asio_loopback PROPERTIES FIXTURES_REQUIRED sanitizer_artifacts)
endif()

# Mixer volume: run.sh sets channelVolumes on the node, the analyzer decodes
# with the inverse, so lock proves the driver scaled each channel.
add_test(NAME asio_loopback_gain COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run.sh")
set_tests_properties(asio_loopback_gain PROPERTIES
    ENVIRONMENT "LOOP_GAIN=0.5 0.25;SIZES=128"
    SKIP_RETURN_CODE 77
    TIMEOUT 120
    RESOURCE_LOCK wineprefix
    LABELS "integration;wine;pipewire")

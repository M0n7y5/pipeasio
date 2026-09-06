# Wine PE probe host.

# Same tool-name variants as cmake/WineSdk.cmake (cache-shared when in-tree).
find_program(WINEGCC
    NAMES winegcc winegcc-stable winegcc-development winegcc-staging
    PATHS /usr/lib/wine /opt/wine-devel/bin /opt/wine-stable/bin /opt/wine-staging/bin
    REQUIRED)

# winegcc compiles this host from source, so it needs the same probed Wine SDK
# include dirs the driver objects get: its builtin header path misses layouts
# like Debian's libwine-dev split or a WineHQ /opt SDK ("windows.h: No such
# file or directory").  Empty when the probe used winegcc's own prefix.
set(_probe_wine_includes "")
foreach(_dir IN LISTS WINE_INCLUDE_DIRS)
    list(APPEND _probe_wine_includes "-I${_dir}")
endforeach()

set(PROBE_SRC  "${CMAKE_CURRENT_SOURCE_DIR}/asio_probe.c")
set(PROBE_OUT  "${CMAKE_CURRENT_BINARY_DIR}/asio_probe.exe.so")
set(PROBE_PE   "${CMAKE_CURRENT_BINARY_DIR}/asio_probe.exe")
# The driver with the stop and gate barriers compiled in (asio.c), under its
# own module name so a prefix can point the CLSID at it.
pipeasio_pe_arch_available(x86_64 _have_x86_64)
if(_have_x86_64)
pipeasio_add_pe_driver(
    NAME     pipeasio64_stop_test
    PE_ARCH  x86_64
    DEFINES  PIPEASIO_TEST_STOP_BARRIER PIPEASIO_TEST_GATE_BARRIER
    NO_INSTALL)
endif()


add_custom_command(
    OUTPUT  "${PROBE_OUT}"
    COMMAND ${WINEGCC} -m64 -mno-cygwin -mconsole
                       ${_probe_wine_includes}
                       -O0 -g -Wall -Wextra
                       "${PROBE_SRC}"
                       -lole32
                       -o "${PROBE_OUT}"
    DEPENDS "${PROBE_SRC}"
    COMMENT "winegcc asio_probe (Wine PE)"
    VERBATIM
)

add_custom_target(asio_probe ALL DEPENDS "${PROBE_OUT}")

# Copy runners next to the probe.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/run.sh"
    "${CMAKE_CURRENT_BINARY_DIR}/run.sh"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/gdb.sh"
    "${CMAKE_CURRENT_BINARY_DIR}/gdb.sh"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/run_rt.sh"
    "${CMAKE_CURRENT_BINARY_DIR}/run_rt.sh"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/run_err.sh"
    "${CMAKE_CURRENT_BINARY_DIR}/run_err.sh"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/run_stop_interleave.sh.in"
    "${CMAKE_CURRENT_BINARY_DIR}/run_stop_interleave.sh"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/run_gate_interleave.sh.in"
    "${CMAKE_CURRENT_BINARY_DIR}/run_gate_interleave.sh"
    @ONLY
)

# Integration test: full COM/bufferSwitch path under Wine against a live
# PipeWire.  run.sh exits 77 (CTest SKIP) without wine/pw-cli/a daemon/an
# installed driver.
add_test(NAME asio_probe COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run.sh")
set_tests_properties(asio_probe PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 300
    RESOURCE_LOCK wineprefix
    LABELS "integration;wine;pipewire")
if(PIPEASIO_ASAN)
    set_property(TEST asio_probe APPEND PROPERTY LABELS pipeasio-sanitize)
    set_tests_properties(asio_probe PROPERTIES FIXTURES_REQUIRED sanitizer_artifacts)
endif()

# Three scheduling-topology legs (default / config-on / env-off) in one prefix.
add_test(NAME asio_probe_rt
    COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run_rt.sh"
            "${CMAKE_CURRENT_BINARY_DIR}/run.sh" 2 native)
set_tests_properties(asio_probe_rt PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 600
    RESOURCE_LOCK wineprefix
    LABELS "integration;wine;pipewire;realtime")
if(PIPEASIO_ASAN)
    set_property(TEST asio_probe_rt APPEND PROPERTY LABELS pipeasio-sanitize)
    set_tests_properties(asio_probe_rt PROPERTIES FIXTURES_REQUIRED sanitizer_artifacts)
endif()

add_test(NAME asio_stop_interleave
    COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run_stop_interleave.sh")
set_tests_properties(asio_stop_interleave PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 300
    RESOURCE_LOCK wineprefix
    LABELS "integration;wine;pipewire;lifecycle")
if(PIPEASIO_ASAN)
    set_property(TEST asio_stop_interleave APPEND PROPERTY LABELS pipeasio-sanitize)
    set_tests_properties(asio_stop_interleave PROPERTIES FIXTURES_REQUIRED sanitizer_artifacts)
endif()

add_test(NAME asio_probe_err
    COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run_err.sh"
            "${CMAKE_CURRENT_BINARY_DIR}/run.sh")
set_tests_properties(asio_probe_err PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 300
    RESOURCE_LOCK wineprefix
    LABELS "integration;wine;pipewire;diagnostics")
if(PIPEASIO_ASAN)
    set_property(TEST asio_probe_err APPEND PROPERTY LABELS pipeasio-sanitize)
    set_tests_properties(asio_probe_err PROPERTIES FIXTURES_REQUIRED sanitizer_artifacts)
endif()

# Deterministic method_gate lost-wakeup: a leaver decrements without signaling
# while CreateBuffers drains; the sliced drain must still complete promptly.
add_test(NAME asio_gate_interleave
    COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run_gate_interleave.sh")
set_tests_properties(asio_gate_interleave PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 300
    RESOURCE_LOCK wineprefix
    LABELS "integration;wine;pipewire;lifecycle")
if(PIPEASIO_ASAN)
    set_property(TEST asio_gate_interleave APPEND PROPERTY LABELS pipeasio-sanitize)
    set_tests_properties(asio_gate_interleave PROPERTIES FIXTURES_REQUIRED sanitizer_artifacts)
endif()

# Optional i386 PE probe for WoW64.
if(BUILD_WOW64_32)
    find_program(I686_W64_MINGW_GCC i686-w64-mingw32-gcc)
    if(I686_W64_MINGW_GCC)
        set(PROBE32_OUT "${CMAKE_CURRENT_BINARY_DIR}/asio_probe32.exe")
        add_custom_command(
            OUTPUT  "${PROBE32_OUT}"
            COMMAND ${I686_W64_MINGW_GCC} -mconsole -O0 -g -Wall -Wextra
                               "${PROBE_SRC}"
                               -lole32
                               -o "${PROBE32_OUT}"
            DEPENDS "${PROBE_SRC}"
            COMMENT "mingw asio_probe32.exe (32-bit WoW64 PE host)"
            VERBATIM
        )
        add_custom_target(asio_probe32 ALL DEPENDS "${PROBE32_OUT}")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/run32.sh"
            "${CMAKE_CURRENT_BINARY_DIR}/run32.sh"
            @ONLY
        )
        add_test(NAME asio_probe32 COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run32.sh")
        set_tests_properties(asio_probe32 PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 300
            RESOURCE_LOCK wineprefix
            LABELS "integration;wine;pipewire;wow64")
        if(PIPEASIO_ASAN)
            set_property(TEST asio_probe32 APPEND PROPERTY LABELS pipeasio-sanitize)
            set_tests_properties(asio_probe32 PROPERTIES
                FIXTURES_REQUIRED sanitizer_artifacts)
        endif()
        add_test(NAME asio_probe32_err
            COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run_err.sh"
                    "${CMAKE_CURRENT_BINARY_DIR}/run32.sh")
        set_tests_properties(asio_probe32_err PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 300
            RESOURCE_LOCK wineprefix
            LABELS "integration;wine;pipewire;wow64;diagnostics")
        if(PIPEASIO_ASAN)
            set_property(TEST asio_probe32_err APPEND PROPERTY LABELS pipeasio-sanitize)
            set_tests_properties(asio_probe32_err PROPERTIES
                FIXTURES_REQUIRED sanitizer_artifacts)
        endif()
        add_test(NAME asio_probe32_rt
            COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run_rt.sh"
                    "${CMAKE_CURRENT_BINARY_DIR}/run32.sh" 2 wow64)
        set_tests_properties(asio_probe32_rt PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 600
            RESOURCE_LOCK wineprefix
            LABELS "integration;wine;pipewire;wow64;realtime")
        if(PIPEASIO_ASAN)
            set_property(TEST asio_probe32_rt APPEND PROPERTY LABELS pipeasio-sanitize)
            set_tests_properties(asio_probe32_rt PROPERTIES
                FIXTURES_REQUIRED sanitizer_artifacts)
        endif()
    endif()
endif()

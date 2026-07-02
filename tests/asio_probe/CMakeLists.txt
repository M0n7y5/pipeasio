# Wine PE probe host.

find_program(WINEGCC winegcc REQUIRED)

set(PROBE_SRC  "${CMAKE_CURRENT_SOURCE_DIR}/asio_probe.c")
set(PROBE_OUT  "${CMAKE_CURRENT_BINARY_DIR}/asio_probe.exe.so")
set(PROBE_PE   "${CMAKE_CURRENT_BINARY_DIR}/asio_probe.exe")

add_custom_command(
    OUTPUT  "${PROBE_OUT}"
    COMMAND ${WINEGCC} -m64 -mno-cygwin -mconsole
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

# Integration test: full COM/bufferSwitch path under Wine against a live
# PipeWire.  run.sh exits 77 (CTest SKIP) without wine/pw-cli/a daemon/an
# installed driver.
add_test(NAME asio_probe COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run.sh")
set_tests_properties(asio_probe PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 300
    RESOURCE_LOCK wineprefix
    LABELS "integration;wine;pipewire")

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
    endif()
endif()

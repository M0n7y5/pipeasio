# Wine PE loopback analyzer: plays a frame counter out of PipeASIO's
# output ports and verifies it on the inputs after an external PipeWire
# loopback (see run.sh).  Checks bit-exactness, sample continuity,
# channel mapping, and measured vs reported round-trip latency.

pipeasio_add_pe_program(NAME asio_loopback
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/asio_loopback.c" LIBS ole32)

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

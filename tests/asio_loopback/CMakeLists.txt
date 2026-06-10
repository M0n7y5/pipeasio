# Wine PE loopback analyzer: plays a frame counter out of PipeASIO's
# output ports and verifies it on the inputs after an external PipeWire
# loopback (see run.sh).  Checks bit-exactness, sample continuity,
# channel mapping, and measured vs reported round-trip latency.

find_program(WINEGCC winegcc REQUIRED)

set(LOOP_SRC  "${CMAKE_CURRENT_SOURCE_DIR}/asio_loopback.c")
set(LOOP_OUT  "${CMAKE_CURRENT_BINARY_DIR}/asio_loopback.exe.so")

add_custom_command(
    OUTPUT  "${LOOP_OUT}"
    COMMAND ${WINEGCC} -m64 -mno-cygwin -mconsole
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

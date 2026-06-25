# pw_probe - Linux-native PipeWire-contract conformance test.
#
# Links libpipewire-0.3 directly (no Wine, no ASIO host) and verifies the
# threading + quantum invariants src/audio.c relies on.  Needs a running
# PipeWire daemon at runtime; exits 77 (CTest SKIP) when none is reachable.

add_executable(pw_filter_probe pw_filter_probe.c)
target_link_libraries(pw_filter_probe PRIVATE PkgConfig::PIPEWIRE pthread)
target_compile_options(pw_filter_probe PRIVATE -Wall -Wextra)

add_test(NAME pw_filter_probe COMMAND pw_filter_probe)
set_tests_properties(pw_filter_probe PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    LABELS "integration;pipewire")

add_executable(pw_delivery_probe pw_delivery_probe.c)
target_link_libraries(pw_delivery_probe PRIVATE PkgConfig::PIPEWIRE pthread m)
target_compile_options(pw_delivery_probe PRIVATE -Wall -Wextra)

add_test(NAME pw_delivery_probe COMMAND pw_delivery_probe)
set_tests_properties(pw_delivery_probe PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    LABELS "integration;pipewire")

add_executable(pw_default_probe pw_default_probe.c)
target_link_libraries(pw_default_probe PRIVATE PkgConfig::PIPEWIRE pthread)
target_compile_options(pw_default_probe PRIVATE -Wall -Wextra)

add_test(NAME pw_default_probe COMMAND pw_default_probe)
set_tests_properties(pw_default_probe PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 30
    LABELS "integration;pipewire")

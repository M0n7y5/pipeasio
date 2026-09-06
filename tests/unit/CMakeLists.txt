# Linux-native unit tests for pure-C logic shared with the Wine-side
# driver.  These build with the host compiler (no winegcc) - they only
# include pipeasio_offsets.h and other pure headers.  Run from CTest.

set(PIPEASIO_UNIT_INCLUDES "${CMAKE_SOURCE_DIR}/include")

function(pipeasio_add_unit_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${PIPEASIO_UNIT_INCLUDES})
    target_compile_options(${name} PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>)
    if(PIPEASIO_ASAN)
        target_compile_options(${name} PRIVATE -fsanitize=address -fsanitize=undefined)
        target_link_options(${name} PRIVATE -fsanitize=address -fsanitize=undefined)
    endif()
    set_target_properties(${name} PROPERTIES C_STANDARD 11)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "unit;pipeasio-sanitize")
    if(PIPEASIO_ASAN)
        set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED sanitizer_artifacts)
    endif()
endfunction()

pipeasio_add_unit_test(test_offsets test_offsets.c)
pipeasio_add_unit_test(test_config test_config.c ${CMAKE_SOURCE_DIR}/src/config.c)
find_package(Threads REQUIRED)
pipeasio_add_unit_test(test_admission_gate test_admission_gate.c)
target_link_libraries(test_admission_gate PRIVATE Threads::Threads)
set_tests_properties(test_admission_gate PROPERTIES TIMEOUT 10)
pipeasio_add_unit_test(test_pw_buffer_region test_pw_buffer_region.c)
target_link_libraries(test_pw_buffer_region PRIVATE PkgConfig::PIPEWIRE)
set_tests_properties(test_pw_buffer_region PROPERTIES TIMEOUT 10)

pipeasio_add_unit_test(test_handle_table test_handle_table.c
    ${CMAKE_SOURCE_DIR}/src/unixlib/handle_table.c)
target_link_libraries(test_handle_table PRIVATE Threads::Threads)
set_tests_properties(test_handle_table PROPERTIES TIMEOUT 10)

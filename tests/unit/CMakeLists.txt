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
    set_target_properties(${name} PROPERTIES C_STANDARD 11)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pipeasio_add_unit_test(test_offsets test_offsets.c)
pipeasio_add_unit_test(test_config test_config.c ${CMAKE_SOURCE_DIR}/src/config.c)

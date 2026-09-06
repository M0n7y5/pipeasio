# PE front end + unixlib layout, the one Wine expects from a builtin module
# since Wine 8 and the only one aarch64/arm64ec Wine will load.
#
#   pipeasio_add_unixlib_objects()
#       Compiles the Unix half (PipeWire client) once, as an object library
#       shared by every front end built for this host.
#
#   pipeasio_add_pe_driver(
#       NAME       pipeasio32           # module name; <NAME>.dll and <NAME>.so
#       PE_ARCH    i386                 # Wine arch dir: i386 | x86_64 | aarch64 | arm64ec
#       TRIPLE     i686-w64-mingw32     # mingw cross triple for the PE half
#       WINEBUILD_FLAGS -m32            # how winebuild addresses this arch
#       DEF        src/wow64/pipeasio32.def
#       [OUT_DIR   dir]                 # default CMAKE_BINARY_DIR
#       [TARGET    name]                # CMake target name, default NAME
#       [NO_INSTALL]
#   )
#       Produces <OUT_DIR>/<NAME>.dll (real PE, marked builtin) and
#       <OUT_DIR>/<NAME>.so (the unixlib, linked from the shared objects),
#       installed under lib/wine/<PE_ARCH>-windows and lib/wine/<host>-unix.

set(PIPEASIO_UNIX_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(PIPEASIO_UNIX_ARCH STREQUAL "AMD64")
    set(PIPEASIO_UNIX_ARCH x86_64)
endif()

function(pipeasio_add_unixlib_objects)
    if(TARGET pipeasio_unix_objs)
        return()
    endif()
    # -fno-lto: same winebuild ld -r / .spec export hazard as add_wine_dll (issue #6).
    add_library(pipeasio_unix_objs OBJECT
        src/wow64/audio_unix.c src/wow64/handle_table.c src/audio.c src/config.c)
    set_target_properties(pipeasio_unix_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(pipeasio_unix_objs PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/wow64
        ${PIPEWIRE_INCLUDE_DIRS} ${WINE_INCLUDE_DIRS})
    target_compile_options(pipeasio_unix_objs PRIVATE
        -DPIPEASIO_AUDIO_UNIXLIB -D_REENTRANT
        -Wall -fno-strict-aliasing -Werror=implicit-function-declaration
        -fno-lto
        $<$<CONFIG:Release>:-O2> $<$<CONFIG:Release>:-DNDEBUG>
        $<$<CONFIG:Release>:-fvisibility=hidden>
        $<$<CONFIG:RelWithDebInfo>:-O2> $<$<CONFIG:RelWithDebInfo>:-g>
        $<$<CONFIG:RelWithDebInfo>:-DNDEBUG> $<$<CONFIG:RelWithDebInfo>:-fvisibility=hidden>
        $<$<CONFIG:Debug>:-O0> $<$<CONFIG:Debug>:-g3> $<$<CONFIG:Debug>:-DDEBUG>
        $<$<CONFIG:Debug>:-fno-omit-frame-pointer>)
    if(PIPEASIO_ASAN)
        target_compile_options(pipeasio_unix_objs PRIVATE
            -fsanitize=address -fsanitize=undefined)
    endif()
endfunction()

function(pipeasio_add_pe_driver)
    cmake_parse_arguments(PA "NO_INSTALL" "NAME;PE_ARCH;TRIPLE;DEF;OUT_DIR;TARGET" "WINEBUILD_FLAGS" ${ARGN})
    if(NOT PA_NAME OR NOT PA_PE_ARCH OR NOT PA_TRIPLE OR NOT PA_DEF)
        message(FATAL_ERROR "pipeasio_add_pe_driver: NAME, PE_ARCH, TRIPLE and DEF are required.")
    endif()
    if(NOT PA_OUT_DIR)
        set(PA_OUT_DIR "${CMAKE_BINARY_DIR}")
    endif()
    if(NOT PA_TARGET)
        set(PA_TARGET "${PA_NAME}")
    endif()
    file(MAKE_DIRECTORY "${PA_OUT_DIR}")

    find_program(${PA_TRIPLE}_GCC ${PA_TRIPLE}-gcc)
    find_program(${PA_TRIPLE}_RANLIB ${PA_TRIPLE}-ranlib)
    set(_gcc "${${PA_TRIPLE}_GCC}")
    set(_ranlib "${${PA_TRIPLE}_RANLIB}")
    if(NOT _gcc OR NOT _ranlib)
        message(FATAL_ERROR "${PA_NAME}: ${PA_TRIPLE}-gcc and ${PA_TRIPLE}-ranlib not found.")
    endif()
    set(_imp_dir "${WINE_LIB_ROOT}/${PA_PE_ARCH}-windows")
    if(NOT EXISTS "${_imp_dir}/libwinecrt0.a")
        message(FATAL_ERROR "${PA_NAME}: ${_imp_dir}/libwinecrt0.a not found; "
                            "pass -DWINE_LIB_ROOT=<lib/wine>.")
    endif()

    set(_dll  "${PA_OUT_DIR}/${PA_NAME}.dll")
    set(_so   "${PA_OUT_DIR}/${PA_NAME}.so")
    set(_spec "${CMAKE_SOURCE_DIR}/src/wow64/pipeasio32_unixlib.spec")
    file(GLOB _headers CONFIGURE_DEPENDS
         "${CMAKE_SOURCE_DIR}/include/*.h"
         "${CMAKE_SOURCE_DIR}/src/wow64/*.h")

    # Wine's import libs must be copied, made writable, and re-indexed with the
    # target ranlib before the mingw linker accepts them.
    set(_imp_libs "")
    set(_imp_cmds "")
    set(_imp_deps "")
    foreach(_n winecrt0 ntdll ole32 uuid kernelbase)
        set(_src "${_imp_dir}/lib${_n}.a")
        set(_dst "${PA_OUT_DIR}/lib${_n}-${PA_PE_ARCH}.a")
        list(APPEND _imp_libs "${_dst}")
        list(APPEND _imp_deps "${_src}")
        list(APPEND _imp_cmds
            COMMAND ${CMAKE_COMMAND} -E copy "${_src}" "${_dst}"
            COMMAND chmod u+w "${_dst}"
            COMMAND "${_ranlib}" "${_dst}")
    endforeach()
    add_custom_command(
        OUTPUT  ${_imp_libs}
        ${_imp_cmds}
        DEPENDS ${_imp_deps}
        VERBATIM
        COMMENT "index ${PA_PE_ARCH} Wine import libraries")

    set(_inc -I "${CMAKE_SOURCE_DIR}/include" -I "${CMAKE_SOURCE_DIR}/src/wow64")
    foreach(_d ${WINE_INCLUDE_DIRS})
        list(APPEND _inc -I "${_d}")
    endforeach()
    set(_cflags -Wall -Wextra -Werror=implicit-function-declaration
        $<$<CONFIG:Release>:-O2> $<$<CONFIG:Release>:-DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:-O2> $<$<CONFIG:RelWithDebInfo>:-g>
        $<$<CONFIG:RelWithDebInfo>:-DNDEBUG>
        $<$<CONFIG:Debug>:-O0> $<$<CONFIG:Debug>:-g3> $<$<CONFIG:Debug>:-DDEBUG>
        $<$<CONFIG:Debug>:-fno-omit-frame-pointer>)
    if(PA_PE_ARCH MATCHES "^(i386|x86_64)$")
        list(APPEND _cflags -msse3)
    endif()

    set(_pe_sources
        "${CMAKE_SOURCE_DIR}/src/asio.c"
        "${CMAKE_SOURCE_DIR}/src/main.c"
        "${CMAKE_SOURCE_DIR}/src/regsvr.c"
        "${CMAKE_SOURCE_DIR}/src/config.c"
        "${CMAKE_SOURCE_DIR}/src/wow64/audio_proxy.c")
    # -lmingw32 first: its tlssup.o must own _tls_index, or Wine 11.16+'s winecrt0 tls.o collides.
    add_custom_command(
        OUTPUT  "${_dll}"
        COMMAND "${_gcc}" -shared
                ${_pe_sources}
                "${PA_DEF}"
                -lmingw32
                "${PA_OUT_DIR}/libwinecrt0-${PA_PE_ARCH}.a"
                -DPIPEASIO_WOW64_PE
                ${_inc} ${_cflags} -static -static-libgcc
                -o "${_dll}"
                "${PA_OUT_DIR}/libntdll-${PA_PE_ARCH}.a"
                "${PA_OUT_DIR}/libole32-${PA_PE_ARCH}.a"
                "${PA_OUT_DIR}/libuuid-${PA_PE_ARCH}.a"
                "${PA_OUT_DIR}/libkernelbase-${PA_PE_ARCH}.a"
        COMMAND "${WINEBUILD}" ${PA_WINEBUILD_FLAGS} --builtin "${_dll}"
        DEPENDS ${_pe_sources} ${_headers} "${PA_DEF}" ${_imp_libs}
        VERBATIM COMMAND_EXPAND_LISTS
        COMMENT "mingw ${PA_NAME}.dll (${PA_PE_ARCH} PE front end)")

    pipeasio_add_unixlib_objects()
    set(_sanitize_libs "")
    if(PIPEASIO_ASAN)
        list(APPEND _sanitize_libs -lasan -lubsan)
    endif()
    add_custom_command(
        OUTPUT  "${_so}"
        COMMAND "${WINEGCC}" -shared
                "${_spec}"
                $<TARGET_OBJECTS:pipeasio_unix_objs>
                -lpthread -ldl ${PIPEWIRE_LINK_LIBRARIES} ${PIPEWIRE_LDFLAGS_OTHER}
                ${_sanitize_libs}
                -o "${_so}"
        DEPENDS pipeasio_unix_objs "${_spec}" $<TARGET_OBJECTS:pipeasio_unix_objs>
        VERBATIM COMMAND_EXPAND_LISTS
        COMMENT "winegcc ${PA_NAME}.so (unixlib)")

    add_custom_target(${PA_TARGET}_unix ALL DEPENDS "${_so}")
    add_custom_target(${PA_TARGET} ALL DEPENDS "${_dll}")
    add_dependencies(${PA_TARGET} ${PA_TARGET}_unix)

    if(NOT PA_NO_INSTALL)
        install(FILES "${_dll}" DESTINATION "${PA_WINE_DEST}/${PA_PE_ARCH}-windows")
        install(FILES "${_so}" DESTINATION "${PA_WINE_DEST}/${PIPEASIO_UNIX_ARCH}-unix"
                PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                            GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    endif()
endfunction()

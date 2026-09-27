# PE front end + unixlib: the builtin layout Wine has used since 8 and the
# only one aarch64 Wine loads.  Every driver build goes through it.
#
# The PE half is linked the way Wine links its own modules: winegcc drives a
# cross compiler with Wine's headers and -nodefaultlibs, links winecrt0 and
# the import libraries from lib/wine/<arch>-windows, and marks the builtin.
#
#   pipeasio_add_unixlib_objects()
#       Compiles the Unix half (PipeWire client) once, as an object library
#       shared by every front end built for this host.
#
#   pipeasio_add_pe_driver(
#       NAME       pipeasio64          # module name; <NAME>.dll and <NAME>.so
#       PE_ARCH    x86_64              # i386 | x86_64 | aarch64
#       [ARM64X]                       # aarch64 only: add arm64ec, link a hybrid
#       [TARGET    name]               # CMake target name, default NAME
#       [DEFINES   FOO BAR]            # extra -D for the PE half only
#       [NO_INSTALL]
#   )
#       Produces ${CMAKE_BINARY_DIR}/<PE_ARCH>-windows/<NAME>.dll and
#       ${CMAKE_BINARY_DIR}/<host>-unix/<NAME>.so (one unixlib per NAME, shared
#       by every arch), installed under lib/wine with the same layout.
#
#   pipeasio_pe_arch_available(<arch> <out var>)
#       TRUE when a compiler and Wine's import libraries exist for <arch>.
#
#   pipeasio_arm64x_available(<out var>)
#       TRUE when an aarch64 front end can be linked as ARM64X: clang serves
#       arm64ec, Wine's aarch64-windows libraries carry arm64ec code, and the
#       linker is LLD 20 or newer.  <out var>_REASON says why not.
#
# ARM64X: Wine built for aarch64 and arm64ec installs no arm64ec-windows
# directory.  tools/makedep.c folds the arm64ec objects into the
# aarch64-windows libraries, and winegcc looks for arm64ec libraries there.
# The loader (dlls/ntdll/unix/loader.c) serves an x86_64 process, a host
# under FEX, from aarch64-windows as well, and loads the x64 view only when
# the module there is a hybrid.  So the front end for both kinds of host is
# one pipeasio64.dll with aarch64 and arm64ec code, linked -marm64x.
#
#   pipeasio_add_pe_program(
#       NAME     asio_probe           # <NAME>.exe in the current binary dir
#       SOURCES  asio_probe.c
#       [LIBS    ole32]
#       [OUT     var]                 # receives the .exe path
#   )
#       A console program for this host's arch, linked like Wine's own
#       programs.  winegcc's ELF winelib form is x86-only; this one is not.

set(PIPEASIO_UNIX_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(PIPEASIO_UNIX_ARCH STREQUAL "AMD64")
    set(PIPEASIO_UNIX_ARCH x86_64)
endif()

# Wine's library root, where <arch>-windows/lib*.a live: lib/wine on Arch,
# lib64/wine-wow64/wine on Fedora, lib/<multiarch>/wine on Debian, or the
# prefix's own lib/wine for WineHQ packages.  Probed from winebuild's prefix,
# overridable.
get_filename_component(_wine_prefix "${WINEBUILD}" DIRECTORY)
get_filename_component(_wine_prefix "${_wine_prefix}" DIRECTORY)
set(_wine_lib_candidates
    "${_wine_prefix}/lib/wine" "${_wine_prefix}/lib64/wine"
    "${_wine_prefix}/lib/${PIPEASIO_UNIX_ARCH}-linux-gnu/wine"
    "${_wine_prefix}/lib64/wine-wow64/wine" "${_wine_prefix}/lib/wine-wow64/wine"
    /usr/lib/wine /usr/lib64/wine "/usr/lib/${PIPEASIO_UNIX_ARCH}-linux-gnu/wine"
    /usr/lib64/wine-wow64/wine /usr/lib/wine-wow64/wine)
set(_wine_lib_default "/usr/lib/wine")
foreach(_c ${_wine_lib_candidates})
    if(EXISTS "${_c}/${PIPEASIO_UNIX_ARCH}-windows/libwinecrt0.a")
        set(_wine_lib_default "${_c}")
        break()
    endif()
endforeach()
set(WINE_LIB_ROOT "${_wine_lib_default}" CACHE PATH
    "Wine lib/wine directory holding the <arch>-windows import libraries")
set(PIPEASIO_PE_COMPILER "auto" CACHE STRING
    "Cross compiler for the PE half: auto | gcc (<triple>-gcc) | clang (needs lld)")
set_property(CACHE PIPEASIO_PE_COMPILER PROPERTY STRINGS auto gcc clang)
find_program(PIPEASIO_CLANG NAMES clang)

# The mingw triple a gcc cross compiler answers to; empty where none exists.
function(_pipeasio_pe_triple arch out)
    if(arch STREQUAL "i386")
        set(${out} i686-w64-mingw32 PARENT_SCOPE)
    elseif(arch STREQUAL "x86_64")
        set(${out} x86_64-w64-mingw32 PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# winegcc's target arguments for <arch>, or empty when no compiler serves it.
function(_pipeasio_pe_target_args arch out)
    _pipeasio_pe_triple(${arch} _triple)
    set(_args "")
    if(_triple AND NOT PIPEASIO_PE_COMPILER STREQUAL "clang")
        find_program(PIPEASIO_MINGW_GCC_${arch} ${_triple}-gcc)
        if(PIPEASIO_MINGW_GCC_${arch})
            set(_args -b ${_triple})
        endif()
    endif()
    # No <arch>-windows-gcc exists, so winegcc falls through to clang on PATH
    # (Wine 10 and 11 alike; --cc-cmd only arrived in 11.3).
    if(NOT _args AND PIPEASIO_CLANG AND NOT PIPEASIO_PE_COMPILER STREQUAL "gcc")
        set(_args -b ${arch}-windows)
    endif()
    set(${out} "${_args}" PARENT_SCOPE)
endfunction()

function(pipeasio_pe_arch_available arch out)
    _pipeasio_pe_target_args(${arch} _args)
    if(_args AND EXISTS "${WINE_LIB_ROOT}/${arch}-windows/libwinecrt0.a")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Sets <out> and <out>_REASON, which says why an aarch64 build falls back to
# a plain aarch64 DLL.
function(pipeasio_arm64x_available out)
    set(${out} FALSE PARENT_SCOPE)
    set(${out}_REASON "" PARENT_SCOPE)
    pipeasio_pe_arch_available(aarch64 _have_aarch64)
    _pipeasio_pe_target_args(arm64ec _ec_args)
    if(NOT _have_aarch64 OR NOT _ec_args)
        return()
    endif()
    # winecrt0 defines this only when built for arm64ec (dlls/winecrt0).
    file(STRINGS "${WINE_LIB_ROOT}/aarch64-windows/libwinecrt0.a" _ec
         REGEX "__wine_unix_call_arm64ec" LIMIT_COUNT 1)
    if(NOT _ec)
        set(${out}_REASON "${WINE_LIB_ROOT}/aarch64-windows carries no arm64ec code"
            PARENT_SCOPE)
        return()
    endif()
    # LLD before 20 keeps one symbol table for both halves and rejects the two
    # spec objects winebuild emits as duplicates.  winegcc links through the
    # clang on PATH, so ask that clang which lld it runs.
    execute_process(COMMAND "${PIPEASIO_CLANG}" -print-prog-name=ld.lld
                    OUTPUT_VARIABLE _lld OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${_lld}" --version
                    OUTPUT_VARIABLE _lld_version ERROR_QUIET)
    string(REGEX MATCH "LLD ([0-9]+)" _match "${_lld_version}")
    if(NOT _match OR CMAKE_MATCH_1 LESS 20)
        if(_match)
            set(_found "found LLD ${CMAKE_MATCH_1}")
        else()
            set(_found "${_lld} reports no LLD version")
        endif()
        set(${out}_REASON "linking ARM64X needs LLD 20 or newer, ${_found}" PARENT_SCOPE)
        return()
    endif()
    set(${out} TRUE PARENT_SCOPE)
endfunction()

function(pipeasio_add_unixlib_objects)
    if(TARGET pipeasio_unix_objs)
        return()
    endif()
    # -fno-lto: same winebuild ld -r / .spec export hazard as before (issue #6).
    add_library(pipeasio_unix_objs OBJECT
        src/unixlib/audio_unix.c src/unixlib/handle_table.c src/audio.c src/config.c)
    set_target_properties(pipeasio_unix_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(pipeasio_unix_objs PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/unixlib
        ${PIPEWIRE_INCLUDE_DIRS} ${WINE_INCLUDE_DIRS} ${WINE_UNIXLIB_INCLUDE_DIR})
    target_compile_options(pipeasio_unix_objs PRIVATE
        -D_REENTRANT
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
    cmake_parse_arguments(PA "NO_INSTALL;ARM64X" "NAME;PE_ARCH;TARGET" "DEFINES" ${ARGN})
    if(NOT PA_NAME OR NOT PA_PE_ARCH)
        message(FATAL_ERROR "pipeasio_add_pe_driver: NAME and PE_ARCH are required.")
    endif()
    if(NOT PA_TARGET)
        set(PA_TARGET "${PA_NAME}")
    endif()
    if(PA_ARM64X AND NOT PA_PE_ARCH STREQUAL "aarch64")
        message(FATAL_ERROR "${PA_NAME}: ARM64X needs PE_ARCH aarch64.")
    endif()
    _pipeasio_pe_target_args(${PA_PE_ARCH} _target_args)
    if(NOT _target_args)
        message(FATAL_ERROR "${PA_NAME}: no cross compiler for ${PA_PE_ARCH} "
                            "(install the mingw-w64 gcc for x86 targets, or clang and lld).")
    endif()
    set(_imp_dir "${WINE_LIB_ROOT}/${PA_PE_ARCH}-windows")
    if(NOT EXISTS "${_imp_dir}/libwinecrt0.a")
        message(FATAL_ERROR "${PA_NAME}: ${_imp_dir}/libwinecrt0.a not found; "
                            "pass -DWINE_LIB_ROOT=<lib/wine>.")
    endif()

    set(_pe_dir "${CMAKE_BINARY_DIR}/${PA_PE_ARCH}-windows")
    set(_so_dir "${CMAKE_BINARY_DIR}/${PIPEASIO_UNIX_ARCH}-unix")
    file(MAKE_DIRECTORY "${_pe_dir}" "${_so_dir}")
    set(_dll  "${_pe_dir}/${PA_NAME}.dll")
    # The export directory, and so the unixlib the loader pairs with the PE, is
    # named after the spec file: it has to carry the module name.
    set(_spec "${_pe_dir}/${PA_NAME}.spec")
    configure_file("${CMAKE_SOURCE_DIR}/src/pipeasio.spec" "${_spec}" COPYONLY)
    file(GLOB _headers CONFIGURE_DEPENDS
         "${CMAKE_SOURCE_DIR}/include/*.h"
         "${CMAKE_SOURCE_DIR}/src/unixlib/*.h")
    set(_pe_sources
        "${CMAKE_SOURCE_DIR}/src/asio.c"
        "${CMAKE_SOURCE_DIR}/src/main.c"
        "${CMAKE_SOURCE_DIR}/src/regsvr.c"
        "${CMAKE_SOURCE_DIR}/src/config.c"
        "${CMAKE_SOURCE_DIR}/src/unixlib/audio_proxy.c")

    set(_cflags -Wall -Wextra -Werror=implicit-function-declaration
        $<$<CONFIG:Release>:-O2> $<$<CONFIG:Release>:-DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:-O2> $<$<CONFIG:RelWithDebInfo>:-g>
        $<$<CONFIG:RelWithDebInfo>:-DNDEBUG>
        $<$<CONFIG:Debug>:-O0> $<$<CONFIG:Debug>:-g3> $<$<CONFIG:Debug>:-DDEBUG>
        $<$<CONFIG:Debug>:-fno-omit-frame-pointer>)
    if(PA_PE_ARCH MATCHES "^(i386|x86_64)$")
        list(APPEND _cflags -msse3) # the pump thread's FTZ/DAZ intrinsics
    endif()
    foreach(_d ${PA_DEFINES})
        list(APPEND _cflags "-D${_d}")
    endforeach()
    # winegcc adds Wine's windows/ and msvcrt/ headers itself.
    set(_inc -I "${CMAKE_SOURCE_DIR}/include" -I "${CMAKE_SOURCE_DIR}/src/unixlib"
             -I "${WINE_UNIXLIB_INCLUDE_DIR}")

    # winegcc, not a bare <triple>-gcc, has to drive this link on every arch
    # (#27).  winegcc passes -nodefaultlibs -nostartfiles, so mingw's
    # dllcrt2.o stays out and cannot drag in libmingw32.a(tlssup.o), whose
    # _tls_used/_tls_start/_tls_end/_tls_index/__xl_a/__xl_z duplicate
    # winecrt0's tls.o; and winebuild's spec object carries the CRT$XI/XC/XT
    # brackets that winecrt0's crt_dllmain.o wants, which mingw's cinitexe.o
    # has no CRT$XT for.  A direct mingw link needs both worked around and
    # broke i386 on mingw-w64-crt 14 / gcc 16.
    set(_libs -L "${_imp_dir}" -lole32 -luuid -luser32 -lkernelbase -Wl,--wine-builtin)
    if(NOT PA_ARM64X)
        add_custom_command(
            OUTPUT  "${_dll}"
            COMMAND "${WINEGCC}" ${_target_args} -shared "${_spec}"
                    ${_pe_sources}
                    -DPIPEASIO_PE ${_inc} ${_cflags}
                    ${_libs}
                    -o "${_dll}"
            DEPENDS ${_pe_sources} ${_headers} "${_spec}"
            VERBATIM COMMAND_EXPAND_LISTS
            COMMENT "winegcc ${PA_NAME}.dll (${PA_PE_ARCH} PE front end)")
    else()
        # One winegcc call targets one arch, so compile each half on its own,
        # then link both object sets with -marm64x, which also has winebuild
        # emit the spec for both views.
        set(_objs "")
        foreach(_arch aarch64 arm64ec)
            _pipeasio_pe_target_args(${_arch} _arch_args)
            set(_obj_dir "${CMAKE_CURRENT_BINARY_DIR}/${PA_TARGET}.dir/${_arch}")
            file(MAKE_DIRECTORY "${_obj_dir}")
            set(_arch_objs "")
            foreach(_src ${_pe_sources})
                get_filename_component(_base "${_src}" NAME_WE)
                list(APPEND _arch_objs "${_obj_dir}/${_base}.o")
            endforeach()
            add_custom_command(
                OUTPUT  ${_arch_objs}
                COMMAND "${WINEGCC}" ${_arch_args} -c ${_pe_sources}
                        -DPIPEASIO_PE ${_inc} ${_cflags}
                WORKING_DIRECTORY "${_obj_dir}"
                DEPENDS ${_pe_sources} ${_headers}
                VERBATIM COMMAND_EXPAND_LISTS
                COMMENT "winegcc -c ${PA_NAME} (${_arch} half)")
            list(APPEND _objs ${_arch_objs})
        endforeach()
        # Linked as makedep links Wine's own hybrids: the arm64ec target with
        # -marm64x and --wine-objdir.  Before Wine 11.17 winegcc takes the
        # first spec object's arch from -b, so an aarch64 target gets two
        # ARM64 spec objects; and outside --wine-objdir it looks up its lib
        # dir, which asserts on arm64ec (fixed by Wine commit d8bb13b7).
        # --wine-objdir also drops winegcc's default libraries and its
        # winebuild lookup, so both are given here.  On 11.18 the result is
        # byte-identical to a plain winegcc link.
        _pipeasio_pe_target_args(arm64ec _ec_args)
        set(_default_libs -ladvapi32 -luser32 -lwinecrt0)
        if(EXISTS "${_imp_dir}/libcompiler-rt.a")
            list(APPEND _default_libs -lcompiler-rt)
        endif()
        list(APPEND _default_libs -lucrtbase -lkernel32 -lntdll)
        add_custom_command(
            OUTPUT  "${_dll}"
            COMMAND "${WINEGCC}" --wine-objdir "${CMAKE_CURRENT_BINARY_DIR}"
                    --winebuild "${WINEBUILD}" ${_ec_args} -marm64x -shared "${_spec}"
                    ${_objs} ${_libs} ${_default_libs} -o "${_dll}"
            DEPENDS ${_objs} "${_spec}"
            VERBATIM COMMAND_EXPAND_LISTS
            COMMENT "winegcc ${PA_NAME}.dll (ARM64X: aarch64 + arm64ec PE front end)")
    endif()

    pipeasio_add_unixlib_objects()
    # A unixlib is a plain ELF shared object the loader dlopens for
    # __wine_unix_call_funcs; Wine links its own with the host compiler, not
    # winegcc, whose spec path is x86-only.
    if(NOT TARGET ${PA_NAME}_unix)
    add_library(${PA_NAME}_unix SHARED $<TARGET_OBJECTS:pipeasio_unix_objs>)
    set_target_properties(${PA_NAME}_unix PROPERTIES
        OUTPUT_NAME "${PA_NAME}" PREFIX "" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${_so_dir}")
    target_link_options(${PA_NAME}_unix PRIVATE -Wl,-Bsymbolic -Wl,-z,defs)
    target_link_libraries(${PA_NAME}_unix PRIVATE
        ${PIPEWIRE_LINK_LIBRARIES} ${PIPEWIRE_LDFLAGS_OTHER} pthread dl m)
    if(PIPEASIO_ASAN)
        target_link_options(${PA_NAME}_unix PRIVATE
            -fsanitize=address -fsanitize=undefined)
    endif()
    if(NOT PA_NO_INSTALL)
        install(TARGETS ${PA_NAME}_unix
                LIBRARY DESTINATION "${PA_WINE_DEST}/${PIPEASIO_UNIX_ARCH}-unix")
    endif()
    endif()

    add_custom_target(${PA_TARGET} ALL DEPENDS "${_dll}")
    add_dependencies(${PA_TARGET} ${PA_NAME}_unix)
    if(NOT PA_NO_INSTALL)
        install(FILES "${_dll}" DESTINATION "${PA_WINE_DEST}/${PA_PE_ARCH}-windows")
    endif()
endfunction()

function(pipeasio_add_pe_program)
    cmake_parse_arguments(PA "" "NAME;OUT" "SOURCES;LIBS" ${ARGN})
    if(NOT PA_NAME OR NOT PA_SOURCES)
        message(FATAL_ERROR "pipeasio_add_pe_program: NAME and SOURCES are required.")
    endif()
    _pipeasio_pe_target_args(${PIPEASIO_UNIX_ARCH} _target_args)
    set(_imp_dir "${WINE_LIB_ROOT}/${PIPEASIO_UNIX_ARCH}-windows")
    if(NOT _target_args OR NOT EXISTS "${_imp_dir}/libwinecrt0.a")
        message(FATAL_ERROR "${PA_NAME}: no PE toolchain for ${PIPEASIO_UNIX_ARCH} "
                            "(a cross compiler and ${_imp_dir}/libwinecrt0.a).")
    endif()
    set(_exe "${CMAKE_CURRENT_BINARY_DIR}/${PA_NAME}.exe")
    set(_libs "")
    foreach(_l ${PA_LIBS})
        list(APPEND _libs "-l${_l}")
    endforeach()
    add_custom_command(
        OUTPUT  "${_exe}"
        COMMAND "${WINEGCC}" ${_target_args} -mconsole
                ${PA_SOURCES} -O0 -g -Wall -Wextra
                -L "${_imp_dir}" ${_libs}
                -o "${_exe}"
        DEPENDS ${PA_SOURCES}
        VERBATIM COMMAND_EXPAND_LISTS
        COMMENT "winegcc ${PA_NAME}.exe (${PIPEASIO_UNIX_ARCH} PE host)")
    add_custom_target(${PA_NAME} ALL DEPENDS "${_exe}")
    if(PA_OUT)
        set(${PA_OUT} "${_exe}" PARENT_SCOPE)
    endif()
endfunction()

set(PIPEASIO_CHECK_EXECUTABLE "" CACHE FILEPATH "Prebuilt installation probe for manager-only packaging")
set(PIPEASIO_CHECK32_EXECUTABLE "" CACHE FILEPATH "Prebuilt 32-bit installation probe for manager-only packaging")

if(BUILD_MANAGER)
    find_package(Qt6 REQUIRED COMPONENTS Core Network)
    find_package(yaml-cpp REQUIRED)
    find_package(LibArchive REQUIRED)
    find_package(ZLIB REQUIRED)
    add_library(pipeasio_manager_core STATIC Common.cpp Launchers.cpp Releases.cpp Installer.cpp)
    target_include_directories(pipeasio_manager_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(pipeasio_manager_core PUBLIC cxx_std_17)
    target_compile_definitions(pipeasio_manager_core PRIVATE
        PIPEASIO_MANAGER_BUILD_PROBE_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(pipeasio_manager_core PUBLIC Qt6::Core Qt6::Network
        ${YAML_CPP_LIBRARIES} LibArchive::LibArchive ZLIB::ZLIB)
    add_executable(pipeasio-manage main.cpp)
    target_include_directories(pipeasio-manage PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(pipeasio-manage PRIVATE pipeasio_manager_core)
    if(PIPEASIO_ASAN)
        target_compile_options(pipeasio_manager_core PUBLIC
            -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(pipeasio_manager_core PUBLIC -fsanitize=address -fsanitize=undefined)
    endif()
    install(TARGETS pipeasio-manage RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Manager)
endif()

if(BUILD_DRIVER)
    pipeasio_add_pe_program(NAME pipeasio-check
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/check.c LIBS ole32 advapi32 shell32
        OUT PIPEASIO_CHECK_EXECUTABLE)
    if(BUILD_WOW64_32)
        pipeasio_pe_arch_available(i386 _have_check32)
        if(_have_check32)
            set(_host_arch "${PIPEASIO_UNIX_ARCH}")
            set(PIPEASIO_UNIX_ARCH i386)
            pipeasio_add_pe_program(NAME pipeasio-check32
                SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/check.c LIBS ole32 advapi32 shell32
                OUT PIPEASIO_CHECK32_EXECUTABLE)
            set(PIPEASIO_UNIX_ARCH "${_host_arch}")
        endif()
    endif()
endif()
foreach(_bits "" "32")
    if(PIPEASIO_CHECK${_bits}_EXECUTABLE)
        install(FILES "${PIPEASIO_CHECK${_bits}_EXECUTABLE}"
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pipeasio/manager
                RENAME pipeasio-check${_bits}.exe COMPONENT Manager)
    endif()
endforeach()

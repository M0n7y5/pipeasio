find_package(Qt6 COMPONENTS Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)

add_executable(test_panel
    test_panel.cpp
    ${CMAKE_SOURCE_DIR}/gui/Config.cpp
    ${CMAKE_SOURCE_DIR}/gui/DeviceEnumerator.cpp
    ${CMAKE_SOURCE_DIR}/gui/PipeWireMonitor.cpp
    ${CMAKE_SOURCE_DIR}/gui/ProfilerParse.cpp
    ${CMAKE_SOURCE_DIR}/gui/SettingsDialog.cpp
    ${CMAKE_SOURCE_DIR}/gui/LoadHistogram.cpp
    ${CMAKE_SOURCE_DIR}/src/config.c)
target_include_directories(test_panel PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/gui)
target_compile_features(test_panel PRIVATE cxx_std_17)
target_link_libraries(test_panel PRIVATE Qt6::Widgets PkgConfig::PIPEWIRE)
add_test(NAME test_panel COMMAND test_panel)
set_tests_properties(test_panel PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(PIPEASIO_ASAN)
    target_compile_options(test_panel PRIVATE
        -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(test_panel PRIVATE -fsanitize=address -fsanitize=undefined)
    set_property(TEST test_panel APPEND PROPERTY LABELS pipeasio-sanitize)
    set_tests_properties(test_panel PROPERTIES FIXTURES_REQUIRED sanitizer_artifacts)
endif()

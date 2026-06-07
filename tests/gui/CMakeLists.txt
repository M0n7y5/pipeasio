# Qt panel unit tests: pure parse/serialize functions plus a cross-language
# round-trip (panel writes the INI, the driver's C reader parses it).  Headless
# — no GUI is constructed — so it runs under CTest with no display.
find_package(Qt6 COMPONENTS Core REQUIRED)
set(CMAKE_AUTOMOC ON)

add_executable(test_panel
    test_panel.cpp
    ${CMAKE_SOURCE_DIR}/gui/Config.cpp
    ${CMAKE_SOURCE_DIR}/gui/DeviceEnumerator.cpp
    ${CMAKE_SOURCE_DIR}/gui/PipeWireMonitor.cpp
    ${CMAKE_SOURCE_DIR}/src/config.c)
target_include_directories(test_panel PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/gui)
target_compile_features(test_panel PRIVATE cxx_std_17)
target_link_libraries(test_panel PRIVATE Qt6::Core)
add_test(NAME test_panel COMMAND test_panel)

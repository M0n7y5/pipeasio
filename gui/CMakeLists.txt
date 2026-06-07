find_package(Qt6 COMPONENTS Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)
add_executable(pipeasio-settings
    main.cpp SettingsDialog.cpp Config.cpp DeviceEnumerator.cpp PipeWireMonitor.cpp)
target_include_directories(pipeasio-settings PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(pipeasio-settings PRIVATE cxx_std_17)
target_link_libraries(pipeasio-settings PRIVATE Qt6::Widgets)
install(TARGETS pipeasio-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

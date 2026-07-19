find_package(Qt6 COMPONENTS Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)
add_executable(pipeasio-settings
    main.cpp SettingsDialog.cpp Config.cpp DeviceEnumerator.cpp PipeWireMonitor.cpp
    LoadHistogram.cpp)
target_include_directories(pipeasio-settings PRIVATE ${CMAKE_SOURCE_DIR}/include
                           ${CMAKE_BINARY_DIR}/generated)
add_dependencies(pipeasio-settings build_info)
set_source_files_properties(SettingsDialog.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/generated/build_info.h")
target_compile_features(pipeasio-settings PRIVATE cxx_std_17)
target_link_libraries(pipeasio-settings PRIVATE Qt6::Widgets)
install(TARGETS pipeasio-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Desktop integration: menu entry + scalable icon (single source of truth is
# the website icon in docs/).  Icon name "pipeasio" matches Icon= in the
# .desktop file and QIcon::fromTheme in main.cpp.
install(FILES pipeasio-settings.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES ${CMAKE_SOURCE_DIR}/docs/icon.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
        RENAME pipeasio.svg)

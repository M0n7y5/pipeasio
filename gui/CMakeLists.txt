find_package(Qt6 COMPONENTS Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)
add_executable(pipeasio-settings
    main.cpp SettingsDialog.cpp Config.cpp DeviceEnumerator.cpp PipeWireMonitor.cpp
    LoadHistogram.cpp ${CMAKE_SOURCE_DIR}/src/config.c)
target_include_directories(pipeasio-settings PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(pipeasio-settings PRIVATE cxx_std_17)
target_link_libraries(pipeasio-settings PRIVATE Qt6::Widgets)
if(PIPEASIO_ASAN)
    target_compile_options(pipeasio-settings PRIVATE
        -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(pipeasio-settings PRIVATE -fsanitize=address -fsanitize=undefined)
endif()
install(TARGETS pipeasio-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Desktop integration: menu entry + scalable icon (single source of truth is
# the website icon in docs/).  Icon name "pipeasio" matches Icon= in the
# .desktop file and QIcon::fromTheme in main.cpp.
install(FILES pipeasio-settings.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES ${CMAKE_SOURCE_DIR}/docs/icon.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
        RENAME pipeasio.svg)

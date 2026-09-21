find_package(Qt6 COMPONENTS Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)
add_executable(pipeasio-settings
    main.cpp SettingsDialog.cpp InstallationsTab.cpp Config.cpp DeviceEnumerator.cpp PipeWireMonitor.cpp
    ProfilerParse.cpp LoadHistogram.cpp ${CMAKE_SOURCE_DIR}/src/config.c)
target_include_directories(pipeasio-settings PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(pipeasio-settings PRIVATE cxx_std_17)
target_compile_definitions(pipeasio-settings PRIVATE
    PIPEASIO_MANAGER_SOURCE_BACKEND="$<TARGET_FILE:pipeasio-manage>")
target_link_libraries(pipeasio-settings PRIVATE Qt6::Widgets PkgConfig::PIPEWIRE)
add_dependencies(pipeasio-settings pipeasio-manage)
if(PIPEASIO_ASAN)
    target_compile_options(pipeasio-settings PRIVATE
        -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(pipeasio-settings PRIVATE -fsanitize=address -fsanitize=undefined)
endif()
install(TARGETS pipeasio-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT Manager)

install(FILES pipeasio-settings.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications COMPONENT Manager)
install(FILES ${CMAKE_SOURCE_DIR}/docs/icon.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
        RENAME pipeasio.svg COMPONENT Manager)

if("$ENV{PIPEASIO_TEST_STRICT}" STREQUAL "1")
    message(FATAL_ERROR "Qt6 Widgets and a C++ compiler are required for strict panel tests")
endif()
message(STATUS "SKIP: Qt6 Widgets or a C++ compiler is unavailable")

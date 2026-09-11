cmake_minimum_required(VERSION 3.16)
project(esp-wifi-agent
        VERSION 0.1.0
        DESCRIPTION "ESP32 extender WiFi handoff daemon"
        LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

option(BUILD_TESTS   "Build unit tests"        ON)
option(ENABLE_INSTALL "Install binary + units" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBNM REQUIRED IMPORTED_TARGET libnm)
pkg_check_modules(GIO  REQUIRED IMPORTED_TARGET gio-2.0)
find_package(Threads REQUIRED)

# Pure-C core (decision engine + state persistence) - no glib/libnm, so it
# is trivially testable and cross-compilable without a target sysroot.
add_library(espcore STATIC
    src/decision.c
    src/state.c)
target_include_directories(espcore PUBLIC include)

add_executable(esp-wifi-agent
    src/main.c
    src/config.c
    src/monitor.c
    src/usb_monitor.c)
target_link_libraries(esp-wifi-agent PRIVATE
    espcore
    PkgConfig::LIBNM
    PkgConfig::GIO
    Threads::Threads)
target_include_directories(esp-wifi-agent PRIVATE include)

if(ENABLE_INSTALL)
    install(TARGETS esp-wifi-agent RUNTIME DESTINATION bin)
    install(FILES etc/esp-wifi-agent.conf
            DESTINATION ${CMAKE_INSTALL_SYSCONFDIR})
    install(FILES systemd/esp-wifi-agent.service
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_executable(test_decision tests/test_decision.c src/decision.c)
    target_include_directories(test_decision PRIVATE include)
    add_test(NAME esp_decision COMMAND test_decision)
endif()
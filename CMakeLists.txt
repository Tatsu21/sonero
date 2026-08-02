cmake_minimum_required(VERSION 3.21)

project(LinuxSonar
    VERSION 0.1.0
    DESCRIPTION "Native Linux audio mixer & router inspired by SteelSeries Sonar"
    HOMEPAGE_URL "https://github.com/"
    LANGUAGES CXX)

# --- Global C++ settings -----------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized-with-symbols build when the user does not pick one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# Export a compile database for clangd / tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Qt meta-object handling. AUTOMOC is enough for Stage 1 (no .ui / .qrc yet).
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# --- Dependencies ------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Widgets DBus Network)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

# spdlog is optional: when present we use it, otherwise the logging facade
# falls back to a small stderr/stdout writer (see core/Log.cpp).
find_package(spdlog QUIET)

# --- Core library (Qt-free): audio backend + cross-cutting utilities ---------
add_library(linuxsonar_core STATIC
    core/Log.h
    core/Log.cpp
    audio/IAudioBackend.h
    audio/IAppRouter.h
    audio/IChannelController.h
    audio/IEqualizerController.h
    audio/PipeWireManager.h
    audio/PipeWireManager.cpp
    audio/DeviceFormat.h
    audio/DeviceFormat.cpp
    audio/Channel.h
    audio/IMixer.h
    audio/Mixer.h
    audio/Mixer.cpp
    dsp/Equalizer.h
    dsp/Equalizer.cpp
    hid/SteelSeriesDevice.h
    hid/SteelSeriesDevice.cpp)

target_include_directories(linuxsonar_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(linuxsonar_core PUBLIC PkgConfig::PIPEWIRE)
target_compile_definitions(linuxsonar_core PUBLIC SONAR_VERSION="${PROJECT_VERSION}")
target_compile_features(linuxsonar_core PUBLIC cxx_std_20)

if(spdlog_FOUND)
    target_link_libraries(linuxsonar_core PRIVATE spdlog::spdlog)
    target_compile_definitions(linuxsonar_core PRIVATE SONAR_HAVE_SPDLOG=1)
    message(STATUS "LinuxSonar: logging via spdlog")
else()
    message(STATUS "LinuxSonar: spdlog not found, using built-in fallback logger")
endif()

# --- Application executable (Qt UI) ------------------------------------------
qt_add_executable(LinuxSonar
    app/main.cpp
    app/Application.h
    app/Application.cpp
    app/SingleInstance.h
    app/SingleInstance.cpp
    app/AutoStart.h
    app/AutoStart.cpp
    app/SystemSetup.h
    app/SystemSetup.cpp
    config/SettingsStore.h
    config/SettingsStore.cpp
    ui/Theme.h
    ui/Theme.cpp
    ui/MainWindow.h
    ui/MainWindow.cpp
    ui/MixerPage.h
    ui/MixerPage.cpp
    ui/EqualizerPage.h
    ui/EqualizerPage.cpp
    ui/MicrophonePage.h
    ui/MicrophonePage.cpp
    ui/DevicesPage.h
    ui/DevicesPage.cpp
    ui/SettingsPage.h
    ui/SettingsPage.cpp
    ui/Notifier.h
    ui/Notifier.cpp
    ui/EqPresetStore.h
    ui/EqPresetStore.cpp
    ui/widgets/EqCurve.h
    ui/widgets/EqCurve.cpp
    ui/widgets/VuMeter.h
    ui/widgets/VuMeter.cpp
    ui/widgets/ChannelStrip.h
    ui/widgets/ChannelStrip.cpp
    ui/widgets/AppChip.h
    ui/widgets/AppChip.cpp
    ui/widgets/FlowLayout.h
    ui/widgets/FlowLayout.cpp)

target_link_libraries(LinuxSonar PRIVATE linuxsonar_core Qt6::Widgets Qt6::DBus Qt6::Network)

# Lets a build-tree run find packaged resources (icons, udev rule) without being
# installed; packaged builds locate them under the install prefix instead.
target_compile_definitions(LinuxSonar PRIVATE
    SONAR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# --- HID probe (dev tool for the SteelSeries base station) -------------------
add_executable(hidprobe hid/probe_main.cpp)
target_link_libraries(hidprobe PRIVATE linuxsonar_core)

# --- Installation (also what the AppImage AppDir is built from) ---------------
include(GNUInstallDirs)

install(TARGETS LinuxSonar RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES packaging/LinuxSonar.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

foreach(icon_size 32 48 64 128 256)
    install(FILES packaging/icons/linuxsonar-${icon_size}.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps
        RENAME LinuxSonar.png)
    # Second copy kept under a stable name so the running app can install icons
    # into the user's home for desktop integration (portable AppImage builds).
    install(FILES packaging/icons/linuxsonar-${icon_size}.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/linuxsonar/icons)
endforeach()

# Shipped for reference; writing HID to the SteelSeries dock needs this rule
# installed into /etc/udev/rules.d as root (see docs) — an AppImage cannot do it.
install(FILES packaging/udev/70-linuxsonar-steelseries.rules
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linuxsonar/udev)

# --- Tests -------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

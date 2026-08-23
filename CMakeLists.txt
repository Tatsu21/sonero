cmake_minimum_required(VERSION 3.21)

project(Sonero
    VERSION 0.1.2
    DESCRIPTION "Per-application audio mixer, router and equalizer for PipeWire"
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
# Network provides QLocalServer/QLocalSocket, used for the single-instance guard.
find_package(Qt6 REQUIRED COMPONENTS Widgets DBus Network)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

# spdlog is optional: when present we use it, otherwise the logging facade falls
# back to a small stderr/stdout writer (see core/Log.cpp).
#
# Packaged builds turn it OFF: libspdlog's package name embeds both its own and
# libfmt's ABI version (libspdlog1.15-fmt10), which differs on every distribution
# and makes a .deb refuse to install elsewhere. The built-in logger costs nothing.
option(SONAR_USE_SPDLOG "Use spdlog for logging when it is available" ON)
if(SONAR_USE_SPDLOG)
    find_package(spdlog QUIET)
endif()

# --- Core library (Qt-free): audio backend + cross-cutting utilities ---------
add_library(sonero_core STATIC
    core/Format.h
    core/Format.cpp
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
    dsp/AutoGain.h
    dsp/AutoGain.cpp
    hid/SteelSeriesDevice.h
    hid/SteelSeriesDevice.cpp)

target_include_directories(sonero_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sonero_core PUBLIC PkgConfig::PIPEWIRE)
target_compile_definitions(sonero_core PUBLIC SONAR_VERSION="${PROJECT_VERSION}")
target_compile_features(sonero_core PUBLIC cxx_std_20)

if(SONAR_USE_SPDLOG AND spdlog_FOUND)
    target_link_libraries(sonero_core PRIVATE spdlog::spdlog)
    target_compile_definitions(sonero_core PRIVATE SONAR_HAVE_SPDLOG=1)
    message(STATUS "Sonero: logging via spdlog")
else()
    message(STATUS "Sonero: using the built-in logger (no spdlog dependency)")
endif()

# --- Application executable (Qt UI) ------------------------------------------
qt_add_executable(Sonero
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

target_link_libraries(Sonero PRIVATE sonero_core Qt6::Widgets Qt6::DBus Qt6::Network)

# Lets a build-tree run find packaged resources (icons, udev rule) without being
# installed; packaged builds locate them under the install prefix instead.
target_compile_definitions(Sonero PRIVATE
    SONAR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# --- HID probe (dev tool for the SteelSeries base station) -------------------
add_executable(hidprobe hid/probe_main.cpp)
target_link_libraries(hidprobe PRIVATE sonero_core)

# --- Installation (also what the AppImage AppDir is built from) ---------------
include(GNUInstallDirs)

install(TARGETS Sonero RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES packaging/Sonero.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

foreach(icon_size 32 48 64 128 256)
    install(FILES packaging/icons/sonero-${icon_size}.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps
        RENAME Sonero.png)
    # Second copy kept under a stable name so the running app can install icons
    # into the user's home for desktop integration (portable AppImage builds).
    install(FILES packaging/icons/sonero-${icon_size}.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/sonero/icons)
endforeach()

# Panel icon for system trays (Cinnamon, KDE, XFCE). Separate from the app icon:
# it is drawn at 16-24px over an unknown panel colour, so it drops the background
# tile and uses heavier strokes.
foreach(icon_size 16 22 24 32 48)
    install(FILES packaging/icons/sonero-tray-${icon_size}.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps
        RENAME Sonero-tray.png)
    install(FILES packaging/icons/sonero-tray-${icon_size}.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/sonero/icons)
endforeach()

# The GitHub mark shown in the footer.
install(FILES packaging/icons/github.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/sonero/icons)

# Licensing, in the two places a user looks for it: the Debian convention
# (/usr/share/doc/<package>/copyright, machine-readable DEP-5) and the plain
# MIT text next to it for anyone unpacking an AppImage or a tarball.
install(FILES packaging/deb/copyright
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sonero)
install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/sonero)

# Two copies of the udev rule, for the two ways Sonero is delivered:
#  - /lib/udev/rules.d is where a package's rules live, so installing the .deb
#    grants SteelSeries HID access immediately (postinst reloads udev).
#  - the share/ copy is a payload the portable AppImage can offer to install
#    itself, since it cannot write outside the user's home.
install(FILES packaging/udev/70-sonero-steelseries.rules
    DESTINATION lib/udev/rules.d)
install(FILES packaging/udev/70-sonero-steelseries.rules
    DESTINATION ${CMAKE_INSTALL_DATADIR}/sonero/udev)

# --- Debian package (cpack -G DEB) -------------------------------------------
set(CPACK_PACKAGE_NAME "sonero")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Per-application audio mixer, router and equalizer for PipeWire")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

set(SONAR_DEB_MAINTAINER "Sonero <noreply@localhost>"
    CACHE STRING "Maintainer field of the .deb package")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${SONAR_DEB_MAINTAINER}")
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

# Derive the exact library dependencies from the linked binary rather than
# guessing version ranges by hand.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Not linked against, so shlibdeps cannot see them: the Qt platform plugins are
# dlopen()ed, and PipeWire is a running service rather than a library call.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pipewire (>= 0.3), wireplumber, qt6-wayland")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "pipewire-audio")

# The project used to be called LinuxSonar. Conflicts+Replaces makes apt remove
# that package as part of installing this one, instead of leaving two copies with
# duplicate menu entries, udev rules and autostart files fighting each other.
set(CPACK_DEBIAN_PACKAGE_CONFLICTS "linuxsonar")
set(CPACK_DEBIAN_PACKAGE_REPLACES "linuxsonar")
set(CPACK_DEBIAN_PACKAGE_PROVIDES "linuxsonar")

set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/deb/postrm")

include(CPack)

# --- Tests -------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

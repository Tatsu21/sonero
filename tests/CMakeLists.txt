find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(PipeWireManagerTest PipeWireManagerTest.cpp)
target_link_libraries(PipeWireManagerTest PRIVATE sonero_core Qt6::Test)
add_test(NAME PipeWireManagerTest COMMAND PipeWireManagerTest)

add_executable(MixerTest MixerTest.cpp)
target_link_libraries(MixerTest PRIVATE sonero_core Qt6::Test)
add_test(NAME MixerTest COMMAND MixerTest)

add_executable(DeviceFormatTest DeviceFormatTest.cpp)
target_link_libraries(DeviceFormatTest PRIVATE sonero_core Qt6::Test)
add_test(NAME DeviceFormatTest COMMAND DeviceFormatTest)

# SettingsStore lives in the app target, so compile its source into the test.
add_executable(SettingsStoreTest SettingsStoreTest.cpp
    ${CMAKE_SOURCE_DIR}/config/SettingsStore.cpp)
target_link_libraries(SettingsStoreTest PRIVATE sonero_core Qt6::Test)
add_test(NAME SettingsStoreTest COMMAND SettingsStoreTest)

# Likewise for the autostart helper.
add_executable(AutoStartTest AutoStartTest.cpp
    ${CMAKE_SOURCE_DIR}/app/AutoStart.cpp)
target_link_libraries(AutoStartTest PRIVATE sonero_core Qt6::Test)
add_test(NAME AutoStartTest COMMAND AutoStartTest)

# System integration checks — most importantly, what the privileged fixes run.
add_executable(SystemSetupTest SystemSetupTest.cpp
    ${CMAKE_SOURCE_DIR}/app/SystemSetup.cpp
    ${CMAKE_SOURCE_DIR}/app/AutoStart.cpp)
target_link_libraries(SystemSetupTest PRIVATE sonero_core Qt6::Test)
target_compile_definitions(SystemSetupTest PRIVATE
    SONAR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME SystemSetupTest COMMAND SystemSetupTest)

# Equalizer gain staging — guards against feeding the summed display curve into
# the filter cascade (which clipped) and against losing the headroom compensation.
add_executable(EqualizerTest EqualizerTest.cpp)
target_link_libraries(EqualizerTest PRIVATE sonero_core Qt6::Test)
add_test(NAME EqualizerTest COMMAND EqualizerTest)

# Automatic gain — the loop must settle rather than hunt, and hold during silence.
add_executable(AutoGainTest AutoGainTest.cpp)
target_link_libraries(AutoGainTest PRIVATE sonero_core Qt6::Test)
add_test(NAME AutoGainTest COMMAND AutoGainTest)

# The brace formatter that stands in for std::format on older toolchains.
add_executable(FormatTest FormatTest.cpp)
target_link_libraries(FormatTest PRIVATE sonero_core Qt6::Test)
add_test(NAME FormatTest COMMAND FormatTest)

# Update logic: version comparison, the GitHub payload, which bundle is offered
# and — the one that decides whether the app may write anything at all — which
# kind of install this is. All pure; the network path is not exercised here.
add_executable(UpdaterTest UpdaterTest.cpp
    ${CMAKE_SOURCE_DIR}/app/Updater.cpp
    ${CMAKE_SOURCE_DIR}/config/SettingsStore.cpp)
target_link_libraries(UpdaterTest PRIVATE sonero_core Qt6::Test Qt6::Network)
add_test(NAME UpdaterTest COMMAND UpdaterTest)

# The microphone chain: gate, de-esser, compressor and limiter, each checked
# against a signal whose right answer is known before the code runs.
add_executable(MicChainTest MicChainTest.cpp)
target_link_libraries(MicChainTest PRIVATE sonero_core Qt6::Test)
add_test(NAME MicChainTest COMMAND MicChainTest)

# The noise model's wrapper. The interesting half of this suite only runs where
# RNNoise is installed (CI builds it); everywhere else it checks the one thing
# that matters without a model — that the microphone still comes out the far end.
add_executable(NoiseSuppressorTest NoiseSuppressorTest.cpp)
target_link_libraries(NoiseSuppressorTest PRIVATE sonero_core Qt6::Test)
add_test(NAME NoiseSuppressorTest COMMAND NoiseSuppressorTest)

# Harmonic bass synthesis. Each case is a signal whose right answer is known
# before the code runs: a pure sine has no harmonics, so anything measured at
# 2f or 3f was synthesised, and halving the input must halve them.
add_executable(HarmonicBassTest HarmonicBassTest.cpp)
target_link_libraries(HarmonicBassTest PRIVATE sonero_core Qt6::Test)
add_test(NAME HarmonicBassTest COMMAND HarmonicBassTest)

# The brick-wall limiter that follows the bass stage, and the only thing standing
# between the Lab channel's unbounded equalizer and a driver.
add_executable(LimiterTest LimiterTest.cpp)
target_link_libraries(LimiterTest PRIVATE sonero_core Qt6::Test)
add_test(NAME LimiterTest COMMAND LimiterTest)



# Themes. The substitution must leave no placeholder behind (Qt would reject the
# sheet), a partial or broken theme file must fall back rather than paint the app
# black, and a theme must not be able to replace the built-in sheet — which is
# what guarantees there is always a way back from a bad one.
add_executable(ThemeTest ThemeTest.cpp
    ${CMAKE_SOURCE_DIR}/ui/Theme.cpp
    ${CMAKE_SOURCE_DIR}/app/SystemSetup.cpp
    ${CMAKE_SOURCE_DIR}/app/AutoStart.cpp
    # The two widgets that restyle themselves on a palette change, so the
    # re-entrancy case can be built and switched.
    ${CMAKE_SOURCE_DIR}/ui/widgets/AppChip.cpp
    ${CMAKE_SOURCE_DIR}/ui/widgets/ChannelStrip.cpp
    ${CMAKE_SOURCE_DIR}/ui/widgets/VuMeter.cpp
    ${CMAKE_SOURCE_DIR}/ui/widgets/FlowLayout.cpp)
target_link_libraries(ThemeTest PRIVATE sonero_core Qt6::Test Qt6::Widgets)
# SystemSetup is compiled in for resourcePath(), which is how the theme loader
# finds the examples that ship with an install. It wants the same define the
# SystemSetup suite passes it.
target_compile_definitions(ThemeTest PRIVATE SONAR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME ThemeTest COMMAND ThemeTest)
# The only test that needs a graphics platform. It links Qt6::Widgets, so
# QTEST_MAIN gives it a QApplication rather than the QCoreApplication the other
# suites get, and it shows real widgets — the re-entrancy case only reproduces on
# a shown widget tree. A CI container has no display, where QApplication aborts
# before the first case runs.
set_tests_properties(ThemeTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# One output channel's whole signal path: trim, equalizer, harmonic bass, limiter
# and fader. The cases that matter are the invariants the old split arrangement
# got for free from PipeWire and this chain now has to keep itself — headroom
# compensation, a balance control that only ever attenuates, and output that does
# not depend on the block size PipeWire happens to hand over.
add_executable(ChannelChainTest ChannelChainTest.cpp)
target_link_libraries(ChannelChainTest PRIVATE sonero_core Qt6::Test)
add_test(NAME ChannelChainTest COMMAND ChannelChainTest)

# The generic battery path. Driven against a fabricated /sys tree, so the result
# does not depend on what hardware is plugged into the machine running it — and
# so the case that matters most can be tested at all: refusing to report the
# laptop's own battery as a headset's.
add_executable(BatterySourceTest BatterySourceTest.cpp)
target_link_libraries(BatterySourceTest PRIVATE sonero_core Qt6::Test)
add_test(NAME BatterySourceTest COMMAND BatterySourceTest)

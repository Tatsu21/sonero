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

# Sonero

A native, open-source Linux alternative to SteelSeries Sonar — a per-application
audio mixer, router and DSP suite built directly on **PipeWire**.

> Status: **Stage 1 / early development.** The CMake project, directory layout,
> Qt6 main window and a minimal PipeWire backend are in place. Audio routing,
> the mixer, EQ and DSP land in later stages.

## Technology

| Area            | Choice                          |
| --------------- | ------------------------------- |
| Language        | C++20                           |
| GUI             | Qt6 (Widgets)                   |
| Build           | CMake (≥ 3.21)                  |
| Audio backend   | PipeWire (`libpipewire-0.3`)    |
| Logging         | spdlog (optional, auto-detected)|

## Build dependencies (Debian / Ubuntu)

```bash
sudo apt install build-essential cmake pkg-config \
    qt6-base-dev libpipewire-0.3-dev libspdlog-dev
```

`libspdlog-dev` is optional — without it the built-in fallback logger is used.

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

Run the application:

```bash
./build/Sonero
```

Run the tests:

```bash
ctest --test-dir build --output-on-failure
```

## Project layout

```
Sonero/
├── app/      Composition root (main, Application)
├── ui/       Qt6 widgets (MainWindow, pages, reusable widgets)
├── audio/    Audio backend (IAudioBackend, PipeWireManager) + engine (later)
├── core/     Cross-cutting utilities (logging)
├── dsp/      DSP modules (later stages)
├── plugins/  LV2 host (later stage)
├── profiles/ Saved user profiles (later stage)
├── config/   Runtime configuration (later stage)
└── tests/    Unit tests (QtTest)
```

## License

See [LICENSE](LICENSE).

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is **Micro-Manager's C++ hardware abstraction layer** (mmCoreAndDevices) - the core of the open-source Micro-Manager microscopy software. It provides APIs to load device adapters, control microscopy hardware, and acquire image data. Bindings exist for Python (pymmcore) and Java (MMCoreJ).

## Build Commands

**Using Meson (Recommended for development)**:
```bash
# Install tools (one-time)
uv tool install ninja meson rust-just

# Build and test everything
just test

# Build individual components
just build-mmdevice
just build-mmcore

# Test individual components
just test-mmdevice
just test-mmcore

# Clean build artifacts
just clean
just zap  # Full clean including subprojects
```

**One-liner with only uv installed**:
```bash
uvx --from rust-just --with meson --with ninja just test
```

**Direct Meson commands**:
```bash
cd MMDevice
meson setup --vsenv builddir  # --vsenv for Windows
meson compile -C builddir
meson test -C builddir --print-errorlogs
```

**Legacy builds**: See README.md for Visual Studio (Windows) and Autotools (Unix) instructions.

## Architecture

### Core Components

- **MMDevice/** - Device adapter interface (header-only library). Defines `MM::Device` and derived interfaces (`MM::Camera`, `MM::Stage`, etc.). Device adapters implement these interfaces.

- **MMCore/** - Hardware abstraction layer (static library). The `CMMCore` class is the main public API. Loads device adapters as plugins and provides unified device control.

- **DeviceAdapters/** - 248+ device adapters for specific hardware (cameras, stages, shutters, etc.). `DemoCamera` is the reference implementation.

- **MMCoreJ_wrap/** - Java bindings via SWIG. Requires SWIG 2.x or 3.x (not 4.x).

### Key Design Constraints

**ABI Stability**: The device interface maintains binary compatibility across compiler versions and settings. This is critical because device adapters are loaded as plugins at runtime.

**POD Types Only**: Public virtual methods in `MMDevice.h` must use Plain Old Data types only. Never use `std::string` parameters - use `const char*` instead. This prevents inter-DLL incompatibilities.

**Device Interface Version**: Currently at version 74 (`DEVICE_INTERFACE_VERSION` macro in `MMDevice.h`). Any changes to class definitions require incrementing this version.

**Public API Naming**: MMCore public methods use lowercase naming (e.g., `getConfiguration()`) for Java compatibility via SWIG.

### Plugin Architecture

Device adapters are dynamic libraries loaded at runtime:
- Windows: DLLs via `LoadedModuleImplWindows.cpp`
- Unix: .so/.dylib via `LoadedModuleImplUnix.cpp`

"Hub" devices can manage child devices (e.g., a controller managing multiple stages).

## Testing

Tests use **Catch2** framework:
- `MMDevice/unittest/` - Device interface tests
- `MMCore/unittest/` - Core tests including mock device adapters

Run specific test file:
```bash
cd MMCore/builddir
meson test CoreCreateDestroy-Tests --print-errorlogs
```

## API Documentation

- Main API: https://micro-manager.org/apidoc/MMCore/latest/
- CMMCore class: https://micro-manager.org/apidoc/MMCore/latest/class_c_m_m_core.html

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

`mmCoreAndDevices` is the C++ core of the [Micro-Manager](https://micro-manager.org)
microscopy project. It contains three layered components plus ~250 hardware device
adapters:

- **MMDevice** (`MMDevice/`) — the device adapter SDK. Defines the C++ interface
  (`MMDevice.h`) between device adapters and the core, plus default implementations
  in `DeviceBase.h`. Statically linked into every adapter.
- **MMCore** (`MMCore/`) — the hardware abstraction layer. `CMMCore` (in
  `MMCore.cpp`, ~9k lines) is the central façade: it loads adapter modules, owns
  device instances, and exposes the user-facing API (image acquisition, property
  access, config groups, etc.).
- **MMCoreJ_wrap** (`MMCoreJ_wrap/`) — SWIG-generated Java bindings for MMCore
  (`MMCoreJ.i` is the SWIG interface). The Python bindings (pymmcore) live in a
  separate repo.
- **DeviceAdapters** (`DeviceAdapters/`) — one subdirectory per hardware adapter
  (e.g. `DemoCamera`, `Utilities`, `SequenceTester`). `DemoCamera` is the
  reference/example adapter.

This repo is also the upstream for split-out mirror repositories: `MMDevice/` →
[mmdevice](https://github.com/micro-manager/mmdevice), `MMCore/` →
[mmcore](https://github.com/micro-manager/mmcore). Those mirrors are read-only;
this repo is the official source. The `sync-components.yml` workflow pushes
history to them on merge to `main`.

## Architecture: how the layers fit together

- A **device adapter** is a shared library (`.dll`/`.so`/`.dylib`) exporting the
  C `MODULE_API` functions in `MMDevice/ModuleInterface.h`: `InitializeModuleData`,
  `CreateDevice`, `DeleteDevice`, `GetDeviceInterfaceVersion`, etc. Adapters
  register the devices they provide via `RegisterDevice(...)` (typically in a
  `*Module.cpp` or in `InitializeModuleData`).
- Each adapter device subclasses a `CDeviceBase`-derived helper from `DeviceBase.h`
  (e.g. `CCameraBase`, `CStageBase`, `CShutterBase`) which provides default
  implementations and the property machinery. Device categories are the
  `MM::DeviceType` enum in `MMDeviceConstants.h` (Camera, Shutter, Stage, XYStage,
  State, Hub, Galvo, SLM, etc.).
- **Properties** (`MMDevice/Property.h`) are the universal name/value mechanism for
  exposing device settings; many use `CPropertyAction` callbacks (the `OnXxx`
  pattern) that fire on get/set.
- MMCore wraps each loaded adapter device in a corresponding **`*Instance`** class
  under `MMCore/Devices/` (e.g. `CameraInstance`, `StageInstance`). These adapt the
  raw vtable-based device interface into the core's internal API and route the
  `MM::Core` callback (`CoreCallback.cpp`) back to devices. `DeviceManager` owns the
  set of loaded instances; `PluginManager`/`LoadedDeviceAdapter` handle loading
  modules from disk.
- Image acquisition flows through a `CircularBuffer` / `FrameBuffer`; logging goes
  through the header-only generic logging framework in `MMCore/Logging/`.

### Binary-compatibility rules (critical when editing MMDevice)

The device interface is deliberately ABI-stable so adapters built with one
compiler/runtime load into a core built with another (e.g. MSVC Debug vs Release).
When touching `MMDevice/`:

- **Method parameters and return values in `MM::Device` and derived interface
  classes must be POD types or pointers** — no `std::string`, `std::vector`, etc.
  across the interface boundary.
- Any binary-incompatible change to the interface **must** increment
  `DEVICE_INTERFACE_VERSION` in `MMDevice/MMDevice.h`. MMCore refuses to load an
  adapter whose DIV does not exactly match its own. See `MMDevice/README.md` for
  the full versioning policy and DIV history.

## Build & test

There are two build systems. The **Meson** system is the modern, cross-platform
one and is the only practical way to build/test MMDevice and MMCore in isolation —
use it for verifying changes to those modules. The traditional systems (MSVC
`.sln`/`.vcxproj` on Windows, GNU Autotools `Makefile.am` on macOS/Linux) build the
whole of Micro-Manager and are driven from the parent
[micro-manager](https://github.com/micro-manager/micro-manager) repo.

### Meson via the justfile (preferred for MMDevice/MMCore work)

Requires `just`, `meson`, and `ninja`. The `justfile` only covers MMDevice and
MMCore.

```sh
just                 # list available commands
just build-mmdevice  # build MMDevice
just build-mmcore    # build MMCore (depends on MMDevice; copies it into subprojects)
just test-mmdevice   # build + run MMDevice unit tests
just test-mmcore     # build + run MMCore unit tests
just test            # run all tests
just clean           # clean build artifacts
just zap             # remove all Meson builddirs and subprojects
```

Zero-install one-liner (only `uv` required):

```sh
uvx --from rust-just --with meson --with ninja just test
```

Direct Meson (mirrors what the justfile and CI do):

```sh
meson setup MMDevice/builddir MMDevice --buildtype debug -Dcatch2:tests=false
meson compile -C MMDevice/builddir
meson test -C MMDevice/builddir --print-errorlogs
```

Run a single test with Meson: `meson test -C MMDevice/builddir <test-name>`.
Unit tests use Catch2 and live in `MMDevice/unittest/` and `MMCore/unittest/`.

### Whole-project build (device adapters)

Building an individual device adapter generally requires the full Micro-Manager
build. On macOS/Linux, clone `micro-manager`, use this repo as its submodule, and
follow its build docs; `./configure --without-java` skips the Java parts and builds
only MMCore and the adapters. On Windows, open `micromanager.sln` in Visual Studio
(see the Micro-Manager "Building on Windows" docs).

## CI checks to satisfy locally

- **UTF-8 encoding** (`ci-misc.yml`): all `.cpp`/`.h`/`.txt` files must be valid
  UTF-8. Run `./tools/check-utf8.sh`.
- **Strict Doxygen** (`ci-misc.yml`): Doxygen runs in strict mode over MMCore docs;
  keep doc comments well-formed.
- **Meson build & tests** (`ci-mmdevice-mmcore.yml`): runs only when files under
  `MMDevice/`, `MMCore/`, or `MMCoreJ_wrap/` change.

## Conventions

- Source files start with the standard Micro-Manager banner comment block
  (FILE / PROJECT / SUBSYSTEM / DESCRIPTION / AUTHOR / COPYRIGHT / LICENSE).
  Match the surrounding file when adding new ones.
- License is BSD; `license.txt` is present per component.

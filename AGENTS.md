# Repository Guidelines

## Project Structure & Module Organization
- `MMCore/`: C++ hardware abstraction layer and public API (`CMMCore`).
- `MMDevice/`: Device interface headers and common utilities for adapters.
- `DeviceAdapters/`: Hardware-specific adapters (each subfolder is a device).
- `MMCoreJ_wrap/`: Java bindings via SWIG.
- `buildscripts/VisualStudio/` and `micromanager.sln`: Windows build assets.
- `tools/`, `m4/`: build helpers and legacy tooling.

## Build, Test, and Development Commands
Preferred dev workflow uses Meson via the `justfile`:
- `just` — list available tasks.
- `just build-mmcore` / `just build-mmdevice` — build core components.
- `just test` — build + run tests.
- `just test-mmcore` / `just test-mmdevice` — run component tests.
- `just clean` / `just zap` — clean build outputs (zap is more thorough).

Direct Meson (example):
```bash
cd MMCore
meson setup --vsenv builddir   # add --vsenv on Windows
meson compile -C builddir
meson test -C builddir --print-errorlogs
```
Windows builds typically use `micromanager.sln` in Visual Studio.

## Coding Style & Naming Conventions
- C++ is the primary language; follow existing file-local conventions.
- Public MMCore methods use lowerCamelCase (for Java/SWIG compatibility).
- **MMDevice ABI rules:** public virtual methods in `MMDevice.h` must use POD
  types only (e.g., `const char*`, not `std::string`).
- Device interface versioning is strict; update `DEVICE_INTERFACE_VERSION` when
  changing interface signatures.

## Testing Guidelines
- Unit tests use Catch2.
- Locations: `MMCore/unittest/`, `MMDevice/unittest/`.
- Naming: `*-Tests.cpp` (e.g., `CoreCreateDestroy-Tests.cpp`).
- Run tests with `just test` or `meson test -C builddir --print-errorlogs`.

## Commit & Pull Request Guidelines
- Commit subjects commonly use a component prefix: `MMCore: ...`, `Core: ...`,
  `DeviceAdapters: ...`, `CI: ...`.
- Prefer short, imperative summaries; keep scope focused.
- PRs should include: clear description, linked issue (if applicable),
  and test results or rationale for why tests were not run.

## Configuration Notes
- If working inside the top-level `micro-manager` repo, ensure this submodule
  points to your fork before making changes.

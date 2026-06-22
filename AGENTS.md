# Repository Guidelines

## Project Structure & Module Organization

This repository contains the C++ core of Micro-Manager plus hardware device adapters.
`MMDevice/` is the device adapter SDK and ABI boundary; edit it carefully because it is
linked into adapters. `MMCore/` contains `CMMCore`, device instance wrappers, module
loading, buffers, and logging. `MMCoreJ_wrap/` holds SWIG Java bindings and Maven
metadata. `DeviceAdapters/` contains one subdirectory per adapter, with
`DeviceAdapters/DemoCamera/` serving as a useful reference. Unit tests live in
`MMDevice/unittest/` and `MMCore/unittest/`. Build helper scripts and checks are in
`buildscripts/`, `tools/`, `m4/`, and the root `justfile`.

## Build, Test, and Development Commands

Use the Meson-based `justfile` for focused MMDevice/MMCore development:

```sh
just                 # list available tasks
just build-mmdevice  # configure and build MMDevice
just build-mmcore    # build MMCore after staging local MMDevice
just test-mmdevice   # run MMDevice Catch2 tests
just test-mmcore     # run MMCore Catch2 tests
just test            # run both test suites
just clean           # clean Meson build outputs
```

With only `uv` installed, `uvx --from rust-just --with meson --with ninja just test`
runs the same test flow. Whole-project adapter builds are normally driven from the
parent `micro-manager` checkout on Unix, or `micromanager.sln` on Windows.

## Coding Style & Naming Conventions

Follow the style of nearby C++ files. Existing code uses BSD-style component license
headers, 3-space indentation in many core files, PascalCase class names, and `OnXxx`
property action callbacks. Keep public `MMDevice` interfaces ABI-stable: method
parameters and return values should remain POD types or pointers, and incompatible
interface changes require updating `DEVICE_INTERFACE_VERSION` in `MMDevice/MMDevice.h`.

## Testing Guidelines

Tests use Catch2 and are named by behavior, for example
`MMDevice/unittest/DeviceUtils-Tests.cpp`. Add or update tests near the module being
changed, and run the relevant `just test-*` command before submitting. For a direct
Meson run, use `meson test -C MMDevice/builddir --print-errorlogs`.

## Commit & Pull Request Guidelines

Recent history uses short, imperative or descriptive subjects such as `fix exposure
limit issues` and `Add SaperaGigE.sln`. Keep commits focused and mention the affected
component when helpful. Pull requests should describe the behavioral change, list
local test commands run, link related issues, and call out device hardware or platform
requirements needed for verification.

## Agent-Specific Notes

Do not overwrite generated files or user changes without checking first. Prefer `rg`
for repository searches, keep edits scoped, and avoid broad refactors when fixing a
specific adapter, core behavior, or binding issue.

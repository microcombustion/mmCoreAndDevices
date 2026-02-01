# AlliedVisionCamera Hardening Rationale (Supersedes _IMPLEMENTATION.md)

Date: 2026-02-01
Repo: `/Volumes/Data/work/GitHub/mmCoreAndDevices`
Scope: Implemented fixes in `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.h` and
`DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`.

This document summarizes each implemented fix and what specific issue it addresses. It supersedes `_IMPLEMENTATION.md`.

## 1) Buffer reallocation only when needed

**What changed**
`resizeImageBuffer()` now reallocates buffers only if the required size increases or buffers are not yet allocated.

**Issue addressed**
Avoids deleting and reallocating buffers on every `SnapImage()` call, reducing the likelihood of use-after-free if the SDK
still references previously announced buffers.

**Relevant code paths**
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp` (`resizeImageBuffer()`)

## 2) Mutex-protected buffer and pixel-format state

**What changed**
Added `m_bufferMutex` and used it to guard access to buffer pointers, buffer size, payload size, and pixel format data.
Functions that read/modify these fields now either hold the lock directly or copy state under lock into locals.

**Issue addressed**
Prevents races between asynchronous Vimba feature invalidation callbacks (which can mutate pixel format and related state)
and capture paths that read/resize buffers or interpret pixel formats.

**Relevant code paths**
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.h` (`m_bufferMutex`)
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp` (accesses in getters, `handlePixelFormatChange()`,
  `resizeImageBuffer()`, `transformImage()`, `insertFrame()`, and sequence setup)

## 3) Synchronization of payload size updates

**What changed**
`resizeImageBuffer()` now stores the payload size into `m_payloadSize` under the buffer mutex, and computes required
buffer sizes using a consistent, locally captured pixel format value.

**Issue addressed**
Eliminates data races where `m_payloadSize` could be written outside of the mutex while being read under the mutex,
and removes the race between pixel format changes and buffer size calculations.

**Relevant code paths**
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp` (`resizeImageBuffer()`)

## 4) Snap frame lifetime

**What changed**
Introduced a class-owned `VmbFrame_t m_snapFrame` and used it in `SnapImage()` instead of a stack-allocated frame.

**Issue addressed**
Prevents use-after-return if the SDK retains the frame pointer beyond the scope of `SnapImage()`.

**Relevant code paths**
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.h` (`m_snapFrame` member)
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp` (`SnapImage()`)

## 5) Revoke/flush/end sequence before resize in `SnapImage()`

**What changed**
Before reusing buffers, `SnapImage()` now attempts a best-effort cleanup sequence:
1. `VmbCaptureEnd_t`
2. `VmbCaptureQueueFlush_t`
3. `VmbFrameRevokeAll_t`

Errors are logged but not fatal to the snap path.

**Issue addressed**
Aligns with the SDK’s recommended teardown pattern used elsewhere (`StopSequenceAcquisition()`), reducing the chance
that frames remain in use or queued when buffers are resized or reused.

**Relevant code paths**
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp` (`SnapImage()`)

## 6) Protected buffer assignment for sequence acquisition

**What changed**
`StartSequenceAcquisition()` assigns `m_frames[i].buffer` and `m_frames[i].bufferSize` under `m_bufferMutex`.

**Issue addressed**
Prevents concurrent pixel format or buffer size updates from racing with frame setup, which could lead to mismatched
buffer sizes or invalid pointers in queued frames.

**Relevant code paths**
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp` (`StartSequenceAcquisition()`)

## 7) Safe transform and insert logic

**What changed**
`transformImage()` and `insertFrame()` copy pixel-format-derived values under the mutex into locals before use.

**Issue addressed**
Avoids race conditions where pixel format or buffer size changes mid-transform or during insertion into the core callback.

**Relevant code paths**
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp` (`transformImage()`, `insertFrame()`)

## 8) Pixel format mutation guarded

**What changed**
`handlePixelFormatChange()` updates `m_currentPixelFormat` under the mutex.

**Issue addressed**
Ensures pixel format updates cannot race with readers in capture and transform code.

**Relevant code paths**
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp` (`handlePixelFormatChange()`)

## 9) ROI getter fix

**What changed**
`GetROI()` now writes to output parameters by pointer rather than writing into a map of copies.

**Issue addressed**
Fixes a bug where `GetROI()` always returned zeros because it modified only local copies.

**Relevant code paths**
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp` (`GetROI()`)

## 10) Exposure setter verification

**What changed**
`SetExposure()` now checks `SetProperty()` return value and reads back the actual exposure before firing
`OnExposureChanged()`.

**Issue addressed**
Prevents reporting a requested exposure value when the SDK rejected or adjusted it. Reduces confusion and race effects
with invalidation callbacks.

**Relevant code paths**
- `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp` (`SetExposure()`)

## Notes

- No changes were made to `MMDevice.h` or public interface signatures.
- The changes focus on memory safety, synchronization, and correctness under asynchronous callbacks and repeated snapping.


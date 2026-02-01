# AlliedVision Crash Reassessment (mmCoreAndDevices)

Date: 2026-02-01
Repo: `/Volumes/Data/work/GitHub/mmCoreAndDevices`

## Scope

This reassessment cross-checks prior hardening notes and current source in this repo, plus the local Vimba SDK headers located at:

- `/Volumes/Data/work/GitHub/3rdparty/AVT/VimbaX-2023-1-Win64/api/include/VmbC/VmbC.h`
- `/Volumes/Data/work/GitHub/3rdparty/AVT/VimbaX-2023-1-Win64/api/include/VmbC/VmbCTypeDefinitions.h`

It also incorporates the earlier investigation writeup:

- `/Volumes/Data/work/GitHub/GitLab/umanager/_ALLIEDVISION_CRASH.md`

## Inputs Reviewed

Local analysis notes in this repo:

- `/Volumes/Data/work/GitHub/mmCoreAndDevices/_CROSSCHECK.md`
- `/Volumes/Data/work/GitHub/mmCoreAndDevices/_EXPOSURE_GLITCH.md`
- `/Volumes/Data/work/GitHub/mmCoreAndDevices/_POTENTIAL_CRASH_ISSUES.md`
- `/Volumes/Data/work/GitHub/mmCoreAndDevices/_IMPLEMENTATION.md`
- `/Volumes/Data/work/GitHub/mmCoreAndDevices/_FIX_ROI.md`

Primary adapter source code:

- `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.h`
- `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`

## Summary of Current State

The hardening described in `_IMPLEMENTATION.md` is largely present in the current codebase. The main crash-risk fixes are implemented (conditional buffer realloc, mutex protection around several accesses, class-owned `VmbFrame_t` for snap, and a revoke-before-resize step). However, the implementation still leaves a few concurrency and SDK-sequencing gaps that can plausibly allow residual instability.

**Key conclusion:** The hardening is mostly complete, but not airtight. Two data-race windows remain, and `SnapImage()` does not follow the Vimba API’s recommended queue-flush-before-revoke sequence. These gaps are enough to justify a continued crash risk under stress (exposure changes + snapping), even if the primary issues were reduced.

## Accuracy Check (Line-Referenced)

All line references below reflect the current file state as inspected on 2026-02-01.

### Confirmed Hardening (All Correct)

1. **Conditional buffer reallocation** — Verified at `AlliedVisionCamera.cpp:240-246`.
2. **Mutex-protected accesses** — Verified in:
   - `GetImageBuffer()` (`AlliedVisionCamera.cpp:376`)
   - `GetImageBytesPerPixel()` (`AlliedVisionCamera.cpp:409`)
   - `GetImageBufferSize()` (`AlliedVisionCamera.cpp:415`)
   - `GetBitDepth()` (`AlliedVisionCamera.cpp:421`)
   - `GetNumberOfComponents()` (`AlliedVisionCamera.cpp:427`)
   - `handlePixelFormatChange()` (`AlliedVisionCamera.cpp:786`)
   - `transformImage()` (`AlliedVisionCamera.cpp:1394`)
   - `insertFrame()` (`AlliedVisionCamera.cpp:1457`)
3. **Class-owned `m_snapFrame`** — Declared in header (`AlliedVisionCamera.h:461`), used in `SnapImage()` (`AlliedVisionCamera.cpp:1206-1248`).
4. **Revoke-before-resize in `SnapImage()`** — `VmbFrameRevokeAll_t` at `AlliedVisionCamera.cpp:1194` before `resizeImageBuffer()` at `AlliedVisionCamera.cpp:1197`.
5. **Protected buffer assignment in sequence acquisition** — Mutex lock at `AlliedVisionCamera.cpp:1288-1294`.
6. **ROI getter fix** — Pointer write at `AlliedVisionCamera.cpp:554` (`*field.second = atoi(value.data())`).
7. **Exposure setter fix** — Error check + readback at `AlliedVisionCamera.cpp:459-466`.

### Remaining Gaps (All Accurate)

1. **Data race in `resizeImageBuffer()`** — Confirmed:
   - `m_payloadSize` written outside mutex (`AlliedVisionCamera.cpp:230`).
   - `m_currentPixelFormat.getBytesPerPixel()` called outside mutex (`AlliedVisionCamera.cpp:237`).
   - Mutex lock starts at `AlliedVisionCamera.cpp:241`.
2. **Missing queue flush before revoke in `SnapImage()`** — Confirmed:
   - `SnapImage()` calls `VmbFrameRevokeAll_t` at `AlliedVisionCamera.cpp:1194` with no prior `VmbCaptureQueueFlush_t`.
   - `StopSequenceAcquisition()` correctly calls `VmbCaptureQueueFlush_t` at `AlliedVisionCamera.cpp:1365` before `VmbFrameRevokeAll_t` at `AlliedVisionCamera.cpp:1372`.
3. **`m_payloadSize` synchronization gap** — Confirmed:
   - Written at `AlliedVisionCamera.cpp:230` (no mutex).
   - Read at `AlliedVisionCamera.cpp:1208` and `AlliedVisionCamera.cpp:1291` (under mutex), which is a true data race.

### Completeness Note (SnapImage Error Path)

`SnapImage()` calls `StopSequenceAcquisition()` on errors (`AlliedVisionCamera.cpp:1215, 1221, 1229, 1235, 1244, 1251`). `StopSequenceAcquisition()` checks `IsCapturing()` before issuing `AcquisitionStop`; if `SnapImage()` fails before `m_isAcquisitionRunning` is set (`AlliedVisionCamera.cpp:1239`), the cleanup path is still safe. If it fails after, the cleanup is correct. No change needed.

## Confirmed Hardening Present in Code

These changes are in the current code and match the described hardening:

1. **Conditional buffer reallocation**
   - `resizeImageBuffer()` now returns early if the buffer is already large enough.
   - File: `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`

2. **Mutex-protected access to buffer/pixel format state**
   - `m_bufferMutex` is present in the class.
   - Protected accesses in `GetImageBuffer`, `GetImageBytesPerPixel`, `GetImageBufferSize`, `GetBitDepth`, `GetNumberOfComponents`, `handlePixelFormatChange`, `transformImage`, and `insertFrame`.
   - Files:
     - `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.h`
     - `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`

3. **Class-owned `VmbFrame_t` for `SnapImage()`**
   - `m_snapFrame` exists and is used instead of a stack frame.
   - File: `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.h`
   - File: `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`

4. **Revoke-before-resize in `SnapImage()`**
   - `VmbFrameRevokeAll_t` is called before `resizeImageBuffer()`.
   - File: `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`

5. **Protected buffer assignment in sequence acquisition**
   - `StartSequenceAcquisition()` assigns `m_frames[i].buffer` under mutex.
   - File: `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`

6. **ROI getter fix**
   - `GetROI()` now uses pointers to the output variables.
   - File: `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`

7. **Exposure setter now checks return value and reads back actual value**
   - `SetExposure()` logs on failure and calls `GetExposure()` before `OnExposureChanged`.
   - File: `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`

## Remaining Gaps / Potential Oversights

These are still present and could plausibly explain continued crashes:

### 1) Data race in `resizeImageBuffer()`

`resizeImageBuffer()` reads `m_currentPixelFormat` and writes `m_payloadSize` without holding `m_bufferMutex`. At the same time, invalidation callbacks can update `m_currentPixelFormat` under the mutex, and other code reads `m_payloadSize` under the mutex (e.g., when assigning buffer sizes for frames).

- Risk: inconsistent `newBufferSize` due to concurrent pixel format changes; inconsistent payload size visibility across threads.
- Files:
  - `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`

### 2) Snapshot path missing queue-flush before revoke

The Vimba C API documentation recommends calling `VmbCaptureQueueFlush` before `VmbFrameRevokeAll` to avoid partial revokes. `StopSequenceAcquisition()` follows this sequence, but `SnapImage()` does not.

- Risk: revoke may fail with frames still in use, leaving a window where buffers can be freed while still referenced by SDK internals.
- Files:
  - `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`
  - `/Volumes/Data/work/GitHub/3rdparty/AVT/VimbaX-2023-1-Win64/api/include/VmbC/VmbC.h`

### 3) `m_payloadSize` and buffer size coupling not fully synchronized

`m_payloadSize` is updated outside the mutex, while it is read inside locked blocks (e.g., in `SnapImage()` and `StartSequenceAcquisition()`). This is a data race under C++ rules.

- Risk: undefined behavior and inconsistent buffer size usage.
- File:
  - `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`

### 4) Vimba SDK threading guarantees remain unconfirmed

The Vimba SDK headers do not clarify what thread context invalidation callbacks are invoked on. The added mutexes help, but there is still no explicit guarantee from the SDK that callbacks are serialized or non-concurrent with frame operations.

- Risk: remaining concurrency hazards depending on SDK behavior.
- Files:
  - `/Volumes/Data/work/GitHub/3rdparty/AVT/VimbaX-2023-1-Win64/api/include/VmbC/VmbC.h`
  - `/Volumes/Data/work/GitHub/mmCoreAndDevices/DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp`

## Assessment Against the Original Crash Report

From `/Volumes/Data/work/GitHub/GitLab/umanager/_ALLIEDVISION_CRASH.md`:

- Hard crashes persist after hardening, and the crash signature suggests heap corruption.
- PNG-per-capture streaming mitigates crashes; TIFF-per-repetition does not.

This aligns with a remaining use-after-free / race window in the adapter or the SDK. The remaining gaps (data races in `resizeImageBuffer()` and missing queue flush before revoke in `SnapImage()`) provide plausible mechanisms for continued instability, especially when exposure changes are interleaved with captures.

## Suggested Path Forward

### Phase 1 — Complete Adapter Hardening (Low risk, local changes)

1. **Eliminate remaining data races in `resizeImageBuffer()`**
   - Hold `m_bufferMutex` while reading `m_currentPixelFormat` and writing `m_payloadSize` / `m_bufferSize`.
   - Consider reading `GetImageWidth()` and `GetImageHeight()` outside the lock if they are slow, then lock only for pixel format and size updates.

2. **Follow Vimba’s revoke guidance in `SnapImage()`**
   - Call `VmbCaptureQueueFlush_t(m_handle)` before `VmbFrameRevokeAll_t(m_handle)`.
   - Best-effort is fine; log errors but do not fail the snap on revoke failure.

3. **Add lightweight runtime logging for verification**
   - Log a build identifier string at adapter init.
   - Log when `resizeImageBuffer()` re-allocates and the new size.
   - This supports validation that the hardened DLL is actually loaded and the new paths are exercised.

### Phase 2 — Isolate SDK vs Adapter Faults

1. **Run the exposure-change stress test in Vimba Viewer**
   - If it crashes there, focus on SDK-level bug report.
   - If it does not, the adapter or pymmcore path is the culprit.

2. **Throttle transport to reduce stress**
   - Temporarily reduce `StreamBytesPerSecond` and disable jumbo frames.
   - If this eliminates crashes, a transport-layer or SDK race is more likely.

3. **Attempt to capture a native stack trace**
   - Build a debug DLL of AlliedVisionCamera with symbols.
   - Use Application Verifier or a debugger that can capture heap corruption sites.

### Phase 3 — Escalation / Vendor Support

1. **File SDK report with Allied Vision**
   - Include reproduction steps, crash frequency, camera model, pixel format, and exposure change pattern.

2. **Cross-reference known issues**
   - Existing reports in VimbaPython/VmbPy suggest similar memory or threading failures.

## Concrete Action Items (Proposed)

- [ ] Fix data races in `resizeImageBuffer()` and `m_payloadSize` updates.
- [ ] Add `VmbCaptureQueueFlush_t` before `VmbFrameRevokeAll_t` in `SnapImage()`.
- [ ] Add a logging line at adapter init to confirm hardened build loaded.
- [ ] Re-run the exposure-change + snap stress test.
- [ ] If crashes persist, proceed with Vimba Viewer test and SDK escalation.

## Notes on Future Changes

If further work touches `MMDevice.h`, the ABI rules (POD types in public virtual methods) must be respected. No changes to device interface signatures are proposed here.

---

If you want, I can apply the Phase 1 changes and prepare a minimal patch for review.

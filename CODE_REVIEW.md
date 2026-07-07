# Code Review: SaperaGigE Device Adapter

**Branch:** `teledyne-dalsa-genie-GigE-new` vs `main`
**Scope:** `DeviceAdapters/SaperaGigE/` (new adapter, ~2,275 lines: `SaperaGigE.cpp`, `SaperaGigE.h`, MSVC project files)
**Date:** 2026-07-06
**Method:** multi-angle review (line-by-line scan, MM::Camera interface-contract trace, reuse, simplification, efficiency, altitude, repo conventions) with per-finding verification.

## Overall assessment

The adapter's architecture is in good shape. The interface-contract checks came back
clean on the things that usually go wrong in camera adapters:

- `RegisterDevice`/`CreateDevice`/`GetName` naming is consistent.
- `PrepareForAcq`/`AcqFinished` are correctly paired (`AcqFinished` fires only when
  `PrepareForAcq` succeeded, via the `notifyCore` flag).
- `Shutdown()`/destructor idempotency and the destructor-after-`Shutdown` path are guarded.
- `GetImageWidth`/`GetImageHeight`/`GetImageBytesPerPixel`/`GetImageBufferSize` are
  mutually consistent (all derived from `img_`) for both mono and the 4-byte color path,
  so the core's computed buffer size matches.
- The 6-arg `InsertImage` without a metadata string is fine —
  `CoreCallback::AddCameraMetadata` supplies the required tags itself.
- All files are valid UTF-8 (`./tools/check-utf8.sh` passes).

Eight findings survived verification, ranked most severe first. Verdicts:
**CONFIRMED** = trigger and wrong outcome are nameable from the code;
**PLAUSIBLE** = mechanism is real, trigger depends on camera/SDK behavior.

## Correctness findings

### 1. `CheckValue()` divide-by-zero / `SetUpBinningProperties()` infinite loop — PLAUSIBLE

`SaperaGigE.cpp:1101–1108`, `1810`, `1824`

The return values of `GetInc`/`GetMin`/`GetMax` are ignored, so a failed call leaves
`inc` an uninitialized `INT64`. If the camera reports an increment of 0 for
`OffsetX`/`OffsetY`/`Width`/`Height`, `(value / (long)inc)` at line 1107 is an integer
divide-by-zero → crash. In `SetUpBinningProperties`, `inc == 0` makes
`for (i = min; i <= max; i += inc)` an infinite loop, hanging `Initialize()`.

**Fix:** check the `Get*` return values and guard `inc <= 0` (treat as `inc = 1`).

### 2. `SnapImage()` hard-codes `Xfer_->Wait(16000)` and holds the SDK mutex across it — CONFIRMED

`SaperaGigE.cpp:583–602`

Exposure limits are taken from the camera (lines 442–444), so a user can legally set an
exposure longer than 16 s; `Wait(16000)` then times out, `Freeze()` aborts the
in-progress exposure, and `SnapImage` returns `DEVICE_ERR` — the valid frame is lost.
Meanwhile `saperaMutex_` is held for the entire blocking wait, and every `OnXxx`
property handler locks it at entry, so any property read/write during a slow snap
blocks for up to 16 s.

**Fix:** derive the wait timeout from the current exposure (plus transfer margin), and
consider releasing `saperaMutex_` across the wait — snap/sequence mutual exclusion is
already provided by `seqLock_`/`transferActive_`.

### 3. Binning allowed values use `set_union` instead of intersection — CONFIRMED

`SaperaGigE.cpp:1834–1838`

Allowed `Binning` values are the set-union of the vertical and horizontal ranges, but
`OnBinning()` writes the chosen value to **both** axes. A value supported by only one
axis (e.g. `BinningVertical {1,2}`, `BinningHorizontal {1,2,4}`, user selects 4) is
offered yet always fails with `DEVICE_ERR`, potentially after one axis was already
changed — leaving the two axes inconsistent and `img_` un-resynced.

**Fix:** use `std::set_intersection`.

### 4. `GetImageBuffer()` can return NULL after a successful snap — PLAUSIBLE

`SaperaGigE.cpp:617–650`

On a color camera, `SnapImage()` can succeed and a transient `Conv_->Convert()` failure
then makes `GetImageBuffer()` return NULL. `CMMCore::getImage()` passes the buffer to an
installed image processor **before** its null check (`MMCore/MMCore.cpp:2933` vs
`:2942`) → null-pointer dereference in the processor. Even without a processor, a
successful snap surfaces as `MMERR_CameraBufferReadFailed`.

**Fix:** retry or fail the snap instead of returning NULL from `GetImageBuffer()`
(e.g. keep the last good frame), and/or note the core-side ordering hazard upstream.

### 5. Unguarded `Xfer_` after a failed reconfigure — PLAUSIBLE

`SaperaGigE.cpp:586–587`, `:976`

After a runtime reconfigure (`OnPixelType`/`OnWidth`/`OnHeight`) whose
`SynchronizeBuffers()` rebuild fails, `DestroySaperaPipelineForReconfigure_()` leaves
the wrappers `Destroy()`ed but non-NULL, with `initialized_` still true. The next
`SnapImage()` (`Xfer_->SetCommandTimeout`/`Snap`) or `StartSequenceAcquisition()`
(`Xfer_->Grab()`) then operates on a destroyed `SapTransfer` — at best a confusing SDK
failure, at worst undefined SDK behavior. `GetImageBuffer()` already has the right
guard (`!Buffers_ || !*Buffers_`).

**Fix:** mirror the `Xfer_ && *Xfer_` created-state guard in both entry points.

### 6. `img_` staging-buffer race between `XferCallback` and `GetImageBuffer()` — PLAUSIBLE

`SaperaGigE.cpp:1746–1752`

`XferCallback` fills `img_` under `saperaMutex_` but unlocks at line 1749 before
`InsertImage()` reads `img_`. During a live sequence, a client calling
`core.getImage()` reaches `GetImageBuffer()` on the MMCore thread, which takes the
(now free) mutex and `ReadRect`s into `img_` while `InsertImage` is still copying it
into the circular buffer → torn/corrupted delivered frame. The header documents the
"don't read `img_` during a sequence" invariant, but nothing enforces it against
`CMMCore::getImage()`, which clients may call at any time.

**Fix:** hold the lock across `InsertImage`, or have `GetImageBuffer()` reject/queue
while `IsCapturing()`.

## Cleanup findings

### 7. Dead code left from the template and debugging iterations — CONFIRMED

`SaperaGigE.cpp:1518` and related

- `GenerateImage()` — never called; also buggy (`std::max` should be `std::min`, so the
  fill always saturates). `MAX_BIT_DEPTH` (`SaperaGigE.h:134`) is used only here.
- `OnCameraName()` — never registered as any property's action; its `AfterSet` branch is
  an empty block.
- `ErrorBox()`/`s2ws()` — never called.
- `NumberOfWorkableCameras_`/`NumberOfAvailableCameras_` — write-only bookkeeping;
  `NumberOfWorkableCameras_` is incremented at line 281 **without ever being
  initialized** (latent UB read).

**Fix:** delete all of it; this shrinks the adapter's apparent state surface to what is
actually load-bearing.

### 8. Conventions: missing COPYRIGHT banner field and missing `license.txt` — CONFIRMED

`SaperaGigE.cpp:1`, `SaperaGigE.h:1`, component directory

Repo `CLAUDE.md`: *"Source files start with the standard Micro-Manager banner comment
block (FILE / PROJECT / SUBSYSTEM / DESCRIPTION / AUTHOR / COPYRIGHT / LICENSE)"* and
*"License is BSD; `license.txt` is present per component."* Both source banners lack a
COPYRIGHT line, and `DeviceAdapters/SaperaGigE/` ships no `license.txt` even though the
banner claims "License text is included with the source distribution" (109 sibling
adapters, e.g. `DemoCamera`, have one).

**Fix:** add the COPYRIGHT line and copy a BSD `license.txt` into the component.

## Honorable mentions (cut by the findings cap)

- `DestroySaperaPipeline_()` / `DestroySaperaPipelineForReconfigure_()` are
  near-duplicates; the crash-sensitive teardown order lives in two places. Fold into one
  `destroyPipeline(bool releaseWrappers)` helper.
- The color-convert + ROI-crop block is duplicated between `GetImageBuffer()` and
  `XferCallback()`; a shared `readCurrentFrameInto(ImgBuffer&)` helper would keep the
  snap and stream paths from diverging.
- `performTeardown_()` logs a `Wait(5000)` timeout but proceeds to clear flags anyway;
  `Shutdown()` then destroys the pipeline — the exact live-transfer-destroy state the
  BSOD.md notes warn about. Consider treating a non-stopped transfer as a hard condition.
- `OnPixelType` does not reset the software ROI before `SynchronizeBuffers()`, unlike
  `OnWidth`/`OnHeight`/`OnBinning`; a format change coupled to a geometry change could
  leave a stale out-of-range ROI.
- The `const_cast<unsigned char*>(img_.GetPixels())` sites could use the existing
  `ImgBuffer::GetPixelsRW()`.
- Software frame pacing lets the camera free-run and discards early frames; when a low
  rate is requested, programming `AcquisitionFrameRate` (already exposed as a property)
  would avoid transferring frames that are then dropped.
- Transfer timeouts are scattered magic literals (`1000`, `16000`, `5000` ×2); a single
  named constant (or an exposure-derived value) would keep them in sync.

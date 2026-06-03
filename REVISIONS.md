# Plan: SaperaGigE → modern (non-legacy) CCameraBase adapter, in compile-green checkpoints

## Context

`DeviceAdapters/SaperaGigE/` is a Teledyne DALSA GigE camera adapter written ~2022
(DIV ~70). Rebased onto current `main` (DIV 75) it no longer compiles. Prior analysis
considered two approaches:

- **Legacy-base approach** — switch to `CLegacyCameraBase`. Minimal, but **rejected**: it
  is the legacy base class, directly contrary to the goal of a non-legacy adapter.
- **Modern-base, stub-only approach** — stay on `CCameraBase`, add the now-required
  members as `DEVICE_NOT_YET_IMPLEMENTED` stubs.

This plan supersedes both. **Goal:** a genuinely non-legacy adapter on `CCameraBase` with
*working* continuous (live/MDA) acquisition driven by the Sapera transfer callback,
reached in checkpoints. The stub-only compile fix becomes **Checkpoint 1** (compile-green
now); the streaming implementation is **Checkpoint 2** (a compile-green *target* once the
SDK-specific questions are resolved); cleanup is **Checkpoint 3**.

### Verified facts (current headers, `DEVICE_INTERFACE_VERSION 75`)

- `CCameraBase` (`MMDevice/DeviceBase.h:1377`) pure virtuals the adapter is **missing**:
  `bool Busy()` (1411) and `int StartSequenceAcquisition(double)` (1418). All other
  CCameraBase pure virtuals are already implemented. These two are the *only* hard
  compile errors.
- `GetPixelSizeUm()`, `PrepareSequenceAcqusition()`, `GetComponentName()` were **removed**
  from `MM::Camera`/`CCameraBase` in commit `310dd9507` (2026-02-20, after first being
  made `final`). The adapter overrides the first two — now dead/misleading; remove them.
- `GetNominalPixelSizeUm` is **not** in the current interface either; the adapter's
  `SaperaGigE.h:105` definition is harmless dead code (leave for now; revisit in Ck3).
- 5-arg `GetCoreCallback()->InsertImage(...)` at `SaperaGigE.cpp:684` still binds
  (`serializedMetadata` defaults to `nullptr`). No use of other removed APIs.
- A prior draft expected a separate migration note (`REVISIONS.md`); this document
  supersedes that note, so no extra file is created or maintained.

### Decisions confirmed with user

- End state = full streaming acquisition; Checkpoint 1 (compile-only) is a stepping stone.
- Acquisition model = **native Sapera transfer callback** (`Xfer_->Grab()` + `XferCallback`),
  no snap-loop thread (the dormant `SequenceThread` is unused in Ck2, deleted in Ck3).
- No Windows+Sapera SDK dev box yet: **Checkpoint 1 is specified for execution now**;
  **Checkpoint 2 is preliminary**, to be refined/validated once the SDK box exists.

---

## Checkpoint 1 — Compile clean on `CCameraBase` (snap-only). DO NOW.

Edits in `DeviceAdapters/SaperaGigE/SaperaGigE.h`:

1. Keep base class `CCameraBase<SaperaGigE>` (line 52, unchanged).
2. **Remove** dead override (line 97): `int PrepareSequenceAcqusition() { return DEVICE_OK; }`.
3. **Remove** dead override (line 106): `double GetPixelSizeUm() const { return 1.0 * GetBinning(); }`
   (`GetNominalPixelSizeUm()` on line 105 stays for now.)
4. **Add** `bool Busy() { return false; }` in the public section (near the existing
   `IsCapturing()` decl, ~line 101). `SnapImage()` blocks until the frame is captured,
   so the device is never asynchronously busy.
5. **Declare** the required overload by replacing the commented-out line 98
   `//int StartSequenceAcquisition(double interval);` with:
   `int StartSequenceAcquisition(double interval_ms);`

Edits in `DeviceAdapters/SaperaGigE/SaperaGigE.cpp`:

6. **Add a `StartSequenceAcquisition(double)` stub** (mirror the existing 3-arg stub at
   line 659): return `DEVICE_NOT_YET_IMPLEMENTED`, with a comment that it will forward to
   `StartSequenceAcquisition(LONG_MAX, interval_ms, false)` once Checkpoint 2 lands.
   Leave the existing 3-arg stub (659) and `StopSequenceAcquisition` stub (644) as-is.

**Checkpoint 1 result:** all `CCameraBase` pure virtuals satisfied, no removed/`final`
members overridden → compiles. Snap works; live mode returns not-implemented (unchanged
behavior). Verify by inspection (see Verification).

---

## Checkpoint 2 — Native-callback streaming acquisition (PRELIMINARY; refine on SDK box).

The transfer callback is already wired: `SynchronizeBuffers()` calls
`SapAcqDeviceToBuf(&AcqDevice_, &Buffers_, XferCallback, this)` (`SaperaGigE.cpp:1029`),
buffers are `SapBufferWithTrash(3, &AcqDevice_)` (cpp:1027), and a **static**
`XferCallback` (cpp:1050, decl `SaperaGigE.h:165`), an `InsertImage()` helper (681), and a
dead `PrepareForAcq` reference (667) already exist. Reference design: the Aravis adapter
(`DeviceAdapters/Aravis/AravisCamera.cpp`), which streams purely from the SDK callback.

This checkpoint is split into **(A) known steps** and **(B) design questions to confirm on
the SDK box**, because the exact Sapera calls for "which buffer just completed" and
continuous grab/stop cannot be verified without the (Windows-only) SDK headers.

### Lifecycle contract (resolves who does what, once)

Two threads touch shared state: the **MMCore thread** (`Start/Stop/IsCapturing`) and the
**Sapera transfer thread** (`XferCallback`). The callback never blocks and never tears
down; teardown happens exactly once on the MMCore thread:

- **Callback thread** only: copy+`InsertImage` the completed frame, `++imageCounter_`, and
  when finished (count reached, `InsertImage` error, or stop-on-overflow) it requests an
  async stop (`Xfer_->Freeze()`, no `Wait`) and flips `sequenceRunning_ = false`. It does
  **not** call `Wait()` or `AcqFinished()`.
- **MMCore thread**: Core polls `IsCapturing()`; when it returns `false` (or the user stops
  early) Core calls `StopSequenceAcquisition()`, which is the **single** place that
  `Xfer_->Freeze()` (idempotent) + `Xfer_->Wait(timeout)` + sets `sequenceRunning_ = false`
  + calls `GetCoreCallback()->AcqFinished(this, 0)`. Guard it so a second call (callback
  already stopped, then Core stops) is a no-op and `AcqFinished` fires once.

### (A) Known steps

State to add (`SaperaGigE.h` private), guarded for cross-thread access:
`long imageCounter_;`, `long numImages_;`, `bool stopOnOverflow_;` (reuse existing
`sequenceRunning_`). Add an `MMThreadLock seqLock_;` (from `DeviceThreads.h`, already
included) — or make the simple flags `std::atomic`.

**Locking rule (avoid stop-while-callback deadlock):** the lock protects *state only*.
Never hold `seqLock_` while calling into the Sapera SDK (`Grab`/`Freeze`/`Wait`,
buffer `ReadRect`) or MMCore (`InsertImage`/`AcqFinished`) — those can block or re-enter.
Pattern: take the lock, read/copy the state you need (and update counters/flags), release
it, then make the SDK/Core call; reacquire only to write back the result. This keeps the
callback thread and `StopSequenceAcquisition()` from blocking each other.

- **`StartSequenceAcquisition(long numImages, double, bool stopOnOverflow)`** (replace stub
  at cpp:659): reject if `sequenceRunning_` (`DEVICE_CAMERA_BUSY_ACQUIRING`);
  `GetCoreCallback()->PrepareForAcq(this)`; under the lock set
  `numImages_ = numImages`, `stopOnOverflow_`, `imageCounter_ = 0`, `sequenceRunning_ =
  true`; start continuous transfer (B1). On any SDK failure, reset `sequenceRunning_ = false`
  and return the error.
- **`StartSequenceAcquisition(double interval_ms)`** (the Checkpoint 1 stub): forward to
  `StartSequenceAcquisition(LONG_MAX, interval_ms, false)`.
- **`XferCallback(SapXferCallbackInfo* pInfo)`** (cpp:1050): recover the instance with
  `static_cast<SaperaGigE*>(pInfo->GetContext())` (the `this` from cpp:1029). **Replace the
  current blocking `ErrorBox(...)` call** with `LogMessage(...)` on the instance (the static
  method has no `LogMessage` of its own — that is why the instance is needed). If
  `pInfo->IsTrash()` → buffer overflow: log it; if `stopOnOverflow_` request stop (Freeze +
  `sequenceRunning_ = false`), else drop the frame and continue. Otherwise read the
  **specific completed buffer** (B2) into `img_`, push via
  `GetCoreCallback()->InsertImage(this, pixels, w, h, bytesPerPixel)`. On `InsertImage !=
  DEVICE_OK` (Core circular buffer full) honor `stopOnOverflow_` the same way. Increment
  `imageCounter_`; when `imageCounter_ >= numImages_` request stop. Shared-state access
  follows the locking rule above (copy under lock, SDK/Core calls outside it); never call
  `Wait()`/`AcqFinished()` here.
- **`StopSequenceAcquisition()`** (replace stub at cpp:644): the single teardown path per
  the lifecycle contract above; idempotent; `AcqFinished` exactly once.
- **`IsCapturing()`** (cpp:687): return `sequenceRunning_` (read under lock).
- **`Busy()`** stays `false` (callback-driven, non-blocking).
- **`SnapImage()`** keeps `Snap(1)`; snap is already guarded by `sequenceRunning_`
  (cpp:445) so snap and grab paths cannot collide.
- Repurpose the dead `InsertImage()` helper (681) or inline it into the callback. **Do
  not use the snap-loop `SequenceThread`** — the native callback replaces it; its deletion
  is Checkpoint 3.

### (B) Design questions to confirm on the SDK box (do not guess in code)

1. **Continuous grab vs stop:** confirm the API for free-running capture — likely
   `Xfer_->Grab()` to start and `Xfer_->Freeze()` to request stop, `Xfer_->Wait(timeout)`
   to join. Verify `Freeze()` is safe to call from the transfer-callback thread; if not,
   signal the MMCore thread to do it instead.
2. **Completed-buffer identity:** the current `GetImageBuffer()` (cpp:474) reads
   `Buffers_.ReadRect(...)` on the buffer's *current* index, which races a free-running
   transfer across the 3 + trash buffers. Determine how to read the **exact** frame that
   just completed — e.g. the index from `pInfo` / `pInfo->GetTransfer()` / the transfer's
   current `SapBuffer::GetIndex()` — and read that specific buffer so frames are neither
   duplicated, skipped, nor torn. This is the highest-risk unknown.
3. **`InsertImage` width/height/bytes during streaming:** confirm `img_`
   width/height/`bytesPerPixel_` are stable while grabbing (they are set in
   `SynchronizeBuffers`/`ResizeImageBuffer`); no ROI/format change is allowed mid-sequence.

**Checkpoint 2 result (target):** working live/MDA acquisition on the modern base → the
adapter is truly non-legacy. Compiles after (A); behavioral correctness depends on
resolving (B) and is validated only on the Windows+SDK build.

---

## Checkpoint 3 — Cleanup & hardening.

- Remove the now-unused `SequenceThread` class (`SaperaGigE.h:169-192`), the `thd_`
  member, its construction/teardown, and the commented snap-loop `svc()` (cpp:1136).
- Remove leftover commented-out blocks (e.g. cpp:632-642) and the dead
  `GetNominalPixelSizeUm()` if confirmed unused.
- Re-confirm no override collides with a removed/`final` `MM::Camera` member.

Each bullet is independent and keeps the build green.

---

## Verification

No local Sapera build (Windows-only SDK); until the SDK box exists, verify by inspection:

1. **Pure-virtual coverage:** every `= 0;` in `CCameraBase` (DeviceBase.h:1377-1583) has a
   matching override in `SaperaGigE.h` — confirmed set: `GetImageBuffer`, `GetImageWidth`,
   `GetImageHeight`, `GetImageBytesPerPixel`, `SnapImage`, `Busy`,
   `StartSequenceAcquisition(double)`, `StartSequenceAcquisition(long,double,bool)`,
   `StopSequenceAcquisition`, `IsCapturing`.
2. **No stale overrides:** `PrepareSequenceAcqusition`, `GetPixelSizeUm`, `GetComponentName`
   absent from the adapter.
3. **Signatures bind:** `InsertImage` 5-arg call matches `CoreCallback`.
4. **Later (SDK box), streaming correctness:** build on Windows with Sapera LT; run snap,
   then live mode and a finite MDA. Confirm:
   - frame count delivered == requested, with **no duplicated/skipped/torn frames**
     (validates design question B2 — completed-buffer identity);
   - clean stop both ways — natural completion (count reached) and early user stop — with
     `AcqFinished` firing **exactly once** and `IsCapturing()` returning to `false`;
   - **no blocking dialogs**: the old `ErrorBox` MessageBox path is gone; overflow surfaces
     as a log message, and `stopOnOverflow_` either stops or drops per spec;
   - no deadlock/crash under stop-while-callback-running (validates thread-safety/locking).
   MMDevice/MMCore CI does not cover individual adapters, so this is the only end-to-end gate.

## Scope notes

- Checkpoint 1 is compile-green and fully specified for execution now. Checkpoint 2 is a
  compile-green *target* once the (B) SDK questions are resolved; Checkpoint 3 is cleanup.
- All edits confined to `DeviceAdapters/SaperaGigE/{SaperaGigE.h,SaperaGigE.cpp}`.
  No DIV bump (adapter-only change).
- The scratch drafts in the repo root (the two prior PLAN_*.md files) are temporary and
  not committed; they can be deleted at any point — this document is the source of truth.

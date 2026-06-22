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
- A Windows box with the Sapera LT SDK installed (`C:\Program Files\Teledyne DALSA\Sapera`)
  is now available. Cross-checking Checkpoint 2 design questions B1/B2 against the actual
  `Classes/Basic/*.h` headers and the SDK's own example/demo sources (see "SDK verification"
  below) resolved both **without needing a build**: **Checkpoint 1 is specified for execution
  now**; **Checkpoint 2's (A) steps are now fully specified** (B1/B2 confirmed by inspection of
  installed headers + demos); only B3 remains as an adapter-side audit item, not an SDK
  unknown. Compiling/running Checkpoint 2 on this box (no Sapera hardware required for a
  build-only check) is still the way to catch syntax/API-signature mistakes before hardware
  testing.

### SDK verification (done against the installed Sapera LT SDK, no build yet)

Checked `Classes/Basic/SapTransfer.h`, `SapBuffer.h`, `SapBufferWithTrash.h`, `SapTransferEx.h`,
and example/demo sources under the SDK install (`Examples/Classes/GrabConsole/GrabCPP.cpp`,
`Demos/Classes/Vc/SeqGrabDemo/SeqGrabDemoDlg.cpp`, `Demos/Classes/Vc/GigeCameraDemo/`):

- **B1 confirmed exactly as assumed.** `SapTransfer` (base of `SapAcqDeviceToBuf`, which the
  adapter already uses) declares `Grab()`, `Freeze()`, `Wait(int timeout)`, `Snap(int count)`.
  `GrabCPP.cpp` free-running start/stop is literally `Xfer->Grab()` ... `Xfer->Freeze()`;
  `Xfer->Wait(5000)`, all from the calling thread — matching the baseline's "all three run only
  on the MMCore thread" design.
- **B2 turns out to already be resolved — not a real unknown.** `SapBuffer::GetIndex()` is
  documented in the header as `"Index of last grabbed buffer"`, and the no-index
  `SapBuffer::ReadRect(x,y,w,h,data)` overload reads at that index implicitly. The adapter's
  *existing* `GetImageBuffer()` (`SaperaGigE.cpp:477`, `Buffers_.ReadRect(...)` with no index
  arg) already uses this. `SeqGrabDemoDlg.cpp` confirms the pattern is correct *from inside the
  callback*: it reads `m_Buffers->GetIndex()` synchronously during/around the xfer-callback
  invocation, with no separate "which buffer just completed" lookup via `pInfo`. So
  Checkpoint 2's `XferCallback` can call the same no-index `Buffers_.ReadRect(...)` already used
  by `GetImageBuffer()`, synchronously inside the callback, and get the correct just-completed
  buffer. The original wording below ("highest-risk unknown") overstated the risk — it is
  superseded by this finding.
- **B3 is unaffected** — it was already correctly scoped as an adapter-side property-handler
  audit, not an SDK question.
- Also confirmed: the adapter's `ErrorBox()` (`SaperaGigE.cpp:74-77`) is a real blocking
  `MessageBox` call, and the `SapAcqDeviceToBuf`/`SapBufferWithTrash`/`Xfer_` wiring described
  below matches the current `.cpp` line-for-line.

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

## Checkpoint 2 — Native-callback streaming acquisition (fully specified; build-verify on SDK box).

The transfer callback is already wired: `SynchronizeBuffers()` calls
`SapAcqDeviceToBuf(&AcqDevice_, &Buffers_, XferCallback, this)` (`SaperaGigE.cpp:1029`),
buffers are `SapBufferWithTrash(3, &AcqDevice_)` (cpp:1027), and a **static**
`XferCallback` (cpp:1050, decl `SaperaGigE.h:165`), an `InsertImage()` helper (681), and a
dead `PrepareForAcq` reference (667) already exist. Reference design: the Aravis adapter
(`DeviceAdapters/Aravis/AravisCamera.cpp`), which streams purely from the SDK callback.

This checkpoint is split into **(A) known steps** and **(B) design questions**. B1 and B2 below
are now confirmed by inspection of the installed SDK headers and example sources (see
"SDK verification" above); only B3 remains as an adapter-side audit item. Build-only
compilation on the SDK box (no camera hardware needed) is still worthwhile to catch
signature/typo mistakes before hardware testing.

### Lifecycle contract (resolves who does what, once)

**Verified MMCore behavior — the HAL does not auto-stop.** MMCore has no internal thread
that polls `IsCapturing()` and tears the sequence down. `CMMCore::isSequenceRunning()` just
returns the device's `IsCapturing()` (`MMCore.cpp:3286`/`3294`), and
`CMMCore::stopSequenceAcquisition()` is only invoked by the **client / acquisition engine**
(`MMCore.cpp:3258`, `3195`). So the adapter must **not** assume `StopSequenceAcquisition()`
is called promptly when some internal "done" condition is reached — there is no such caller
inside the core.

**Proven reference pattern (Aravis) — follow it.** The cited streaming reference
`DeviceAdapters/Aravis/AravisCamera.cpp` is *client-stop driven* and sidesteps both the
callback-safety unknown (B1) and the auto-stop dependency:

- Both `StartSequenceAcquisition(numImages, …)` and `StartSequenceAcquisition(double)` start
  the **same** free-running stream; Aravis **ignores `numImages`** and never self-limits
  (`AravisCamera.cpp:1162`, `1175`). For a finite acquisition the **acquisition engine** stops
  it by calling `stopSequenceAcquisition()` after it has pulled `numImages` frames out of the
  circular buffer — the camera just keeps streaming until told to stop.
- The stream callback (`AravisCamera.cpp:147`) returns immediately if `!capturing`, otherwise
  copies the completed frame and `InsertImage`s it. It **never** stops the hardware, **never**
  calls `Wait()`, and **never** calls `AcqFinished()`.
- `StopSequenceAcquisition()` (`AravisCamera.cpp:1187`) is the **sole** teardown: guarded by
  `if (capturing)`, it sets `capturing = false`, stops the hardware, and calls `AcqFinished`
  exactly once. `IsCapturing()` returns `capturing`, which stays `true` until the client
  stops — so there is **never** a window where `IsCapturing()` is `false` while the hardware
  is still transferring.

Adopting this for SaperaGigE means one load-bearing flag, not three. The existing
`sequenceRunning_` member is **renamed** to `sequenceStarted_` (its single meaning: a
transfer is live and not yet torn down; also what `SnapImage()` checks to reject snap during
a sequence). The flip of this flag inside `StopSequenceAcquisition()` is the once-only guard
for both the hardware stop and `AcqFinished`. (`stopRequested_`/`acqFinishedSent_` from the
prior draft are **not needed** in this baseline; they reappear only in the optional
self-limiting refinement below.)

- **Callback thread** (`XferCallback`) only: if `!sequenceStarted_`, return immediately.
  Otherwise read the **specific completed buffer** (B2) into the staging buffer `img_`,
  `InsertImage` it, and `++imageCounter_` (counter is for image-number metadata/diagnostics
  only — it does **not** drive stopping). The callback **never** calls a Sapera stop function,
  `Wait()`, or `AcqFinished()`.
- **MMCore thread**: `StopSequenceAcquisition()` is the **single** teardown, called by the
  client/engine (early user stop, or after a finite acquisition has drained its frames).
  Under `seqLock_`, if `sequenceStarted_` is already false return `DEVICE_OK` (idempotent);
  otherwise set `sequenceStarted_ = false` and release the lock. Then, **outside** the lock,
  perform the Sapera stop (`Xfer_->Freeze()` then `Xfer_->Wait(timeout)`) and call
  `GetCoreCallback()->AcqFinished(this, 0)`. The flag flip under the lock guarantees exactly
  one thread runs the teardown and `AcqFinished` fires once.
- **`IsCapturing()`** returns `sequenceStarted_` (read under `seqLock_`). It only goes false
  in `StopSequenceAcquisition()`, so it never reports "done" while hardware still streams.

**Optional refinement (defer; not required for working live/MDA):** if a camera-side stop at
`numImages` or on overflow is later wanted (so the camera stops without waiting for the
engine), the callback would have to *initiate* the stop — which reintroduces the B1
callback-safety question and brings back `stopRequested_`/`acqFinishedSent_`. The baseline
above does not need it: Aravis ships finite MDA support without self-limiting, relying on the
engine to stop. Treat self-limiting as a Checkpoint-2+ option to evaluate on the SDK box, not
a requirement.

### (A) Known steps

State to add (`SaperaGigE.h` private): `long imageCounter_;` (image-number metadata only) and
the renamed lifecycle flag `bool sequenceStarted_;` (was `sequenceRunning_`). `numImages_` and
`stopOnOverflow_` are **not** stored in the baseline — the camera does not self-limit (see the
contract); add them only if the optional self-limiting refinement is taken. Add an
`MMThreadLock seqLock_;` (from `DeviceThreads.h`, already included).

**Locking primitive — use `MMThreadLock`, not `std::atomic`.** The flag, `imageCounter_`, and
(in the refinement) the stop bookkeeping must be read/updated *together*; a per-field atomic
cannot give that and would invite torn decisions across fields. One `MMThreadLock seqLock_`
guards all sequence state. **Locking rule (avoid stop-while-callback deadlock):** the lock
protects *state only*. Never hold `seqLock_` while calling into the Sapera SDK
(`Grab`/`Freeze`/`Wait`, buffer `ReadRect`) or MMCore (`InsertImage`/`AcqFinished`) — those can
block or re-enter. Pattern: take the lock, read/copy the state you need (and update the
counter), release it, then make the SDK/Core call.

- **`StartSequenceAcquisition(long numImages, double, bool stopOnOverflow)`** (replace stub
  at cpp:659): reject if `sequenceStarted_` (`DEVICE_CAMERA_BUSY_ACQUIRING`). Then, following
  the Aravis order, **start the transfer first** (B1) and only on success call
  `GetCoreCallback()->PrepareForAcq(this)` and, under the lock, set `imageCounter_ = 0` and
  `sequenceStarted_ = true`. `numImages`/`stopOnOverflow` are accepted but not enforced
  device-side (engine-driven stop). **Failure rollback:** if the transfer fails to start, do
  **not** set `sequenceStarted_`, do **not** call `PrepareForAcq`/`AcqFinished` (the sequence
  never began, so there is nothing to finish — `AcqFinished` pairs only with a successful
  `PrepareForAcq`), and return the SDK error. If `PrepareForAcq` itself fails after the
  transfer started, `Freeze()`+`Wait()` to undo the start, leave `sequenceStarted_` false, and
  return its error.
- **`StartSequenceAcquisition(double interval_ms)`** (the Checkpoint 1 stub): forward to
  `StartSequenceAcquisition(LONG_MAX, interval_ms, false)`.
- **`XferCallback(SapXferCallbackInfo* pInfo)`** (cpp:1050): recover the instance with
  `static_cast<SaperaGigE*>(pInfo->GetContext())` (the `this` from cpp:1029). **Replace the
  current blocking `ErrorBox(...)` call** with `LogMessage(...)` on the instance (the static
  method has no `LogMessage` of its own — that is why the instance is needed). First, under
  the lock, read `sequenceStarted_`; if false, return immediately (this is what makes stop
  clean — once `StopSequenceAcquisition()` has flipped the flag, no further frames are
  inserted). If `pInfo->IsTrash()` → buffer overflow: `LogMessage` it and return (drop frame).
  Otherwise read the **specific completed buffer** (B2) into the staging buffer `img_` and
  push via `GetCoreCallback()->InsertImage(this, pixels, w, h, bytesPerPixel)` (outside the
  lock, per the locking rule). Then **reacquire `seqLock_` to do `++imageCounter_`** —
  `imageCounter_` is sequence state, so every read/write of it goes through `seqLock_`, never
  bare; the only work outside the lock is the SDK buffer read and the `InsertImage` call. The
  callback **never** calls a Sapera stop function, `Wait()`, or `AcqFinished()`.
- **`StopSequenceAcquisition()`** (replace stub at cpp:644): the single teardown path. Under
  the lock, if `!sequenceStarted_` return `DEVICE_OK` (idempotent); else set
  `sequenceStarted_ = false` and release the lock. Then, outside the lock, `Xfer_->Freeze()` +
  `Xfer_->Wait(timeout)` and `GetCoreCallback()->AcqFinished(this, 0)`. The flag flip under the
  lock makes this run once even if called twice (engine cleanup after the user already
  stopped).
- **`IsCapturing()`** (cpp:687): return `sequenceStarted_` (read under lock). It only goes
  false in `StopSequenceAcquisition()`, so it never reports done while hardware streams.
- **`Busy()`** stays `false`. **Invariant to record in a comment:** sequence/streaming state
  is reported *only* through `IsCapturing()`; `Busy()` reflects *blocking synchronous device
  operations* (of which this callback-driven adapter has none during a sequence). A future
  maintainer must not "fix" `Busy()` to track the sequence — that is `IsCapturing()`'s job.
- **`SnapImage()`** keeps `Snap(1)`; snap is already guarded by the sequence flag
  (cpp:445, now `sequenceStarted_`) so snap and grab paths cannot collide.
- **`img_` is the single staging buffer; document its ownership.** Both snap
  (`GetImageBuffer()`, cpp:474) and the streaming callback read a completed Sapera buffer into
  `img_`. The safety argument that no lock is needed around `img_` itself: during a sequence
  **only the callback writes `img_`** — snap is rejected (`sequenceStarted_`), property
  reconfig that would resize `img_` is rejected (next bullet), and `GetImageBuffer()` is not
  used as the streaming path (frames go straight to `InsertImage`). Add a comment on `img_`
  stating this single-writer-during-sequence invariant so a maintainer does not later read
  `img_` from the MMCore thread mid-sequence.
- **Enforce "no reconfiguration mid-sequence" in the property handlers (do not merely
  assume it).** B3 requires `img_` width/height/`bytesPerPixel_` to be stable while grabbing,
  and the existing handlers call `SynchronizeBuffers()` which reallocates buffers. So every
  buffer-affecting property action — ROI/`ClearROI`, image dimensions, binning, pixel
  format, and any other handler that reaches `SynchronizeBuffers()`/`ResizeImageBuffer()` —
  must reject the change with `DEVICE_CAMERA_BUSY_ACQUIRING` (or set the property read-only)
  while `sequenceStarted_` is true, *before* touching the buffers. Audit `OnXxx` handlers in
  `SaperaGigE.cpp` for this; it is part of Checkpoint 2, not an assumption left to the
  caller.
- Repurpose the dead `InsertImage()` helper (681) or inline it into the callback. **Do
  not use the snap-loop `SequenceThread`** — the native callback replaces it; its deletion
  is Checkpoint 3.

### (B) Design questions — B1/B2 resolved by SDK inspection, B3 is an adapter-side audit

1. **Continuous grab vs stop — RESOLVED.** `Xfer_->Grab()` to start, `Xfer_->Freeze()` to
   request stop, `Xfer_->Wait(timeout)` to join — confirmed against `SapTransfer.h` and
   `Examples/Classes/GrabConsole/GrabCPP.cpp` (see "SDK verification" above). In the baseline
   design all three run **only on the MMCore thread** (`Start`/`StopSequenceAcquisition`); the
   callback never stops, so callback-thread safety of `Freeze()` is **not** required (matching
   how Aravis stops only from `StopSequenceAcquisition`, and how `GrabCPP.cpp` calls
   `Grab`/`Freeze`/`Wait` from the same thread). A `Wait` timeout of a few seconds (matching the
   existing `SnapImage()`'s 16000 ms pattern, cpp:455) is reasonable; exact value is a tuning
   choice, not a blocking unknown. Callback-safe `Freeze()` only becomes necessary if the
   optional camera-side self-limiting refinement is later adopted.
2. **Completed-buffer identity — RESOLVED, not actually racy.** The current `GetImageBuffer()`
   (cpp:474) reads `Buffers_.ReadRect(...)` with no index argument, which reads at
   `SapBuffer::GetIndex()` — documented in `SapBuffer.h` as "Index of last grabbed buffer".
   `Demos/Classes/Vc/SeqGrabDemo/SeqGrabDemoDlg.cpp` confirms this is the intended pattern:
   it reads `m_Buffers->GetIndex()` synchronously from inside/around the xfer-callback
   invocation, with no separate per-event buffer-index lookup via `pInfo`. So Checkpoint 2's
   `XferCallback` reads the just-completed frame with the **same no-index
   `Buffers_.ReadRect(...)` call already used by `GetImageBuffer()`**, called synchronously
   inside the callback (which by the demos' pattern is invoked serially, one completed buffer
   at a time) — no `pInfo`/`GetTransfer()`-based index lookup is needed. The original framing
   of this as "the highest-risk unknown" is superseded.
3. **`InsertImage` width/height/bytes during streaming — adapter-side, not an SDK question.**
   Confirm `img_` width/height/`bytesPerPixel_` are stable while grabbing (they are set in
   `SynchronizeBuffers`/`ResizeImageBuffer`). Stability is *enforced* by the property-handler
   guards added in (A); the Sapera side does not itself resize buffers mid-grab (buffer
   geometry is fixed at `Buffers_.Create()` time, cpp:1027-1031, and is not touched again until
   `SynchronizeBuffers()` is called), so this is a closed question once the (A) guards are in
   place.

**Checkpoint 2 result (target):** working live/MDA acquisition on the modern base → the
adapter is truly non-legacy. The (A) steps are now fully specified (B1/B2 confirmed against
the installed SDK headers/demos; B3 is a closed adapter-side audit). This is still an
**implementation target, not a claimed-compiling checkpoint here**: actual compilation should
be verified on the SDK box (a build-only check, no camera hardware required, would catch
signature/typo mistakes), and behavioral correctness (frame integrity, clean stop, no
mid-sequence reconfiguration) can only be validated against real camera hardware.

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

A Windows box with the Sapera LT SDK is now available (headers/libs under
`C:\Program Files\Teledyne DALSA\Sapera`), so a real build-only compile check (no camera
hardware needed) is possible and should be done once Checkpoint 1/2 edits land. Until that
build is run, verify by inspection:

1. **Pure-virtual coverage:** every `= 0;` in `CCameraBase` (DeviceBase.h:1377-1583) has a
   matching override in `SaperaGigE.h` — confirmed set: `GetImageBuffer`, `GetImageWidth`,
   `GetImageHeight`, `GetImageBytesPerPixel`, `SnapImage`, `Busy`,
   `StartSequenceAcquisition(double)`, `StartSequenceAcquisition(long,double,bool)`,
   `StopSequenceAcquisition`, `IsCapturing`.
2. **No stale overrides:** `PrepareSequenceAcqusition`, `GetPixelSizeUm`, `GetComponentName`
   absent from the adapter.
3. **Signatures bind:** `InsertImage` 5-arg call matches `CoreCallback`.
4. **Later (needs camera hardware), streaming correctness:** build on Windows with Sapera LT
   (SDK box now available); run snap, then live mode and a finite MDA. Confirm:
   - frame content correct, with **no duplicated/skipped/torn frames** (validates design
     question B2 — completed-buffer identity);
   - **finite MDA delivers exactly the requested number of frames to the application**, with
     the camera free-running and the acquisition engine stopping it (baseline = client-driven
     stop). Watch for the inverse failure too: after `StopSequenceAcquisition()` flips
     `sequenceStarted_`, **no further `InsertImage` calls occur** — i.e. extra callback frames
     that arrive during/after stop are dropped, not pushed into the buffer (validates the
     `!sequenceStarted_` early-return in the callback);
   - clean stop — both early user stop and end-of-MDA engine stop — with `AcqFinished` firing
     **exactly once** and `IsCapturing()` returning to `false`; a redundant second
     `StopSequenceAcquisition()` is a no-op;
   - **no blocking dialogs**: the old `ErrorBox` MessageBox path is gone; overflow surfaces
     as a log message;
   - no deadlock/crash under stop-while-callback-running (validates thread-safety/locking);
   - **no reconfiguration mid-sequence**: ROI/binning/pixel-format/dimension changes are
     rejected while capturing (validates the property-handler guards).
   MMDevice/MMCore CI does not cover individual adapters, so this is the only end-to-end gate.

## Scope notes

- Checkpoint 1 is compile-green and fully specified for execution now. Checkpoint 2's design
  is now fully specified (B1/B2 resolved by inspection of the installed Sapera LT SDK; B3 is a
  closed adapter-side audit) — it remains an *implementation target* whose actual compile and
  runtime behavior should still be verified on the Windows + Sapera SDK box (build-only check
  needs no hardware; behavioral correctness needs camera hardware). Checkpoint 3 is cleanup.
- All edits confined to `DeviceAdapters/SaperaGigE/{SaperaGigE.h,SaperaGigE.cpp}`.
  No DIV bump (adapter-only change).
- The scratch drafts in the repo root (the two prior PLAN_*.md files) are temporary and
  not committed; they can be deleted at any point — this document is the source of truth.

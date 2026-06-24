# Fix SaperaGigE sequence acquisition: pacing, finite length, overflow, color metadata, build configs

## Context

A separate planning pass (recorded in `PLAN.md` at the repo root) found several
correctness problems in `SaperaGigE`'s callback-driven sequence-acquisition path,
plus two branch-level bugs (default-camera selection, broken Win32 build configs).
That plan was written without access to the Sapera SDK or the live file state, so
before adopting it I verified its concrete claims (line numbers, SDK API
signatures, threading semantics, MMCore contract) against the actual SDK headers,
demo sources, and current repo state. **All of the plan's technical claims check
out**, with one important correction to its verification assumptions (see below).
This plan folds in those fixes plus the smaller, already-agreed documentation item
from the earlier color-conversion review (the `SapProcessing` divergence).

## Verification of PLAN.md's assumptions (done this session)

- **SDK availability — correction.** PLAN.md's own verification section says "The
  Sapera SDK is not on this machine" and frames the whole plan as
  review-only/uncompilable here. That's stale: the SDK **is** installed locally at
  `C:\Program Files\Teledyne DALSA\Sapera` (used in prior sessions — see
  `SaperaGigE.vcxproj` can be built standalone via the MSBuild command already
  established in past work), and this session read its real headers directly
  (`SapTransfer.h`, `SapAcqDevice.h`, `SapFeature.h`, etc.) to confirm every API
  claim below. **Local build verification (Debug|x64, standalone MSBuild) is
  possible and should be done as part of this work.** Only hardware-dependent
  behavior (actual frame pacing, overflow, a frame-rate-capable camera, the color
  sensor) requires the physical camera and can't be exercised here.
- **`Grab()`/`Snap(n)` distinction**, **`IsTrash()`**, **`SapAcqDevice::IsFeatureAvailable
  /GetFeatureValue/SetFeatureValue/GetFeatureInfo`**, **`SapFeature::GetMin/GetMax`** —
  all confirmed to exist with the signatures the plan assumes (`SapTransfer.h:544-545,643`,
  `SapAcqDevice.h:88,90,97,117`, `SapFeature.h:136-148`).
- **Callback deadlock claim** ("never call `Freeze()`/`Wait()`/`AcqFinished()` from inside
  `XferCallback`") — not a hard documented SDK rule, but consistent with every demo's
  pattern (callbacks never block on the same transfer's `Wait()`; that's always done
  from a different thread/context) and with the *existing* code comment already in
  `SaperaGigE.cpp:1200-1209` from a prior session. Treat as a sound inference, not a
  literal SDK doc citation.
- **`AcquisitionFrameRate`/`AcquisitionFrameRateEnable`/`AcquisitionFrameRateControlMode`** —
  not findable anywhere in this SDK install (demos/headers), which is expected: GenICam
  feature names are defined by each camera's own XML, not the SDK. This remains
  genuinely unverifiable without the physical camera. The plan already designs for
  this correctly (fail-soft, `IsFeatureAvailable`-guarded, never fails `Initialize()`)
  — no change needed, just flagging that this item's *runtime* correctness still needs
  a real camera even though the *code* can be written and compiled now.
- **MMDevice/MMCore contract** — `MM::Core::InsertImage` 6-arg overload (with explicit
  `nComponents`) vs. the 5-arg overload (assumes `nComponents == 1`,
  `MMDevice.h` doc comment confirmed), the "stop on any `InsertImage` error" contract
  (`MMDevice.h:1712-1717`), and that MMCore does **not** auto-stop finite acquisitions —
  all confirmed. `DemoCamera.cpp`'s `MySequenceThread::svc()` (`:1107`) and
  `OnThreadExiting()` (`:1043`, calls `AcqFinished`) confirm the adapter-owns-the-count
  pattern the plan models against.
- **Current `SaperaGigE.cpp` state** — re-read directly: `XferCallback` (`:1211-1253`)
  already has the `IsTrash()` drop (`:1224-1229`) exactly where the plan's pacing gate
  needs to be inserted (right after it, before the demosaic/`ReadRect`/`InsertImage`
  block at `:1231`); the `InsertImage` call (`:1245-1246`) is confirmed 5-arg with no
  `nComponents`, and `imageCounter_` is currently incremented unconditionally
  regardless of `InsertImage`'s return value — confirming fix D is needed exactly as
  described. `StopSequenceAcquisition()` (`:688-707`) is confirmed fully synchronous
  today (`Freeze`/`Wait(5000)`/`AcqFinished` inline, no worker thread) — confirming the
  worker-thread refactor in fix E is new work, not already partially done.
  `activeDevice_` is confirmed never assigned a default (only set in `OnCamera`'s
  `AfterSet`); the `loc_` local-shadowing-member bug at `:238` is confirmed exactly as
  described. The `.vcxproj`/`.sln` Win32/x86 config breakage (unconditional
  `Lib\Win64\SapClassBasic.lib`, missing Sapera include dirs on `Release|Win32`, and
  the exact `x86` config-mapping line numbers in the `.sln`) is confirmed exactly as
  cited.

Net: PLAN.md's design is sound and can proceed as written. The only update is the
verification section (local build is possible) and folding in the documentation item
below.

## Key architectural constraint

The callback's existing design rule (`SaperaGigE.cpp:1200-1209`) — it must never
call `Freeze()`/`Wait()`/`AcqFinished()` — is correct: Sapera serializes transfer
callbacks, so calling `Xfer_->Wait()` from inside the callback risks deadlock.
Self-stop (on `numImages` reached or `InsertImage` error) must **request** a stop
and let teardown run on a different thread.

## Stop-worker ownership model (single, explicit)

Exactly one model, to avoid self-join / direct-teardown races:

- **Teardown runs in one place only: the worker thread.** Factor the body of the
  current `StopSequenceAcquisition()` into a private `performTeardown_()` that is
  idempotent: lock `seqLock_`, if `!sequenceStarted_` return, set
  `sequenceStarted_ = false`, unlock, then `Freeze` + `Wait(5000)` + `AcqFinished`.
  (Calling `AcqFinished` off the MMCore thread is the established pattern —
  `DemoCamera::OnThreadExiting`.)
- **`stopRequested_` synchronization** — guarded **solely** by the worker's CV mutex
  (`stopMutex_`), never by `seqLock_`. Every read/write/reset happens under
  `stopMutex_` (or make it `std::atomic<bool>` *and* still notify under `stopMutex_`
  so the wait/notify can't be missed). Do not touch it inside the `seqLock_` block.
- **`RequestStop()`** — locks `stopMutex_`, sets `stopRequested_ = true`, unlocks,
  notifies the CV. Non-blocking, idempotent, safe from the callback thread.
- **Worker thread** (started in `StartSequenceAcquisition`) — takes `stopMutex_`,
  waits on the CV until the `stopRequested_` predicate holds, then **releases
  `stopMutex_` before** calling `performTeardown_()` exactly once, then exits. Never
  hold `stopMutex_` across `Freeze`/`Wait`/`AcqFinished` — that would block
  `RequestStop()` for the whole teardown. Checking the predicate after acquiring
  `stopMutex_` means a stop requested before the worker reaches its wait is observed
  immediately (no lost-wakeup).
- **`StopSequenceAcquisition()` (the MM API)** — calls `RequestStop()`; then, if the
  worker is joinable **and the caller is not the worker thread itself**
  (`std::this_thread::get_id()` guard), joins it. After join, the hardware is
  guaranteed stopped before return. No-op if no worker exists.
- **`StartSequenceAcquisition`** — strict ordering to reject re-entry and avoid a
  lock-order deadlock (the prior worker's `performTeardown_()` also takes
  `seqLock_`, so a join must never happen while `seqLock_` is held; a *live* worker
  is blocked on the CV, so joining it would hang forever):
  1. take `seqLock_`, check `sequenceStarted_`; if true, release and return
     `DEVICE_CAMERA_BUSY_ACQUIRING`; otherwise release the lock;
  2. join any prior (already finished, not-yet-joined) worker — safe here since
     step 1 guarantees no sequence is active — **before** re-taking `seqLock_`;
  3. reset `stopRequested_ = false` under `stopMutex_`;
  4. `Grab` + `PrepareForAcq` (undo `Grab` on failure);
  5. take `seqLock_` only to reset sequence state (`imageCounter_ = 0`,
     `intervalMs_`, `numImages_`, `nextFrameTime_`, `sequenceStarted_ = true`) and
     release it;
  6. start the fresh worker **outside** `seqLock_`.
- **`Shutdown()` / destructor** — `RequestStop()` then join if joinable, before any
  SDK objects are destroyed (no thread referencing `this` after teardown).

A *self-stop* (callback `RequestStop`) tears down on the worker but leaves it
unjoined; it's joined by the subsequent `StartSequenceAcquisition`, MMCore's
follow-up `StopSequenceAcquisition`, or `Shutdown` — whichever comes first. A *user
stop* both requests and joins.

Use the project's existing thread primitives (`DeviceThreads.h`, already included —
confirmed it provides `MMThreadLock`/`MMThreadGuard`, already used at
`SaperaGigE.cpp:470,732,755,764,791`) or `std::thread` + `std::mutex` +
`std::condition_variable` — match what the adapter already links.

## Changes

### A. State + members (`SaperaGigE.h`)

Add, with scalar fields read/written under the existing `seqLock_` (extend its
comment):
- `double intervalMs_;` — requested min frame spacing; `<= 0` = deliver every frame.
- `MM::MMTime nextFrameTime_;` — earliest delivery time of the next frame.
- `long numImages_;` — requested finite length (`LONG_MAX` for live/unbounded).
- Worker/stop members: worker thread handle, `stopRequested_` flag, condition
  variable + `stopMutex_`. `stopRequested_` is guarded by `stopMutex_` only — **not**
  `seqLock_`; make it `std::atomic<bool>` or always access under `stopMutex_`.
- `void RequestStop();` and `void performTeardown_();` private helpers.

Initialize scalar members in the constructor. (`stopOnOverflow` is not stored —
see section D.)

### B. Software frame-pacing gate (fix #1) — `StartSequenceAcquisition` + `XferCallback`

In `StartSequenceAcquisition(long, double, bool)` (`SaperaGigE.cpp:729`), follow the
6-step ordering above: busy-check under `seqLock_`; join any prior finished worker
with no lock held; reset `stopRequested_` under `stopMutex_`; `Grab` + `PrepareForAcq`
(undo `Grab` on failure); take `seqLock_` only to set `intervalMs_`, `numImages_`,
`nextFrameTime_ = GetCurrentMMTime()` (first frame due immediately), `imageCounter_ =
0`, `sequenceStarted_ = true`, release; start the fresh worker **outside**
`seqLock_`.

In `XferCallback` (`SaperaGigE.cpp:1211`), insert the pacing gate right after the
existing `IsTrash()` drop (`:1224-1229`) and before the demosaic/`ReadRect`/
`InsertImage` block (`:1231`), under `seqLock_`: if `intervalMs_ > 0.0` and
`GetCurrentMMTime() < nextFrameTime_`, drop the frame (`return`); otherwise set
`nextFrameTime_ = GetCurrentMMTime() + MM::MMTime::fromMs(intervalMs_)` and proceed.
Advance from "now" (not the previous deadline) to avoid catch-up bursts. Only
*delivered* frames advance the deadline and the counter (section D).

### C. Correct InsertImage component count (fix #3) — `XferCallback`

Replace the current 5-arg `InsertImage(...)` call (`SaperaGigE.cpp:1245-1246`,
confirmed to omit `nComponents`) with the 6-arg overload (confirmed to exist in
`MMDevice.h`) passing `self->GetNumberOfComponents()` (4 for color, 1 for mono, per
`SaperaGigE.cpp:550-552`). `bytePerPixel` already reflects the converted RGB buffer
via `GetImageBytesPerPixel()`.

### D. Finite-length + overflow self-stop (fixes #2 and #4) — `XferCallback`

Evaluate the return of **every** `InsertImage` call (currently `imageCounter_` is
incremented unconditionally regardless of the result — confirmed bug):
- if `ret != DEVICE_OK` (covers `DEVICE_BUFFER_OVERFLOW` and any genuine error,
  per the confirmed `MMDevice.h:1712-1717` contract "stop on any InsertImage
  error"): log, `self->RequestStop()`, and `return` **without** incrementing
  `imageCounter_` — a frame that never reached MMCore must not count toward the
  finite total.
- only on `ret == DEVICE_OK`: under `seqLock_`, `++imageCounter_`; if
  `imageCounter_ >= numImages_`, `RequestStop()`.

Reject `numImages <= 0` up front in `StartSequenceAcquisition(long, double, bool)`
with `DEVICE_INVALID_INPUT_PARAM` (before `Grab`/arming), even though MMCore
normally passes positive counts. The live overload (`SaperaGigE.cpp:713-718`,
confirmed it forwards to the counted overload with `LONG_MAX`) is unaffected.
`stopOnOverflow` need not be stored: the actionable rule is simply "stop on any
`InsertImage` error."

### E. Teardown reuse + lifecycle — `StopSequenceAcquisition`, `Shutdown`

Refactor `StopSequenceAcquisition()` (`SaperaGigE.cpp:688-707`, confirmed currently
fully synchronous/inline with no worker thread) to the ownership model above: it
calls `RequestStop()` and joins the worker (unless on the worker thread). Move the
actual `Freeze`/`Wait`/`AcqFinished` into `performTeardown_()`. On teardown, reset
`intervalMs_ = 0.0` and `numImages_` to its default. Update `Shutdown()`/destructor
to request-stop and join.

### F. `AcquisitionFrameRate` as a real property — dedicated setup + handler

Do **not** add it to the generic `deviceFeatures` loop with a NULL action (that
only caches the value; a `GetFeatureValue` failure there returns `DEVICE_ERR` and
fails `Initialize()`). Add a self-contained, fail-soft setup modeled on
`SetUpBinningProperties()`:

- New `int SetUpFrameRateProperty();` called from `Initialize()`.
- `IsFeatureAvailable("AcquisitionFrameRate")`; if false, log and return
  `DEVICE_OK` (skip).
- Attempt `GetFeatureValue`; on failure, log and return `DEVICE_OK` (skip — do not
  fail `Initialize()`).
- `CreateProperty("AcquisitionFrameRate", value, MM::Float, false, action)` with a
  new `OnAcquisitionFrameRate` handler modeled on `OnGain` (`SaperaGigE.cpp:1007-1030`,
  confirmed signature/pattern): `AfterSet` → `SetFeatureValue`; `BeforeGet` →
  `GetFeatureValue`. Optionally set property limits from `GetFeatureInfo`'s
  `SapFeature::GetMin/GetMax` (confirmed available, `SapFeature.h:136-148`).
- Best-effort, guarded enable: in `OnAcquisitionFrameRate`'s `AfterSet`, if
  `AcquisitionFrameRateEnable` (bool) or `AcquisitionFrameRateControlMode` (enum
  `Programmable`) is available, set it before writing the rate; both guarded by
  `IsFeatureAvailable`. **Genuinely untestable without the physical camera** — these
  exact GenICam feature names could not be found anywhere in the local SDK install
  (expected: they're camera-firmware-defined, not SDK-defined) — document this
  explicitly in the property's code comment.

### G. Default camera selection (fix #5) — `Initialize()`

At the property creation site (confirmed at `SaperaGigE.cpp:158-166`), assign
`activeDevice_ = acqDeviceList_[0];` alongside the existing
`CreateProperty(g_CameraServer, acqDeviceList_[0].c_str(), ...)` call, so the
default-camera path has a non-empty device even when `OnCamera` is never invoked
(confirmed `activeDevice_` is currently only set in `OnCamera`'s `AfterSet`). Fix
the confirmed `loc_` shadowing at `SaperaGigE.cpp:238` (`SapLocation loc_(...)`
currently declares a local shadowing the header's member `loc_`): assign the
member instead, so `Shutdown`'s `loc_.GetServerName()` log (`:419`, confirmed) is
meaningful.

### H. Win32 build configurations (fix #6) — `SaperaGigE.vcxproj` + `SaperaGigE.sln`

Confirmed: `SapClassBasic.lib` is referenced in an unconditioned `ItemGroup`
(`:160`) pointing at `Lib\Win64`, so the advertised `Win32` configs link the 64-bit
library; `Release|Win32` (`:118-134`) also lacks the Sapera include dirs present in
every other config (e.g. `Release|x64:144`) and won't compile. Micro-Manager is
built x64-only. Remove the unsupported `Win32` build across **both** files:

- `SaperaGigE.vcxproj`: remove the `Debug|Win32` and `Release|Win32`
  `ProjectConfiguration`s (confirmed at lines 4, 12) and their associated
  `PropertyGroup`/`ImportGroup`/`ItemDefinitionGroup` blocks, leaving only the
  `x64` configs that correctly reference `Lib\Win64\SapClassBasic.lib` and the
  Sapera include dirs.
- `SaperaGigE.sln`: remove the `x86` entries (confirmed at lines 13, 15 in
  `SolutionConfigurationPlatforms`; lines 20-21, 24-25 for the `SaperaGigE`
  project and 28-29, 32-33 for `MMDevice-SharedRuntime` in
  `ProjectConfigurationPlatforms`). Otherwise the solution advertises x86 configs
  pointing at project configs that no longer exist.

(Alternative if 32-bit must stay: make the `SapClassBasic.lib` reference and
include dirs platform-conditional — `Lib\Win32` for `Win32`. Removal is preferred
as it matches the rest of the project.)

### I. Comments + docs

- Rewrite the now-incorrect comments in `StartSequenceAcquisition`
  (`SaperaGigE.cpp:721-728`, confirmed exact text: claims a "proven Aravis
  client-stop pattern" where MMCore drains `numImages` frames and stops the
  adapter — this is incorrect, MMCore does not auto-stop) and `XferCallback`
  (`:1200-1209`): state the adapter self-stops finite/overflow acquisitions via
  the deferred worker, the callback paces frames and tags components, and
  teardown is worker-only.
- **Document the `Conv_`/`SapProcessing` divergence** (carried over from the
  earlier color-conversion review): extend the existing comments above
  `Conv_->Convert()` in `GetImageBuffer()` (`:503-509`) and `XferCallback`
  (`:1231-1237`) noting that the SDK demos drive `Convert()` asynchronously via a
  `SapProcessing` helper (`Execute()`/`ProCallback`) to keep a live-preview UI
  thread unblocked, and that this adapter intentionally calls `Convert()`
  synchronously instead because it has no live preview and both call sites need
  the converted buffer ready immediately — `SapProcessing` exposes no `Wait()` on
  a specific buffer index, so adopting it would require adding new
  synchronization, not just swapping the call. No functional change for this item.
- Match the codebase's `/** ... */` banner-comment convention for new/edited
  functions (`performTeardown_`, `RequestStop`, `OnAcquisitionFrameRate`,
  `SetUpFrameRateProperty`), noting "Required by the MM::Camera API" where
  applicable, consistent with `DemoCamera.cpp` and existing `SaperaGigE` docs
  (e.g. `SnapImage`, `:461`). Keep new inline comments brief; trim verbose
  existing ones where the rewrite touches them.
- Update `HOT_CAMERA.md`: record that `interval_ms` pacing, finite-length stop,
  overflow stop, and color metadata are fixed, and that `AcquisitionFrameRate` is
  now a real property (with the caveat that its actual effect is untested against
  hardware).

### Naming (recommendation, not a code change)

Keep the SDK-based name `SaperaGigE` rather than renaming to "Teledyne". SDK-based
adapter names are the MM convention (`PVCAM`, `Spinnaker`/`SpinnakerC`,
`AndorSDK3`, `Aravis`). "Teledyne" is ambiguous — Teledyne also owns FLIR
(`Spinnaker`), Photometrics/Princeton (`PVCAM`), and Lumenera (`Lumenera`), which
already exist as separate adapters. A rename would also break existing user
device configs, so it's out of scope unless explicitly requested.

## Files modified

- `DeviceAdapters/SaperaGigE/SaperaGigE.h` — new members, worker/stop helpers,
  `OnAcquisitionFrameRate`/`SetUpFrameRateProperty` decls, comments.
- `DeviceAdapters/SaperaGigE/SaperaGigE.cpp` — constructor init,
  `StartSequenceAcquisition`, `XferCallback`, `StopSequenceAcquisition` +
  `performTeardown_`/`RequestStop`, `Shutdown`/destructor, `SetUpFrameRateProperty`
  + `OnAcquisitionFrameRate`, default-camera assignment + `loc_` member fix in
  `Initialize`, comment rewrites (including the `Conv_`/`SapProcessing` note).
- `DeviceAdapters/SaperaGigE/SaperaGigE.vcxproj` and
  `DeviceAdapters/SaperaGigE/SaperaGigE.sln` — remove the broken `Win32`/`x86`
  configurations from both.
- `HOT_CAMERA.md` — status update.

## Verification

The SDK **is** available locally, so local build verification is possible —
correcting PLAN.md's original "cannot be compiled here" assumption:

1. **Build locally first**: standalone MSBuild of `SaperaGigE.vcxproj`
   (Debug|x64, `-p:SAPERADIR="C:\Program Files\Teledyne DALSA\Sapera"`, the
   command already established in prior sessions) — confirms the code compiles
   against the real SDK headers and the `.vcxproj` Win32 removal didn't break the
   x64 configs. Then a full Micro-Manager Windows build (`micromanager.sln`) to
   confirm the `.sln` change doesn't break the broader solution.
2. The remaining items genuinely require the physical Genie GigE camera(s) and
   cannot be exercised on this machine:
   - **Default camera**: `Initialize()` succeeds with the pre-init property left
     at its default; `Shutdown` log shows the real server name.
   - **Finite length**: `startSequenceAcquisition(N, interval, false)` returns
     exactly `N` frames and self-stops; `isSequenceRunning()` goes false without a
     manual stop.
   - **Timing**: long interval (e.g. 2000 ms) → frames ~2 s apart; first frame due
     immediately, so an N-frame run takes about `(N-1) × interval`. `interval = 0`
     (live) delivers at full rate.
   - **Color**: sequence frames are RGB32 (not GRAY32) on the color sensor.
   - **Overflow/errors**: forced overflow (`stopOnOverflow = true`, slow consumer)
     stops acquisition cleanly; the failed frame isn't counted toward the finite
     total.
   - **Stop lifecycle**: mid-run user stop is clean (no deadlock, worker joined);
     back-to-back sequences work; `Shutdown` during a run is clean.
   - **Early-stop race**: a very short/immediately-stopped sequence (e.g.
     `numImages = 1`, or a stop right after start) tears down exactly once, no
     hang, no lost wakeup.
   - **`AcquisitionFrameRate` property**: appears and actually changes the rate on
     a supporting camera; silently absent (logged) on one without it,
     `Initialize()` still succeeds either way.

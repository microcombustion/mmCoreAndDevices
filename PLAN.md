# Fix SaperaGigE sequence acquisition: pacing, finite length, overflow, color metadata

## Context

The callback-driven sequence path in `SaperaGigE` has several correctness
problems on common acquisition paths. The camera is put into free-running
continuous acquisition (`Xfer_->Grab()`, `SaperaGigE.cpp:738`) and the native
Sapera transfer callback `XferCallback` (`SaperaGigE.cpp:1211`) pushes frames
into the MMCore circular buffer. The current code comments assert that MMCore
auto-stops finite acquisitions ("the proven Aravis client-stop pattern") — that
assertion is **incorrect**, as confirmed against the `DemoCamera` reference
adapter and the `InsertImage` contract in `MMDevice/MMDevice.h`.

Problems to fix (verified):

1. **`interval_ms` ignored** — `StartSequenceAcquisition(numImages, interval_ms,
   stopOnOverflow)` never reads `interval_ms`; frames are delivered back-to-back
   at the native rate instead of spaced by the requested interval. (Original
   request — a "timed" sequence isn't actually timed.)
2. **[P1] `numImages` ignored** — MMCore does *not* stop the camera after
   `numImages` frames; the adapter must stop and call `AcqFinished()` itself
   (see `DemoCamera.cpp:1107` / `:1043`). As written, finite acquisitions grab
   indefinitely and `IsCapturing()` stays true.
3. **[P2] Wrong color component metadata** — the callback uses the 5-arg
   `InsertImage` overload, which "Assumes nComponents == 1"
   (`MMDevice.h:1726-1731`). Color (4-byte BGRx) frames get tagged GRAY32
   instead of RGB32 and are saved/interpreted with the wrong pixel type.
4. **[P2] No stop on buffer overflow** — the contract states cameras "should
   always just stop the acquisition if InsertImage() returns any error"
   (`MMDevice.h:1712-1717`). With `stopOnOverflow == true`, `InsertImage()`
   returns `DEVICE_BUFFER_OVERFLOW`; the callback only logs it and keeps
   grabbing.

Branch-level review (against `main`) surfaced two further blocking bugs in the
new adapter, folded in here:

5. **[P1] Empty default camera selection** — `activeDevice_` is assigned only in
   `OnCamera`'s `AfterSet`, but `CreateProperty(g_CameraServer, ...)`
   (`SaperaGigE.cpp:161`) only stores the default and never fires the action. In
   the normal default path `activeDevice_` stays `""`, so `Initialize()` builds
   `SapLocation loc_(activeDevice_.c_str())` (`SaperaGigE.cpp:238`) from an empty
   string and can fail to initialize. (That line also shadows the member `loc_`
   with a local, leaving the member used in `Shutdown`'s log (`:419`) default-
   constructed — fixed in passing.)
6. **[P2] Broken Win32 build configs** — `SapClassBasic.lib` is referenced in an
   unconditioned `ItemGroup` (`SaperaGigE.vcxproj:160`) pointing at
   `Lib\Win64`, so the advertised `Win32` configs link the 64-bit library;
   `Release|Win32` (`:118-134`) also lacks the Sapera include dirs and won't
   compile.

Also (secondary): expose the GenICam `AcquisitionFrameRate` feature as a real,
writable property so a capable camera can cap hardware readout (per
`HOT_CAMERA.md`). This is **not** zero-risk and cannot be validated here (no
SDK), so it is implemented as a self-contained, robustly-guarded property that
never jeopardizes `Initialize()` — see section F.

Reference patterns: `DeviceAdapters/DemoCamera/DemoCamera.cpp`
(`StartSequenceAcquisition`, `MySequenceThread::svc`, `OnThreadExiting`,
`InsertImage`). `MM::MMTime` API: `MMDevice/MMDevice.h` (`fromMs`, `operator+`,
`operator<`). `GetCurrentMMTime()`: `DeviceBase.h:1223`. `GetNumberOfComponents()`
returns `4` for color, `1` for mono (`SaperaGigE.cpp:550`). Float get/set action
template: `OnGain` (`SaperaGigE.cpp:1007`).

## Key architectural constraint

The callback's existing design rule (`SaperaGigE.cpp:1200-1209`) — it must never
call `Freeze()`/`Wait()`/`AcqFinished()` — is correct: Sapera serializes
transfer callbacks, so calling `Xfer_->Wait()` from inside the callback
deadlocks. Therefore self-stop (on `numImages` reached or `InsertImage` error)
must **request** a stop and let teardown run on a different thread.

## Stop-worker ownership model (single, explicit)

Exactly one model, to avoid self-join / direct-teardown races:

- **Teardown runs in one place only: the worker thread.** Factor the body of the
  current `StopSequenceAcquisition()` into a private `performTeardown_()` that is
  idempotent: lock `seqLock_`, if `!sequenceStarted_` return, set
  `sequenceStarted_ = false`, unlock, then `Freeze` + `Wait(5000)` +
  `AcqFinished`. (Calling `AcqFinished` off the MMCore thread is the established
  pattern — `DemoCamera::OnThreadExiting`.)
- **`stopRequested_` synchronization** — `stopRequested_` is the worker's
  condition-variable predicate and is guarded **solely by the worker's CV
  mutex** (`stopMutex_`), never by `seqLock_`. Every read/write/reset of it
  happens under `stopMutex_` (or make it `std::atomic<bool>` *and* still notify
  under `stopMutex_` so the wait/notify can't be missed). Do not touch it inside
  the `seqLock_` block.
- **`RequestStop()`** — locks `stopMutex_`, sets `stopRequested_ = true`,
  unlocks, notifies the CV. Non-blocking, idempotent, safe from the callback
  thread.
- **Worker thread** (started in `StartSequenceAcquisition`) — takes `stopMutex_`,
  waits on the CV until the `stopRequested_` predicate holds, then **releases
  `stopMutex_` before** calling `performTeardown_()` exactly once, then exits.
  Never hold `stopMutex_` across `Freeze`/`Wait`/`AcqFinished` — doing so would
  block `RequestStop()` (from `StopSequenceAcquisition`/`Shutdown`) for the whole
  teardown. Because the predicate is checked after acquiring `stopMutex_`, a stop
  requested before the worker reaches its wait is observed immediately and the
  worker does not block (no lost-wakeup).
- **`StopSequenceAcquisition()` (the MM API)** — calls `RequestStop()`; then, if
  the worker is joinable **and the caller is not the worker thread itself**
  (`std::this_thread::get_id()` guard), joins it. After join, the hardware is
  guaranteed stopped before return. No-op if no worker exists.
- **`StartSequenceAcquisition`** — strict ordering to reject re-entry and avoid a
  lock-order deadlock (the prior worker's `performTeardown_()` also takes
  `seqLock_`, so a join must never happen while `seqLock_` is held; and a *live*
  worker is blocked on the CV, so joining it would hang forever):
  1. take `seqLock_`, check `sequenceStarted_`; if true, release and return
     `DEVICE_CAMERA_BUSY_ACQUIRING`; otherwise release the lock;
  2. join any prior (already finished, not-yet-joined) worker — now safe, since
     step 1 guarantees no sequence is active — **before** re-taking `seqLock_`;
  3. reset `stopRequested_ = false` under `stopMutex_`;
  4. `Grab` + `PrepareForAcq` (undo `Grab` on failure);
  5. take `seqLock_` only to reset sequence state (`imageCounter_ = 0`,
     `intervalMs_`, `numImages_`, `nextFrameTime_`, `sequenceStarted_ = true`)
     and release it;
  6. start the fresh worker **outside** `seqLock_`.
- **`Shutdown()` / destructor** — `RequestStop()` then join if joinable, before
  any SDK objects are destroyed (no thread referencing `this` after teardown).

This means a *self-stop* (callback `RequestStop`) tears down on the worker but
leaves the worker unjoined; it is joined by the subsequent
`StartSequenceAcquisition`, MMCore's follow-up `StopSequenceAcquisition`, or
`Shutdown` — whichever comes first. A *user stop* both requests and joins.

Use the project's existing thread primitives (`DeviceThreads.h` /
`MMDeviceThreadBase`, already included) or `std::thread` + `std::mutex` +
`std::condition_variable` — match what the adapter already links.

## Changes

### A. State + members (`SaperaGigE.h`)

Add, with the scalar fields read/written under the existing `seqLock_` (extend
its comment):
- `double intervalMs_;` — requested min frame spacing; `<= 0` = deliver every
  frame.
- `MM::MMTime nextFrameTime_;` — earliest delivery time of the next frame.
- `long numImages_;` — requested finite length (`LONG_MAX` for live/unbounded).
- Worker/stop members: the worker thread handle, the `stopRequested_` flag, and
  its condition variable + `stopMutex_` (or the chosen `DeviceThreads.h`
  equivalent). `stopRequested_` is guarded by `stopMutex_` only — **not** by
  `seqLock_` (see the synchronization rule in the ownership-model section); make
  it `std::atomic<bool>` or always access it under `stopMutex_`.
- `void RequestStop();` and `void performTeardown_();` private helpers.

Initialize the scalar members in the constructor.
(`stopOnOverflow` is not stored — see section D.)

### B. Software frame-pacing gate (fix #1) — `StartSequenceAcquisition` + `XferCallback`

In `StartSequenceAcquisition(long, double, bool)` (`SaperaGigE.cpp:729`), follow
the exact 6-step ordering in the "Stop-worker ownership" section: busy-check
`sequenceStarted_` under `seqLock_` and return `DEVICE_CAMERA_BUSY_ACQUIRING` if
running; join any prior (finished) worker with no lock held; reset
`stopRequested_` under `stopMutex_`; `Grab` + `PrepareForAcq` (undo `Grab` on
failure); take `seqLock_` only to set the sequence state (`intervalMs_ =
interval_ms;`, `numImages_ = numImages;`, `nextFrameTime_ = GetCurrentMMTime();`
first frame due immediately, `imageCounter_ = 0`, `sequenceStarted_ = true`) and
release it; then start the fresh worker **outside** `seqLock_`. Note
`stopRequested_` is reset under `stopMutex_`, never inside the `seqLock_` block.

In `XferCallback` (`SaperaGigE.cpp:1211`), after the `sequenceStarted_` check and
the `IsTrash()` drop, before the demosaic/`ReadRect`/`InsertImage` work, add a
pacing gate under `seqLock_`: if `intervalMs_ > 0.0` and
`GetCurrentMMTime() < nextFrameTime_`, drop the frame (`return`); otherwise set
`nextFrameTime_ = GetCurrentMMTime() + MM::MMTime::fromMs(intervalMs_)` and
proceed. Advance from "now" (not the previous deadline) to avoid catch-up
bursts. Only *delivered* frames advance the deadline and the counter (section D).

### C. Correct InsertImage component count (fix #3) — `XferCallback`

Replace the 5-arg `InsertImage(...)` call (`SaperaGigE.cpp:1245-1246`) with the
6-arg overload passing `self->GetNumberOfComponents()` (4 for color, 1 for
mono). `bytePerPixel` already reflects the converted RGB buffer via
`GetImageBytesPerPixel()`.

### D. Finite-length + overflow self-stop (fixes #2 and #4) — `XferCallback`

Evaluate the return of **every** `InsertImage` call; do not pre-label the path
"successful":
- `ret = GetCoreCallback()->InsertImage(self, ..., self->GetNumberOfComponents())`.
- if `ret != DEVICE_OK` (covers `DEVICE_BUFFER_OVERFLOW` and any genuine error):
  log, `self->RequestStop()`, and `return` **without** incrementing
  `imageCounter_` — a frame that never reached MMCore must not count toward the
  finite total.
- only on `ret == DEVICE_OK`: under `seqLock_`, `++imageCounter_`; if
  `imageCounter_ >= numImages_`, `RequestStop()`.

Handle `numImages <= 0` deliberately in `StartSequenceAcquisition(long, double,
bool)` even though MMCore normally passes positive counts: reject it up front
with `DEVICE_INVALID_INPUT_PARAM` (before `Grab`/arming) so a zero/negative
request never starts the hardware or accidentally delivers a frame. (The live
overload at `SaperaGigE.cpp:713` passes `LONG_MAX`, so live view is unaffected.)

`numImages_` defaults to `LONG_MAX` for the live overload
(`SaperaGigE.cpp:713`), so live view never self-stops on count. `stopOnOverflow`
need not be stored: MMCore configures the circular-buffer overflow behavior, and
the actionable rule for the adapter is simply "stop on any `InsertImage` error".

### E. Teardown reuse + lifecycle — `StopSequenceAcquisition`, `Shutdown`

Refactor `StopSequenceAcquisition()` (`SaperaGigE.cpp:688`) to the model in the
"Stop-worker ownership" section: it calls `RequestStop()` and joins the worker
(unless on the worker thread). Move the actual `Freeze`/`Wait`/`AcqFinished`
into `performTeardown_()`. On teardown, reset `intervalMs_ = 0.0` and
`numImages_` to its default. Update `Shutdown()`/destructor to request-stop and
join.

### F. `AcquisitionFrameRate` as a real property — dedicated setup + handler

Do **not** add it to the generic `deviceFeatures` loop with a NULL action (that
only caches the value; user changes would never call `SetFeatureValue`, and a
`GetFeatureValue` failure in the loop returns `DEVICE_ERR` and fails
`Initialize()`). Instead, add a self-contained, fail-soft setup modeled on
`SetUpBinningProperties()`:

- New `int SetUpFrameRateProperty();` called from `Initialize()`.
- `IsFeatureAvailable("AcquisitionFrameRate")`; if false, log and return
  `DEVICE_OK` (skip).
- Attempt `GetFeatureValue`; on failure, log and return `DEVICE_OK` (skip — do
  not fail `Initialize()`).
- `CreateProperty("AcquisitionFrameRate", value, MM::Float, false, action)` with
  a new `OnAcquisitionFrameRate` handler modeled on `OnGain`
  (`SaperaGigE.cpp:1007`): `AfterSet` → `SetFeatureValue` (Hz, no unit
  conversion); `BeforeGet` → `GetFeatureValue`. Optionally set property limits
  from `GetFeatureInfo` min/max.
- Best-effort, guarded enable so the value actually takes effect: in
  `OnAcquisitionFrameRate` `AfterSet`, if `AcquisitionFrameRateEnable`
  (bool) or `AcquisitionFrameRateControlMode` (enum `Programmable`) is available,
  set it before writing the rate. Both guarded by `IsFeatureAvailable`; absence
  is fine. (This path is untestable here and is documented as such.)

### G. Default camera selection (fix #5) — `Initialize()`

At the property creation site (`SaperaGigE.cpp:158-163`), assign
`activeDevice_ = acqDeviceList_[0];` alongside `CreateProperty(g_CameraServer,
acqDeviceList_[0].c_str(), ...)` so the default-camera path has a non-empty
device even when `OnCamera` is never invoked. While here, fix the `loc_`
shadowing at `SaperaGigE.cpp:238`: assign the member (`loc_ =
SapLocation(activeDevice_.c_str());`) instead of declaring a local of the same
name, so `Shutdown`'s `loc_.GetServerName()` log (`:419`) is meaningful.

### H. Win32 build configurations (fix #6) — `SaperaGigE.vcxproj` + `SaperaGigE.sln`

Micro-Manager is built x64-only. Remove the unsupported `Win32` build across
**both** files so no stale/broken config remains:

- `SaperaGigE.vcxproj`: remove the `Debug|Win32` and `Release|Win32`
  `ProjectConfiguration`s and their associated
  `PropertyGroup`/`ImportGroup`/`ItemDefinitionGroup` blocks
  (`SaperaGigE.vcxproj:4-6, 12-14, 29-..., 41-..., 57-..., 67-..., 78-..., 84-...,
  90-103, 118-134`), leaving only the `x64` configs that correctly reference
  `Lib\Win64\SapClassBasic.lib` and the Sapera include dirs.
- `SaperaGigE.sln`: remove the `x86` entries that map to `Win32`
  (`SaperaGigE.sln:13, 15` in `SolutionConfigurationPlatforms`, and the
  `*.Debug|x86.*` / `*.Release|x86.*` lines `20-21, 24-25, 28-29, 32-33` in
  `ProjectConfigurationPlatforms` for both the `SaperaGigE` and
  `MMDevice-SharedRuntime` projects). Otherwise the solution advertises x86
  configs pointing at project configs that no longer exist.

(Alternative if 32-bit must stay: make the `SapClassBasic.lib` reference and
include dirs platform-conditional — `Lib\Win32` for `Win32`. Removal is
preferred as it matches the rest of the project.)

### I. Comments + docs

- Rewrite the now-incorrect comments in `StartSequenceAcquisition`
  (`SaperaGigE.cpp:721-728`) and `XferCallback` (`SaperaGigE.cpp:1200-1209`):
  the adapter self-stops finite/overflow acquisitions via the deferred worker,
  the callback paces frames and tags components, and teardown is worker-only.
- **Match the codebase comment/docstring convention.** New and edited functions
  (`performTeardown_`, `RequestStop`, `OnAcquisitionFrameRate`,
  `SetUpFrameRateProperty`) get a concise `/** ... */` banner above the
  definition, noting "Required by the MM::Camera API" where applicable — the
  style used throughout `DemoCamera.cpp` and the existing `SaperaGigE` function
  docs (e.g. `SnapImage`, `SaperaGigE.cpp:461`). Keep added inline comments brief
  and consistent with neighbors rather than the long prose blocks; trim where the
  rewrite touches existing verbose comments.
- Update `HOT_CAMERA.md`: record that `interval_ms` pacing, finite-length stop,
  overflow stop, and color metadata are fixed, and that `AcquisitionFrameRate`
  is now a real property.

### Naming (recommendation, not a code change)

Keep the SDK-based name `SaperaGigE` rather than renaming to "Teledyne". SDK-
based adapter names are the MM convention (`PVCAM`, `Spinnaker`/`SpinnakerC`,
`AndorSDK3`, `Aravis`). "Teledyne" is ambiguous: Teledyne also owns FLIR
(`Spinnaker`), Photometrics/Princeton (`PVCAM`), and Lumenera (`Lumenera`),
which already exist as separate adapters — so a "Teledyne" adapter would not be
uniquely identifiable, whereas "Sapera" precisely names the SDK driving Teledyne
DALSA Genie/Linea cameras. A rename would also break existing user device
configs, so it is out of scope unless explicitly requested. (If broader Sapera
transports beyond GigE Vision are ever targeted, dropping the `GigE` suffix to
`Sapera` could be reconsidered then.)

## Files modified

- `DeviceAdapters/SaperaGigE/SaperaGigE.h` — new members, worker/stop helpers,
  `OnAcquisitionFrameRate`/`SetUpFrameRateProperty` decls, comments.
- `DeviceAdapters/SaperaGigE/SaperaGigE.cpp` — constructor init,
  `StartSequenceAcquisition`, `XferCallback`, `StopSequenceAcquisition` +
  `performTeardown_`/`RequestStop`, `Shutdown`/destructor,
  `SetUpFrameRateProperty` + `OnAcquisitionFrameRate`, default-camera
  assignment + `loc_` member fix in `Initialize`, comment rewrites.
- `DeviceAdapters/SaperaGigE/SaperaGigE.vcxproj` and
  `DeviceAdapters/SaperaGigE/SaperaGigE.sln` — remove the broken `Win32` / `x86`
  configurations from both.
- `HOT_CAMERA.md` — status update.

## Verification

The Sapera SDK is **not** on this machine, so this cannot be compiled/run here.
Review + a Windows build with the SDK and a connected Genie GigE camera:

1. **Build** `SaperaGigE` in the full Micro-Manager Windows build (the Meson
   `justfile` covers only MMDevice/MMCore, not device adapters). Confirm the
   `x64` Debug/Release configs build and link; confirm the solution no longer
   advertises broken `Win32` configs.
2. **Default camera**: with one or more cameras detected and the
   `AcquisitionDevice` pre-init property left at its default, `Initialize()`
   succeeds (non-empty `activeDevice_`); the `Shutdown` log shows the real
   server name.
3. **Finite length**: `startSequenceAcquisition(N, interval, false)` returns
   exactly `N` frames, the camera stops itself, `isSequenceRunning()` goes false
   without a manual stop.
4. **Timing**: with a long interval (e.g. 2000 ms) delivered frames are ~2 s
   apart; since the first frame is due immediately, an N-frame run takes about
   `(N - 1) × interval` after the first delivered frame. With `interval = 0`
   (live) every frame is delivered at full rate.
5. **Color**: on a color sensor, sequence frames are RGB32 (not GRAY32).
6. **Overflow / errors**: force overflow (slow consumer, `stopOnOverflow = true`)
   and confirm acquisition stops and reports completion; confirm the failed
   frame is not counted toward a finite total.
7. **Stop lifecycle**: user `stopSequenceAcquisition()` mid-run stops cleanly
   (no deadlock, worker joined); back-to-back sequences work (prior worker
   joined on next start); `Shutdown` during a run is clean.
8. **Early-stop race**: a very short / immediately-stopped sequence (e.g.
   `numImages = 1`, or a stop issued right after start) where the stop is
   requested before the fresh worker reaches its CV wait — confirm it still tears
   down exactly once with no hang and no lost wakeup (the predicate is observed
   under `stopMutex_`).
9. **Property**: `AcquisitionFrameRate` appears and actually changes the rate on
   a supporting camera; on a camera without it, it is silently absent (logged
   "not supported") and `Initialize()` still succeeds.

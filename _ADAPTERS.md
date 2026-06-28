# Sapera adapter comparison — deliberate memory leaks in `SaperaGigE`

This note compares the `SaperaGigE` Micro-Manager device adapter against three
other, unrelated applications that wrap the same Teledyne DALSA **Sapera LT /
Sapera++** SDK. The goal is to understand the **deliberate memory leaks** in
`SaperaGigE` and whether they are fundamentally required.

## TL;DR

- `SaperaGigE` deliberately leaks the Sapera++ C++ wrapper objects (it calls
  `Destroy()` on the SDK handles but never `delete`s the wrappers) to avoid a
  Windows kernel BSOD in `cormem.sys` → `MmUnmapLockedPages`
  (bugcheck `0x1a` / `0x1230`) that was hit when the wrapper destructors ran.
- **Three** independent reference wrappers (Bonsai, go-sapera,
  DalsaPythonConnector) all run the full destructor / dispose path and **none
  of them leak.**
- All three references also use a **minimal pipeline**: none of them uses
  `SapBufferRoi` or `SapColorConversion`, which are exactly the two objects
  `SaperaGigE` blames for the crash.
- However, **none of the references reproduce `SaperaGigE`'s trigger
  conditions** (continuous streaming-stop + repeated mid-session reconfigure +
  ROI churn). They prove the *simple/steady-state* delete path is safe on this
  stack; they do **not** prove the *streaming-stop / reconfigure* delete path is
  safe.

## The deliberate leaks in `SaperaGigE`

Both sites are in `DeviceAdapters/SaperaGigE/SaperaGigE.cpp`. In both, the kernel
/ driver handles are correctly released via `Destroy()`; only the user-space
Sapera++ wrapper objects are abandoned (pointer set to `NULL`, never `delete`d).

| Site | Function | What leaks | Frequency |
|------|----------|-----------|-----------|
| A | `DestroySaperaPipeline_()` (lines ~504–530) | `AcqDeviceToBuf_`/`Xfer_`, `Conv_`, `Roi_`, `Buffers_` wrappers | once per device lifetime (final shutdown) |
| B | `DestroySaperaPipelineForReconfigure_()` (lines ~532–548) | `Roi_` (`SapBufferRoi`) only — others are kept and re-`Create()`d | **once per reconfigure** (ROI / pixel-format / geometry change) |

Site A is bounded (a few small objects, reclaimed by the OS at process exit).
**Site B is unbounded over a session**: every ROI, binning, or pixel-format
change adds another leaked `SapBufferRoi`. This is the leak of real concern.

### Why (per the in-code rationale and commit history)

Commits such as *"Troubleshoot more BSOD crashes"*, *"Further BSOD avoidance"*,
and *"Address another BSOD issue"* document the cause: running the wrapper
destructors after `Destroy()` repeatedly reached `cormem.sys`'s
`MmUnmapLockedPages` path and bugchecked the machine (`0x1a` / `0x1230`).
`MmUnmapLockedPages` operates on **locked physical DMA pages**. The two objects
specifically flagged as crash-prone are `SapBufferRoi` and `SapColorConversion`.

## The three reference applications

### 1. `../Bonsai.TeledyneDALSA` — C# / Windows / GigE

`Bonsai.TeledyneDALSA/SaperaCapture.cs`.

- **Closest environment match besides DalsaPythonConnector**: Windows + GigE
  (`SapAcqDevice`).
- `DestroyObjects()` (lines 166–191) calls **both `Destroy()` and `Dispose()`**
  on every object — the full finalization `SaperaGigE` deliberately skips — with
  no reported crash.
- Buffers are `new SapBufferWithTrash(3, device, SapBuffer.MemoryType.ScatterGather)`
  — **explicit ScatterGather** (host pageable), vs `SaperaGigE`'s SDK-default
  (contiguous / OS-locked) buffer. `MmUnmapLockedPages` is, by name, about
  *locked* pages — this is the single most interesting difference.
- **No `SapBufferRoi`, no `SapColorConversion`.**
- Lifecycle: the entire pipeline is created and destroyed **per acquisition** as
  locals (clean slate each subscription); objects are never reused across a
  reconfigure.

### 2. `../go-sapera` — Go + cgo (C++) / **Linux** / frame-grabber

`go-sapera/buffer.cpp`, `transfer.cpp`, `acquisition.cpp`.

- **Linux only** (README: "currently only works on Linux platforms"). There is
  **no `cormem.sys` on Linux**, and the bugcheck codes are Windows-only, so this
  wrapper operates **entirely outside the failure domain**. It neither confirms
  nor refutes the Windows BSOD.
- Drives a **frame-grabber board** (`SapAcquisition` + a `.ccf` camera file +
  `mx4` / Xtium MX4 line-scan metadata), **not** a GigE camera.
- `SapBuffer_Delete` / `SapAcqToBuf_Delete` / `SapAcquisition_Delete` are plain
  `delete` (`buffer.cpp:17`, `transfer.cpp:8`, `acquisition.cpp:12`) — **no
  leak**; lifecycle is driven explicitly from Go.
- Uses plain `SapBuffer` (`SapBuffer::TypeContiguous` for sized buffers,
  `buffer.cpp:14`); **no `SapBufferRoi`, no `SapColorConversion`**.

### 3. `../DalsaPythonConnector` — C++ / Windows / GigE / Python-driven

`DalsaPythonConnector/CPP/DalsaCamera.cpp`.

- **Exact environment match**: C++ + Windows + GigE (`SapAcqDevice`), driven
  from Python — the same stack as `SaperaGigE`.
- `CleanupCamera()` (lines 182–211) runs the **full path**: `Destroy()` **and**
  `delete` on `g_pXfer`, `g_pBuffer`, `g_pAcqDevice` — **no leak**.
- But it is **minimal**: buffer is `new SapBuffer(1, g_pAcqDevice)` (plain
  `SapBuffer`, count 1); **no `SapBufferRoi`, no `SapColorConversion`**; no
  callback. `CaptureImage()` just `Grab()`s one frame and `Save()`s a BMP.
- Per its own docs, **Live Streaming** and **ROI selection** are listed as
  *future enhancements* — not implemented. It therefore **never reaches** the
  streaming-stop / repeated-reconfigure / ROI-churn states that triggered
  `SaperaGigE`'s BSODs.

## Four-way comparison

| | Bonsai | go-sapera | DalsaPythonConnector | **SaperaGigE (MM)** |
|---|---|---|---|---|
| Lang / OS | C# / Windows | Go+C++ / **Linux** | C++ / Windows | C++ / Windows |
| Acquisition class | `SapAcqDevice` (GigE) | `SapAcquisition` (grabber) | `SapAcqDevice` (GigE) | `SapAcqDevice` (GigE) |
| Buffer | `SapBufferWithTrash`, **ScatterGather** | `SapBuffer`, Contiguous | `SapBuffer(1,…)`, default | `SapBufferWithTrash(3,…)`, default |
| `SapBufferRoi` | none | none | none | **yes** |
| `SapColorConversion` | none | none | none | **yes** |
| Streaming / callback | yes (XferNotify) | yes | **no — single grab** | yes (XferCallback) |
| Reconfigure mid-session | no (per-acq rebuild) | n/a | **no** | **yes** |
| Teardown | `Destroy()` + `Dispose()` | `Destroy()` + `delete` | `Destroy()` + `delete` | **`Destroy()` only → leak** |
| `cormem.sys` exposure | yes | **no (Linux)** | yes | yes |
| Crashes? | none reported | n/a (Linux) | none — but never stresses it | BSOD → leak workaround |

## Conclusions

1. **`SapBufferRoi` and `SapColorConversion` are unique to `SaperaGigE`.** All
   three references run a clean delete/dispose and all three omit those two
   classes — the very objects `SaperaGigE` blames. They are the strongest
   suspects, and the per-reconfigure `SapBufferRoi` (Site B) is the only
   *unbounded* leak.

2. **Plain delete is demonstrably safe on Windows GigE — in steady state.**
   DalsaPythonConnector (C++/Windows/GigE) `delete`s the same `SapAcqDevice` /
   `SapAcqDeviceToBuf` objects with no crash, so the destructor path is not
   *inherently* fatal on this stack. "The leak is fundamentally required" looks
   unlikely.

3. **But no reference reproduces `SaperaGigE`'s trigger conditions.** None drives
   streaming-stop + repeated reconfigure + ROI churn the way MMCore does. They
   show the simple path is fine; they do not disprove the BSOD. The crash most
   plausibly lives at the intersection of
   {`SapBufferWithTrash` / `SapBufferRoi` / `SapColorConversion`} **and**
   {repeated streaming-stop / reconfigure} — a combination no other wrapper
   exercises.

## Recommended next steps

In rough priority order:

1. **Drop `SapBufferRoi`.** Use camera-side `OffsetX` / `OffsetY` / `Width` /
   `Height` features for ROI (as all three references implicitly do) instead of
   a buffer-child ROI object. This removes both the crashiest object and the
   only unbounded leak in one change.
2. **Try `ScatterGather` buffers** (match Bonsai) instead of the SDK-default
   contiguous/locked buffer, since the BSOD is specifically about unmapping
   *locked* pages.
3. With those in place, **re-enable normal `delete`** and stress-test:
   repeated sequence start/stop, ROI changes, and pixel-format changes. If it
   survives, retire the leaks (Site B first). If only the final-shutdown leak
   (Site A) is still needed, that one is bounded and defensible.

## Source references

- `DeviceAdapters/SaperaGigE/SaperaGigE.cpp` — `DestroySaperaPipeline_()`,
  `DestroySaperaPipelineForReconfigure_()`, `FreeHandles()`, `SynchronizeBuffers()`.
- `../Bonsai.TeledyneDALSA/Bonsai.TeledyneDALSA/SaperaCapture.cs`
- `../go-sapera/{buffer,transfer,acquisition}.cpp`
- `../DalsaPythonConnector/CPP/DalsaCamera.cpp`

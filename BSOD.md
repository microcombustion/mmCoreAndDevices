# SaperaGigE Memory Leak Fixes

## Context

`SaperaGigE` has two deliberate leak sites introduced to avoid a Windows kernel BSOD
(`cormem.sys` / `MmUnmapLockedPages`, bugcheck `0x1a` / `0x1230`) triggered by C++ wrapper
destructors running after `Destroy()`. The concern is specifically about *locked* (physically
contiguous) DMA pages being unmapped unsafely by the driver.

- **Site A** (`DestroySaperaPipeline_()`, lines 504–530): All four wrappers leaked at final
  shutdown — bounded, OS reclaims at exit.
- **Site B** (`DestroySaperaPipelineForReconfigure_()`, lines 532–549): `SapBufferRoi` leaked
  on every property change that affects buffer layout (ROI, pixel format, width, height).
  **Unbounded** — accumulates throughout the session.

The comment at line 537 ("its geometry is constructor-only") is incorrect: `SapBufferRoi`
exposes `SetRoi(int xmin, int ymin, int width, int height)` (SDK header line 37), which
allows updating geometry without delete/new. This enables fixing Site B with zero destructor
risk.

The plan proceeds from zero-BSOD-risk (Site B) through a prerequisite buffer-type change and
then re-enables `delete` calls one wrapper at a time for Site A. Each step is an independent
commit that can be individually reverted if a BSOD is observed on hardware.

---

## Step 0 — Remove stale planning docs ✓

Delete from repo root: `PLAN.md`, `PLAN2.md`, `REVISIONS.md`, `HOT_CAMERA.md`.
Keep: `_ADAPTERS.md`, `AGENTS.md`.

---

## Step 1 — Reuse `Roi_` via `SetRoi()` (zero BSOD risk) ✓ hardware-verified

**Fixes:** Site B (the unbounded leak) entirely.  
**Files:** `DeviceAdapters/SaperaGigE/SaperaGigE.cpp`

### Change A — `DestroySaperaPipelineForReconfigure_()` (lines 541–546)

Remove the comment block (lines 542–545) and the `Roi_ = NULL;` assignment (line 546).
Keep `Roi_->Destroy()` (line 541) — the kernel handle must still be released.

The wrapper object stays alive; its kernel handle is gone; `Create()` will re-establish it.

### Change B — `SynchronizeBuffers_()` (line 1603)

Replace the unconditional allocation:
```cpp
// Before (line 1603):
Roi_ = new SapBufferRoi(Buffers_, roiX_, roiY_, roiW_, roiH_);

// After:
if (Roi_ == nullptr)
    Roi_ = new SapBufferRoi(Buffers_, roiX_, roiY_, roiW_, roiH_);
else
    Roi_->SetRoi(roiX_, roiY_, roiW_, roiH_);
```

Also update the comment at lines 534–537 to reflect that `SetRoi()` is used and the
"constructor-only" claim is no longer accurate.

**No destructors are ever called.** `Roi_` is allocated once on first pipeline creation
and reused via `SetRoi()` on every subsequent reconfigure.

**Verification:** Build (standalone `msbuild SaperaGigE.sln /t:Rebuild`). With hardware:
perform 10+ ROI/format/resolution changes in a live session; confirm process private bytes
do not grow monotonically.

---

## Step 2 — Switch `SapBufferWithTrash` to `TypeScatterGather` (low risk) ✓ hardware-verified

**Prerequisite for safe `delete` in Steps 3–5.**  
**Files:** `DeviceAdapters/SaperaGigE/SaperaGigE.cpp` (line 1594)

```cpp
// Before:
Buffers_ = new SapBufferWithTrash(3, &AcqDevice_);

// After:
Buffers_ = new SapBufferWithTrash(3, &AcqDevice_, SapBuffer::TypeScatterGather);
```

Matches constructor overload `(int count, SapXferNode*, Type, SapLocation)` in
`SapBufferWithTrash.h` line 19. `SapBuffer::TypeScatterGather` is defined at
`SapBuffer.h` line 26.

The BSOD (`MmUnmapLockedPages`) is specifically about *locked* physically contiguous DMA
pages. `TypeScatterGather` allocates from pageable virtual memory; the destructor path
does not go through `MmUnmapLockedPages`. For GigE Vision cameras (data arrives via the
network stack, not direct board DMA), scatter-gather is correct and standard — Bonsai's
`SaperaCapture.cs` uses it exclusively.

**Verification:** Build. Run `SnapImage` and a multi-frame `StartSequenceAcquisition`;
verify frames arrive with correct pixel data and no acquisition errors.

---

## Step 3 — Delete `Roi_` at final shutdown ✗ CONFIRMED BSOD — rolled back

### What was tried

```cpp
if (Roi_ && *Roi_ && !Roi_->Destroy()) ret = DEVICE_ERR;
delete Roi_;
Roi_ = nullptr;
```

### Crash history

**Run 1** (dump `063026-8031-01.dmp`): Played around (snaps, ROI changes) — no crash
during use. BSOD timing ambiguous: could have been shutdown or subsequent restart.

**Run 2** (dump `063026-7171-01.dmp`): BSOD confirmed **immediately on closing MM**.
Stack identical: `cormem+0x6cbb → MmUnmapLockedPages → MiUnmapLockedPagesInUserSpace+0x138
→ KeBugCheckEx (0x1a/0x1230)`. Process: `javaw.exe`.

### Conclusion

**Hypothesis A confirmed.** `delete Roi_` runs the `SapBufferRoi` destructor even
after `Roi_->Destroy()`, and that destructor calls `MmUnmapLockedPages` via
`cormem+0x6cbb` on a VA that is no longer valid. The `TypeScatterGather` change on the
parent `Buffers_` object does not affect `SapBufferRoi`'s internal allocation/cleanup
path. `SapBufferRoi` cannot be safely deleted in any form; it must be leaked like the
other wrappers.

**Rolled back**: `delete Roi_; Roi_ = nullptr;` removed; `Roi_ = NULL;` restored to the
null-assignment block at the end of `DestroySaperaPipeline_()`.

`Roi_` is already effectively a bounded leak since Step 1: one allocation on first
pipeline creation, reused forever via `SetRoi()`. No unbounded growth.

---

## Step 4 — Delete `Conv_` at final shutdown ✗ CONFIRMED BSOD — rolled back

### What was tried

`Conv_` is only allocated for color cameras; the null-check in `*Conv_` guarded against
mono cameras.

```cpp
if (Conv_ && *Conv_ && !Conv_->Destroy()) ret = DEVICE_ERR;
delete Conv_;
Conv_ = nullptr;
```

### Crash history

**Run 1**: First load/use/shutdown passed. Second load/use also passed, but BSOD occurred
while shutting down the second Micro-Manager process.

Dump `070126-7296-01.dmp`: `cormem+0x6cbb -> MmUnmapLockedPages ->
MiUnmapLockedPagesInUserSpace+0x138 -> KeBugCheckEx (0x1a/0x1230)`. Process:
`javaw.exe`.

### Conclusion

`SapColorConversion` cannot be safely deleted either. Its destructor reaches the same
`cormem.sys` locked-page unmap path as `SapBufferRoi`, possibly through the internal
`SapBufferRoi` / `SapLut` objects owned by `SapColorConversion`. The failure appearing on
the second shutdown still counts as a failed final-shutdown destructor test: the first
shutdown left the driver/runtime in a state where the same destructor path later bugchecked.

**Rolled back**: `delete Conv_; Conv_ = nullptr;` removed; `Conv_ = NULL;` restored to
the null-assignment block at the end of `DestroySaperaPipeline_()`.

`Conv_` remains a bounded one-time process leak, like `Roi_`.

---

## Step 5 — Delete `Buffers_` and `AcqDeviceToBuf_` at final shutdown (do not test yet)

**Files:** `SaperaGigE.cpp` — `DestroySaperaPipeline_()` (~lines 510, 516)

`Xfer_` aliases `AcqDeviceToBuf_`; delete only through `AcqDeviceToBuf_`.

```cpp
// Existing:
if (Xfer_ && *Xfer_ && !Xfer_->Destroy()) ret = DEVICE_ERR;
// Add immediately after:
delete AcqDeviceToBuf_;
AcqDeviceToBuf_ = nullptr;
Xfer_ = nullptr;
// Remove the later: AcqDeviceToBuf_ = NULL; Xfer_ = NULL;  (lines 524–525)

// Existing:
if (Buffers_ && *Buffers_ && !Buffers_->Destroy()) ret = DEVICE_ERR;
// Add immediately after:
delete Buffers_;
Buffers_ = nullptr;
// Remove the later: Buffers_ = NULL;  (line 528)
```

**Status:** Deferred. Since `delete Roi_` and `delete Conv_` both hit the same
`MmUnmapLockedPages` bugcheck path, further final-shutdown destructor experiments should
not continue until there is a stronger reason to believe these two remaining wrappers avoid
that path.

---

## What is NOT changed

- Destroy ORDER in `DestroySaperaPipeline_()`: `Xfer_ → Conv_ → Roi_ → Buffers_` is
  documented as critical (Roi_ before Buffers_, after Xfer_). Order is preserved.
- `AcqDevice_` teardown — separate function, already clean.
- No `MMDevice` / `MMCore` changes; no DIV bump.

## Rollback strategy

Steps 3–5 are individually revertible. If a BSOD appears at Step N, revert that commit;
Steps 1–2 (the zero/low risk changes) remain. Site A at that point is a bounded one-time
leak; Site B is fully eliminated by Step 1.

---

## Final assessment after Step 4 rollback

The rolled-back baseline appears stable again in hardware testing:

- `Roi_` and `Conv_` are both unsafe to delete at final shutdown. Each destructor test
  reached the same `cormem.sys -> MmUnmapLockedPages -> 0x1a/0x1230` kernel crash path.
- `TypeScatterGather` on the parent `SapBufferWithTrash` does not make child objects such
  as `SapBufferRoi` or `SapColorConversion` safe to destroy.
- The remaining final-shutdown leaks are bounded process-lifetime leaks and are preferable
  to a system crash.
- Step 5 should not be tested next. After two independent wrapper destructors reached the
  same bugcheck path, deleting `Buffers_` or `AcqDeviceToBuf_` has poor upside and real
  BSOD risk.

Recommended next testing is to stress the current baseline rather than continue destructor
experiments: repeated snap, live start/stop, ROI changes, pixel-format changes,
width/height/binning changes, and several full Micro-Manager restart/shutdown cycles while
watching process private bytes for monotonic growth.

The next code direction, if more cleanup is needed, should be structural: remove
`SapBufferRoi` from the Sapera pipeline and rely on camera-side `OffsetX`, `OffsetY`,
`Width`, and `Height` features for ROI. That targets the crash-prone object directly instead
of trying to make its destructor safe.

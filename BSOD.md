# SaperaGigE `cormem.sys` BSOD — diagnosis and mitigation

**Status (2026-07-06, 13:41 — crash #7):** The plain-`SapBuffer` (no trash) build
crashed 15 minutes after install with the identical signature
(`070626-7312-01.dmp`; minidump verified: same `cormem+0x6cbb → MmUnmapLockedPages`
kernel stack, `javaw.exe`). **The trash-buffer hypothesis is falsified** — the
double-unmapped mapping record is not the trash resource. The crash #7 Active dump
(copied to `Documents\Minidumps\MEMORY-crash7.dmp`; crash #6's Active dump remains as
`MEMORY.dmp`) was analyzed the same day: the user-mode chain is **frame-for-frame
identical to crash #6** — Java `setProperty(PixelType)` → `OnPixelType` →
`SynchronizeBuffers` → `DestroySaperaPipelineForReconfigure_` →
`SapTransfer::Destroy+0x6a` → `SapTransfer::Disconnect+0x6c` →
`CorApi!CorXferDisconnect+0x7e` → `cor_cam_SapGige_s` →
`CorMem!CorMemUnmapPhysMemory+0x83` → `DeviceIoControl` → cormem.sys. PTE evidence is
even cleaner than crash #6: **every page of the target VA range (P3
`0x1d41c5a0000`) is `not valid` (PTE = 0)** — the range was fully unmapped before this
second unmap request (crash #6 had page 0 already recycled by the heap; here not even
that). Root-cause model unchanged and now confirmed independent of buffer type.
CoreLog for the crashed process (pid 988, `CoreLog20260706T134018_pid988.txt`) shows
one camera init at 13:40:21 and a live transfer running by 13:40:23, then ~78 s of
log lost unflushed; the Active dump supplies the missing trigger (a PixelType change).
Operator context (reported after the fact): the *previous* MM session was closed with
live view still open/running; MM was then restarted, and the crash hit when changing
a setting in the fresh session. The previous session's log (pid 18328) shows that
exit-with-live was handled cleanly: frames flowed until 13:40:15.56, then Shutdown ran
Freeze+Wait and all Destroy() calls without a single failure or timeout log, "Core
session ended" 13:40:15.67. Nothing carried over: the deliberate wrapper leaks skip
only `delete`, never `Freeze()`/`Destroy()`/`AcqDevice_.Destroy()` (which closes the
GigE control channel), so exit-with-live cannot leave the camera streaming. The
new session's 13:40:23 dropped-frame line is immediately followed by a display-window
creation — the signature of a manual Live/Snap start, not inherited streaming.
Acquisition history remains a non-ingredient — crash #6 had zero frames ever on its
connection — the only invariant is the Disconnect.

**Prior status (2026-07-06, midday):** Crash #6 occurred on the `SapBufferRoi`-free build and
was captured by the newly configured **Active memory dump** — the user-mode stack is now
known exactly (section 5). The fatal call is **`SapTransfer::Destroy()` →
`SapTransfer::Disconnect` → `cor_cam_SapGige_s` → `CorMem!CorMemUnmapPhysMemory`**,
double-unmapping a frame-buffer-sized mapping whose VA range had already been freed and
partially recycled. This is a defect inside the Sapera GigE transfer module's
connect/disconnect bookkeeping, not in the adapter's object lifecycle: the adapter
cannot avoid disconnecting the transfer (reconfigure and shutdown both require it).
Earlier theories in date order: "wrapper destructors are the trigger" — invalidated by
crash #5 (section 2); "SapBufferRoi child views seed the stale record" — weakened to
exonerated by crash #6 (Roi_-free build, fresh 12-second-old process). See
`BUGREPORT.md` for the vendor-facing report with the full evidence chain.

**Claude Fable 5.0 session pickup**

claude --resume 6c06c798-7779-4b6d-8ee1-24f55542f37d

---

## 1. The crash, precisely

Every BSOD in this investigation is the same crash. From the Windows System event log
(WER event 1001), **19 bugchecks between 2026-06-24 and 2026-07-06**, all with the
identical signature:

```
0x0000001a (0x0000000000001230, <kernel address>, <user VA, 64KB-aligned>, 0x2)
```

All five retained minidumps (`C:\Windows\Minidump`, copies in
`C:\Users\ischoegl\Documents\Minidumps`) were analyzed with kd/WinDbg. Every one
contains the **byte-identical kernel stack**, down to the same two return addresses
inside the driver:

```
javaw.exe thread
  nt!KiSystemServiceUser
  nt!NtDeviceIoControlFile            <- ordinary DeviceIoControl syscall from user mode
  nt!IopXxxControlFile
  nt!IopSynchronousServiceTail
  nt!IofCallDriver
  cormem+0x32df                       <- same cormem call site in all five dumps
  cormem+0x6cbb
  nt!MmUnmapLockedPages+0x34a
  nt!MiUnmapLockedPagesInUserSpace+0x138
  nt!KeBugCheckEx (0x1a / 0x1230)
```

| Dump | Date/time | Code state under test |
|------|-----------|----------------------|
| `062726-7000-01.dmp` | 6/27 18:21 | pre-Step-1 baseline (leak workarounds) |
| `063026-8031-01.dmp` | 6/30 12:36 | Step 3 (`delete Roi_`) run 1 |
| `063026-7171-01.dmp` | 6/30 12:55 | Step 3 run 2 |
| `070126-7296-01.dmp` | 7/1 11:18 | Step 4 (`delete Conv_`) |
| `070126-7093-01.dmp` | 7/1 11:53 | **Steps 1+2 only — no destructor ever runs** |
| `070626-7250-01.dmp` + `MEMORY.dmp` | 7/6 12:55 | **crash #6: `Roi_`-free build; Active dump gave full user chain (section 5)** |
| `070626-7312-01.dmp` + `MEMORY-crash7.dmp` | 7/6 13:41 | **crash #7: plain `SapBuffer` (no trash) build — trash hypothesis falsified; Active dump shows chain identical to crash #6, triggered by PixelType change** |

Environment: Windows 11 Enterprise 26100, Sapera LT 9.12 (`cormem.sys` 9.12.00.2431,
2026-03-31 — current at the time of analysis), `CorGigeFilter.sys` 7.00.01.1700.

### What the signature means

`MiUnmapLockedPagesInUserSpace` bugchecks with subcode `0x1230` when the user-space
PTEs it is asked to unmap do not match the MDL — i.e. the mapping is **already gone or
was never valid**. The stack shows the request arrived as a *synchronous, user-mode
initiated DeviceIoControl* from `javaw.exe` (the Micro-Manager process). Therefore:

1. **It is not a destructor-specific code path.** `Destroy()`, the C++ destructors, and
   any Sapera-runtime cleanup all issue the same unmap IOCTL. Deleting wrappers did not
   add a new failure mode; it rolled the same dice.
2. **It is not kernel process-rundown cleanup.** User-mode Sapera code explicitly asked
   cormem.sys to unmap a mapping that no longer existed — a **stale mapping record /
   double-unmap** in the Sapera user-mode runtime's bookkeeping.
3. **cormem.sys 9.12 forwards the unmap to `MmUnmapLockedPages` without validating it.**
   A stale record in user-mode bookkeeping should fail the IOCTL, not bugcheck the
   machine. This is a vendor driver robustness bug (see `BUGREPORT.md`).

### Known limitation of the minidumps

A kernel minidump carries no user-mode stack, so the dumps cannot show *which* user-mode
call issued the fatal IOCTL — the adapter's `Shutdown()` teardown and the Sapera
runtime's own DLL-detach/atexit cleanup at process exit look identical from the kernel
side. The machine is now configured for **Active memory dumps**
(`HKLM\SYSTEM\CurrentControlSet\Control\CrashControl`: `CrashDumpEnabled=1`,
`FilterPages=1`), so the *next* crash, if any, will name the exact user-mode call chain
in `C:\Windows\MEMORY.DMP`.

---

## 2. Why the earlier step-wise conclusions are withdrawn

The June/July experiment series (documented in earlier revisions of this file) tested
"remove one leak at a time, watch for BSOD":

- **Step 1** — reuse `Roi_` via `SetRoi()` instead of leaking one `SapBufferRoi` per
  reconfigure. *Initially marked hardware-verified.*
- **Step 2** — `SapBufferWithTrash` with explicit `SapBuffer::TypeScatterGather`.
  *Initially marked hardware-verified.*
- **Step 3** — `delete Roi_` at final shutdown → BSOD observed twice → "CONFIRMED".
- **Step 4** — `delete Conv_` at final shutdown → BSOD observed once → "CONFIRMED".
- **Step 5** — `delete Buffers_`/`AcqDeviceToBuf_` — never tested.

Crash #5 (7/1 11:53) broke this methodology: it occurred **after** the Step 4 rollback
was committed (11:27–11:44), on the Steps 1+2 build in which no wrapper destructor ever
runs — and it is byte-identical to the Step 3/4 crashes. (Caveat: git timestamps show
the rollback preceded the crash, but it was not separately documented that the DLL was
rebuilt and reinstalled in that 9-minute window. Even under the worst-case assumption
that the Step 4 binary was still loaded, the argument below stands on the 6/27 crash —
identical signature on the pre-Step-1 baseline — and on the dump evidence that
destructors and `Destroy()` are indistinguishable at the driver interface.)

Conclusion: the baseline itself intermittently produces this crash at MM shutdown, so
single-crash observations on top of that baseline cannot attribute causality to the
change under test. The Step 3/4 "CONFIRMED BSOD" verdicts are **withdrawn as
unproven** — the experiments could not establish causality either way.
(Mechanistically, the deletes are now *believed unsafe again* for a better-grounded
reason: the wrapper destructors re-enter the release path that the driver cannot
tolerate twice — see the correction at the end of section 4.) All of Steps 1–4 were
rolled back; Steps 1+2 survive in commit `d77ab6c40` (branch `backup`), the Step 4
experiment in `stash@{0}`.

Additional control data point: **CamExpert (Teledyne's own tool) has never produced
this BSOD on the same machine/cameras.** CamExpert uses plain `SapBuffer` pipelines with
full destructor teardown, but does not use `SapBufferRoi` and does not churn
Destroy()/Create() cycles mid-session. The stale record is therefore created by
something in *this adapter's* usage pattern (see `_ADAPTERS.md` — no reference
application uses `SapBufferRoi` or `SapColorConversion` either).

---

## 3. Root-cause model (best supported by all evidence)

Somewhere in the Sapera user-mode runtime's per-process buffer bookkeeping, a record of
a locked-page user mapping survives after the mapping itself is gone. At MM shutdown, a
cleanup pass (either the adapter's `Destroy()` cascade or the runtime's own exit
cleanup) walks that bookkeeping and issues an unmap IOCTL for the stale record;
cormem.sys passes it straight to `MmUnmapLockedPages`; the kernel bugchecks.

Prime suspects for what seeds the stale record, in order:

1. **`SapBufferRoi` child views.** A `SapBufferRoi` creates child `CORBUFFER` handles
   over its parent's pages *plus* a separate `m_hTrashChild` over the parent's trash
   buffer — several handles aliasing one resource, created and destroyed on **every**
   reconfigure (ROI, pixel format, width/height, binning). Aliased handles over shared
   pages are the classic double-free/double-unmap setup, and an in-code comment from
   earlier debugging already documented one ordering (`Roi_` destroyed while `Xfer_`
   live) that reliably bugchecked.
2. **Silently ignored `Destroy()` failures.** Both teardown functions previously folded
   all failures into one `DEVICE_ERR` with no log, so a failed Destroy (which leaves
   wrapper bookkeeping out of sync with kernel state) was invisible and the next
   teardown pass ran against inconsistent state.
3. **`SapColorConversion`** (color camera only) internally owns its own `SapBufferRoi`
   and LUT buffers — same aliasing pattern, outside the adapter's control.

---

## 4. Mitigation implemented (2026-07-06)

`SapBufferRoi` turned out to be **functionally inert** in this adapter: it was never
passed to the transfer (`SapAcqDeviceToBuf(&AcqDevice_, Buffers_, ...)` — the transfer
always delivers full frames into `Buffers_`) nor to the converter
(`SapColorConversion(&AcqDevice_, Buffers_)`). Every use of `Roi_` was reading back the
four coordinates already stored in `roiX_/roiY_/roiW_/roiH_`; the actual ROI has always
been a software crop via `ReadRect` with `img_` sized to the ROI.

Changes in `SaperaGigE.cpp` / `SaperaGigE.h`:

1. **`Roi_` (`SapBufferRoi`) removed entirely.** No child-buffer/trash-child kernel
   objects are created or destroyed anymore, on any path. This also permanently
   eliminates leak Site B (formerly one leaked wrapper per reconfigure) by construction.
2. **`SetROI()`/`ClearROI()` no longer tear down the Sapera pipeline.** A software-ROI
   change now only updates `roiX_/roiY_/roiW_/roiH_` and resizes `img_`
   (`ResizeImageBuffer()`). This removes the most frequent source of Destroy()/Create()
   churn entirely. Reconfigures that genuinely change buffer geometry (pixel format,
   width/height, binning, timeout) still rebuild via `SynchronizeBuffers()`.
3. **Per-object `Destroy()` failure logging** in `DestroySaperaPipeline_()` and
   `DestroySaperaPipelineForReconfigure_()`, so a failed Destroy — the suspected seed of
   the stale mapping — leaves a breadcrumb in the CoreLog that can be correlated with a
   later crash.

Unchanged: the wrappers (`Buffers_`, `AcqDeviceToBuf_`, `Conv_`) are still deliberately
leaked (Destroy() only, no delete) at final shutdown. That is now a small, bounded,
once-per-session leak — and it is a **legitimate mitigation, not superstition** (a
correction to an earlier draft of this analysis): the wrapper destructors re-enter the
same resource-release path that `Destroy()` already ran, and per section 5 the driver
stack bugchecks on *any* second release of a mapping record. Delete-after-Destroy is
therefore an extra roll of a die the machine cannot afford to lose. This also explains
the project history: before the leak/order/double-Freeze workarounds, crashes were
frequent and reproducible (the adapter was supplying deterministic second releases);
after them, crashes became rare and timing-dependent (only the vendor stack's own
internal double-release remained). Do not re-enable `delete` before Teledyne fixes the
underlying defect.

### Test plan for the new build

1. Mono camera: repeated snap, live start/stop, and **many ROI changes** (now
   teardown-free), several full MM restart/shutdown cycles.
2. Pixel-format and width/height/binning changes (these still rebuild the pipeline).
3. Color camera: same, plus verify demosaiced images still correct (Conv_ path
   unchanged).
4. Watch `javaw` private bytes across reconfigures — should be flat now that Site B is
   gone by construction.
5. If a BSOD occurs: `C:\Windows\MEMORY.DMP` (Active dump) now captures the user-mode
   stack — analyze before changing anything else.

---

## 5. Crash #6 (2026-07-06 12:55) — the Active dump names the culprit

**Context.** First test run of the section-4 build (`Roi_` removed; DLL timestamp
12:30:19, verified identical to the repo build output). Mono camera **Genie
Nano-M1930** (`Nano-M1930_1`). Several clean MM open/close cycles (six separate javaw
processes 12:47–12:55, all closed without incident), then: fresh MM start at 12:55:18,
device initialized 12:55:21 (pipeline created exactly once, in `Initialize()`), GUI up
12:55:23, user switched PixelType **mono8 → mono10** ≈12:55:30 → BSOD. Same bugcheck
signature. CoreLog (`CoreLog20260706T125518_pid10268.txt`) shows **no acquisition and
no Destroy-failure breadcrumbs** before the end of the log. Dumps: minidump
`070626-7250-01.dmp` + **Active dump** `MEMORY.DMP` (6.3 GB, copy in
`C:\Users\ischoegl\Documents\Minidumps`).

**The complete call chain** (kernel stack from `!analyze -v`; user stack recovered via
`.thread /r /p <thread>; .reload /user` plus a raw `dps` scan of the user stack — the
Sapera DLLs have no unwind info, adapter frames resolve fully via the Debug-build PDB):

```
Java: mmcorej CMMCore.setProperty                     (MMCoreJ_wrap)
→ CDeviceBase<MM::Camera,SaperaGigE>::SetProperty
→ MM::PropertyCollection::Set → MM::Property::Apply → MM::Action<SaperaGigE>::Execute
→ SaperaGigE::OnPixelType+0x1e7
→ SaperaGigE::SynchronizeBuffers+0x20d
→ SaperaGigE::DestroySaperaPipelineForReconfigure_+0x8d   <- the FIRST Destroy call
→ SapClassBasic91!SapTransfer::Destroy+0x6a               (SapClassBasic91.dll 9.12.00.2431)
→ SapClassBasic91!SapTransfer::Disconnect+0x6c
→ CorApi!CorXferDisconnect+0x7e                           (corapi.dll 9.12.00.2431)
→ cor_cam_SapGige_s (internal, no symbols)                (cor_cam_SapGige_s.dll 7.00.00.1703)
→ CorMem!CorMemUnmapPhysMemory+0x83                       (CorMem.dll 9.00)
→ KERNELBASE!DeviceIoControl
→ ... → cormem.sys (+0x32df/+0x6cbb) → nt!MmUnmapLockedPages
→ nt!MiUnmapLockedPagesInUserSpace+0x138 → KeBugCheckEx(0x1a, 0x1230, …)
```

**Memory-state evidence.** Bugcheck P3 (the user VA being unmapped, `0x1fb60b70000`):
`!pte` shows page 0 of the range **valid** (PFN 0x289217, plain user read/write page)
but pages at +0x1000, +0x2000, +0x10000, +0x90000 all **not valid (PTE = 0)**. A live
locked-pages user mapping would have every page valid. Interpretation: the
frame-buffer-sized mapping at that VA had **already been unmapped once**, the freed VA
range was partially recycled by an unrelated allocation (page 0), and the crashing call
is the **second** unmap of the same record. A thread sweep (`!process <proc> 17`)
shows every other Sapera thread parked in waits (`CorXferWait`, `CorGetControl`,
GigE server) — no concurrent unmap at crash time.

**What this rules out / establishes:**

1. **The adapter's object lifecycle is not the seed.** The process was 12 seconds old;
   the pipeline had been created exactly once; no wrapper had ever been destroyed,
   deleted, or reused; `SapBufferRoi` no longer exists in the build. The stale record
   was created and invalidated entirely *inside* the Sapera GigE stack between
   `Xfer_->Create()` (Connect) and the first `Xfer_->Destroy()` (Disconnect), or the
   Disconnect path unmaps one of its own records twice in a single call.
2. **Both historical crash sites are the same call.** `DestroySaperaPipeline_()`
   (shutdown) and `DestroySaperaPipelineForReconfigure_()` (reconfigure) both begin
   with `Xfer_->Destroy()` → the same `Disconnect` path. Every earlier "crash at MM
   close" is consistent with this one chain; there was never a second mechanism.
3. **The adapter cannot dodge this call.** A transfer, once connected, must be
   disconnected to change buffer geometry and at shutdown. The exposure can at most be
   *reduced* (fewer reconfigures — already done for ROI in section 4), never
   eliminated.
4. Sapera LT 9.1x is Teledyne's current release line; there is no newer version to
   upgrade into. The fix has to come from Teledyne (`cor_cam_SapGige_s.dll` /
   `CorMem.dll` bookkeeping, and/or cormem.sys validating unmap requests).

**Remaining adapter-side experiment — RESULT: FAILED (crash #7, 2026-07-06 13:41).**
The no-trash build (installed 13:26, byte-identical to the repo build output) crashed
at 13:41 with the same signature (`070626-7312-01.dmp`). The trash resource is
therefore NOT the aliased mapping record; the double-release lives in mapping records
the connection always creates (the frame buffers themselves or GigE-internal buffers,
e.g. TurboDrive). Keeping plain `SapBuffer` or reverting to `SapBufferWithTrash` is
now a functional choice, not a stability one. Original rationale kept below for the
record:
`SapBufferWithTrash(3, …)` replaced with plain `SapBuffer(3, …)`. The trash buffer is
an extra buffer resource registered with the transfer connection — if the GigE
module's double-unmap involves the trash resource's mapping record, removing it
changes the outcome; Bonsai uses trash, but DalsaPythonConnector (crash-free, same
stack) uses plain `SapBuffer`. `XferCallback`'s `IsTrash()` branch is now inert (kept
for easy revert); overflow manifests as dropped/overwritten frames in the 3-buffer
ring instead of trash events. Because the crash is intermittent (six clean cycles
preceded crash #6), only an extended crash-free streak under reconfigure-heavy use
counts as a positive signal.

---

## 6. Next steps

1. **Send `BUGREPORT.md` to Teledyne DALSA support** — it now contains the complete
   user-mode chain, module versions, the minimal 12-second fresh-process scenario, and
   the PTE evidence. This is a strong, actionable vendor report.
2. **Keep the Active-dump configuration.** Every future crash yields a full chain for
   the cost of one 6.3 GB file.
3. ~~Test the plain-`SapBuffer` (no trash) build~~ — **done, negative result**
   (crash #7, see section 5). Decide whether to keep plain `SapBuffer` (simpler; frame
   drops in the ring on overflow) or revert to `SapBufferWithTrash` (explicit trash
   events); stability is unaffected either way.
3a. ~~Analyze the crash #7 Active dump~~ — **done** (`MEMORY-crash7.dmp`): chain
   identical to crash #6 (PixelType change → `DestroySaperaPipelineForReconfigure_` →
   `SapTransfer::Destroy/Disconnect` → double-unmap); entire target VA range already
   PTE=0. Confirms the defect is independent of buffer type.
4. **If crashes persist on the color camera only:** replace `SapColorConversion` with
   in-adapter demosaicing (it owns an internal `SapBufferRoi`).
5. **Only after Teledyne fixes the underlying defect:** consider retiring the
   final-shutdown wrapper leaks (delete after Destroy). The destructors re-enter the
   same release path the driver cannot tolerate twice (see the correction at the end
   of section 4), so the leaks stay until the driver stack is fixed.

---

## 7. Toolbox — everything needed to pick this up cold

This section exists so a future maintainer (or analysis session) can continue without
rediscovering the tooling. All commands are PowerShell unless noted.

### 7.1 Check for new bugchecks (no admin required)

```powershell
Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001} -MaxEvents 20 |
  Where-Object { $_.Message -like '*bugcheck*' } |
  ForEach-Object { "{0} :: {1}" -f $_.TimeCreated, ($_.Message -replace "`r`n"," ") }
```

Each entry lists the bugcheck code, all four parameters, and the dump filename. Compare
against the signature in section 1: same `0x1a/0x1230/…/0x2` shape means the same bug.

### 7.2 Getting at the dumps (admin required for the originals)

`C:\Windows\Minidump\*.dmp` and `C:\Windows\MEMORY.DMP` are readable by Administrators
only, and `sudo` is disabled on this machine. Copy them out from an **elevated**
PowerShell (files inherit user-readable ACLs from the destination):

```powershell
Copy-Item C:\Windows\Minidump\*.dmp C:\Users\ischoegl\Documents\Minidumps\
Copy-Item C:\Windows\MEMORY.DMP    C:\Users\ischoegl\Documents\Minidumps\   # after a new crash
```

The five analyzed minidumps are already in `C:\Users\ischoegl\Documents\Minidumps`.

### 7.3 Dump analysis without classic Debugging Tools

The classic WDK debuggers are NOT installed; the Microsoft Store WinDbg is, and it
ships console debuggers inside its package:

```powershell
$kd = Join-Path (Get-AppxPackage Microsoft.WinDbg).InstallLocation "amd64\kd.exe"
# (was ...\Microsoft.WinDbg_1.2603.20001.0_x64__8wekyb3d8bbwe\amd64\kd.exe — the
#  version segment changes when the app updates, so resolve it via Get-AppxPackage)
$env:_NT_SYMBOL_PATH = "srv*C:\Users\ischoegl\Documents\Minidumps\symbols*https://msdl.microsoft.com/download/symbols"
& $kd -z <dump> -c "!analyze -v; q"
```

**Important technique — the stack unwind breaks at cormem.** There is no PDB (and in a
minidump no image) for `cormem.sys`, so `!analyze -v` / `k` stop after the
`cormem+0x6cbb` frame and the true caller chain is invisible. To recover it, take the
child-SP of the `cormem+0x6cbb` frame (the first column of that same line in
`STACK_TEXT`) and scan the raw stack upward for return addresses:

```
.thread <FAULTING_THREAD from !analyze>; dps <childSP> L140; q
```

Symbol-resolving quadwords give the real chain. That is how the
`NtDeviceIoControlFile` origin in section 1 was established. Caveat: `dps` also shows
*stale* addresses from earlier syscalls on the same stack — `Npfs!NpFsdClose`,
`FLTMGR!FltpPassThrough`, `nt!ObCloseHandleTableEntry` appear at identical offsets in
all five dumps and are residue, not live frames. The live chain is the monotonic
`IopfCallDriver → IofCallDriver → IopSynchronousServiceTail → IopXxxControlFile →
NtDeviceIoControlFile → KiSystemServiceCopyEnd/User` progression.

The `cormem+0x32df` / `cormem+0x6cbb` offsets are specific to cormem.sys **9.12.00.2431**;
re-derive them if the driver has been updated.

### 7.4 Analyzing the next crash (Active memory dump)

The machine is configured (`HKLM\SYSTEM\CurrentControlSet\Control\CrashControl`:
`CrashDumpEnabled=1`, `FilterPages=1`) to write `C:\Windows\MEMORY.DMP` with user-mode
memory included. On the next crash, the goal is the **user-mode stack of the faulting
thread**, which the minidumps could not provide:

```
!analyze -v                       ; note PROCESS_NAME / FAULTING_THREAD
.process /r /p <process>; .thread <thread>
.reload /user
kb                                ; user frames should now resolve through
                                  ; SapClassBasic64.dll / corapi / the adapter DLL
```

The single question to answer: does the fatal IOCTL originate from the adapter's
explicit teardown (`SaperaGigE::Shutdown → FreeHandles → …Destroy()`), from the device
destructor path (`DeleteDevice → ~SaperaGigE → ~SapAcqDevice`), or from Sapera runtime
DLL-detach/atexit cleanup during process exit? Each points to a different next fix.

**This procedure was executed for crash #6 (section 5) and worked.** Practical notes:
the standard `k` after the context switch resolved kernel frames plus the first user
frame (`CorMem!CorMemUnmapPhysMemory`); the Sapera DLLs lack unwind data, so the rest
of the user stack was recovered with a raw scan (`dps <userSP> L600`, keep only lines
with resolved symbols). Add the adapter's Debug build directory to
`_NT_SYMBOL_PATH` — `mmgr_dal_SaperaGigE` frames then resolve with full C++ names,
which anchors the whole chain. Cross-check any recovered chain against the CoreLog
(`C:\Program Files\Micro-Manager-2.0\CoreLogs\CoreLog<timestamp>_pid<pid>.txt`) —
note the log tail is buffered and typically lost at the bugcheck.

### 7.5 Building the adapter standalone (no full Micro-Manager build needed)

```sh
# Bash/Git Bash. VS 2026 lives under version dir "18", not a year.
"/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe" \
  "DeviceAdapters/SaperaGigE/SaperaGigE.vcxproj" \
  -p:Configuration=Debug -p:Platform=x64 \
  "-p:SAPERADIR=C:/Program Files/Teledyne DALSA/Sapera" \
  -p:WindowsTargetPlatformVersion=10.0 -v:minimal
```

Output: `DeviceAdapters/SaperaGigE/build/Debug/x64/mmgr_dal_SaperaGigE.dll`.
Known pre-existing issue: Release|x64 fails to link (v142/v143 toolset mismatch in the
vcxproj) — unrelated to adapter code.

### 7.6 Where everything lives

| Artifact | Location |
|----------|----------|
| Analyzed minidump copies | `C:\Users\ischoegl\Documents\Minidumps` |
| Rolled-back Steps 1+2 code (SetRoi reuse + ScatterGather) | commit `d77ab6c40`, branch `backup` |
| Step 4 experiment code (`delete Conv_`) | `git stash@{0}` |
| Sapera++ SDK headers (ground truth for wrapper behavior) | `C:\Program Files\Teledyne DALSA\Sapera\Classes\Basic\*.h` |
| SDK demo apps (correct lifecycle patterns) | `...\Sapera\Demos\Classes\Vc\`, `...\Examples\Classes\` |
| Reference-wrapper comparison (Bonsai / go-sapera / DalsaPythonConnector) | `_ADAPTERS.md` (repo root) |
| Vendor bug report draft | `BUGREPORT.md` (repo root) |
| Vendor GUI control (crash-free baseline) | `...\Sapera\CamExpert\` |

### 7.7 Re-verifying the load-bearing claims

Do not take this document's word for it; both key claims are mechanically checkable:

- **"`SapBufferRoi` was inert":** in the pre-removal source
  (`git show f8c1005d3:DeviceAdapters/SaperaGigE/SaperaGigE.cpp`), grep `Roi_`. The
  transfer is constructed as `SapAcqDeviceToBuf(&AcqDevice_, Buffers_, ...)` and the
  converter as `SapColorConversion(&AcqDevice_, Buffers_)` — neither receives `Roi_`.
  Every remaining `Roi_` use is a `GetXMin/GetYMin/GetWidth/GetHeight` read or
  create/destroy bookkeeping. `SapBufferRoi.h` (SDK header) shows the `m_hTrashChild`
  member that made each instance carry extra aliased kernel handles.
- **"Crash #5 ran a destructor-free build":** `git log --format="%h %ci %s"` around
  7/1 shows the Step 4 rollback committed 11:27–11:44 (`d515ffaa6`, `20945f82d` on
  `backup`); the WER event log (7.1) shows the crash at 11:53; the Steps 1+2 diff
  (`git show d77ab6c40`) contains no `delete` of any Sapera wrapper. See the caveat in
  section 2 about the rebuild/reinstall timing within that window.

### 7.8 History of withdrawn experiments (condensed)

Full details in this file's git history (`git log -p -- BSOD.md`, and `backup` branch).
Summary: Step 1 = reuse `Roi_` via `SetRoi()` between Destroy/Create (SetRoi is valid
only pre-Create; it was called in that window). Step 2 = explicit
`SapBuffer::TypeScatterGather` on `SapBufferWithTrash` (note: the previous default was
`SapDefBufferType` = `TypeDefault` = -1, resolved by the SDK at Create time, so whether
Step 2 changed actual allocation behavior was never established). Steps 3/4 = `delete`
of `Roi_`/`Conv_` after `Destroy()` at final shutdown; crashes followed but — per
section 2 — attribution was unsound. Step 5 (`delete Buffers_`/`AcqDeviceToBuf_`) was
never tested. All were rolled back; the shipped mitigation is section 4.

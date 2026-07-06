# Bug report — GigE transfer disconnect double-unmaps a buffer mapping, causing system bugcheck 0x1A/0x1230 in cormem.sys

*Draft for submission to Teledyne DALSA technical support. Fill in the fields marked
`[FILL IN]` before sending. Attach the dumps listed in section 5 (at minimum
`MEMORY.DMP` from 2026-07-06 and one or two minidumps).*

## 1. Summary

`SapTransfer::Destroy()` → `SapTransfer::Disconnect` on a GigE Vision acquisition
device (`cor_cam_SapGige_s.dll`) intermittently issues a `CorMemUnmapPhysMemory`
request for a buffer mapping that has **already been unmapped** (the freed VA range is
partially recycled by the process heap by the time of the second unmap). `cormem.sys`
forwards the request to `MmUnmapLockedPages` without validating it, and the machine
bugchecks: `0x1A MEMORY_MANAGEMENT`, subcode `0x1230`, in
`nt!MiUnmapLockedPagesInUserSpace`.

We have hit this bugcheck **19 times between 2026-06-24 and 2026-07-06**, always with
the identical signature and identical call sites inside cormem.sys. On 2026-07-06 we
captured it with a full (Active) memory dump, so the complete user-mode call chain is
known — see section 3. It reproduces with both `SapBufferWithTrash` and plain
`SapBuffer` buffer objects, so the trash-buffer resource is not required for the
defect. A second Active dump (occurrence #19, same day, plain-`SapBuffer`
configuration) shows the **frame-for-frame identical user-mode chain**, with the
entire target VA range already unmapped (every PTE zero) at the time of the fatal
second unmap request. The defect has two layers:

- **User mode** (`cor_cam_SapGige_s.dll` / `CorMem.dll`): the transfer
  connect/disconnect bookkeeping unmaps the same mapping record twice (or retains a
  record for a mapping that was already released through another path).
- **Kernel mode** (`cormem.sys`): the unmap IOCTL is passed to `MmUnmapLockedPages`
  unvalidated, so a stale user-mode request crashes the operating system instead of
  failing the call. Independently of the bookkeeping bug, a user-mode process should
  not be able to bugcheck the machine through this driver.

## 2. Environment

- OS: Windows 11 Enterprise, build 26100 (x64), 16 logical processors
- Sapera LT 9.12 — component versions from the crashed process:
  - `SapClassBasic91.dll` 9.12.00.2431
  - `corapi.dll` 9.12.00.2431
  - `cor_cam_SapGige_s.dll` 7.00.00.1703
  - `CorGigEServerLib.dll` 7.00.00.1703
  - `CorMem.dll` 9.00
  - `CorHost.dll` 9.00
  - `cormem.sys` 9.12.00.2431 (2026-03-31)
  - `CorGigeFilter.sys` 7.00.01.1700
- Camera: Teledyne DALSA **Genie Nano-M1930** (monochrome, GigE Vision); a color Genie
  Nano is also in use on this system and earlier crashes occurred with it as well.
  Serial numbers: `[FILL IN]`
- Application: Micro-Manager 2.0 (Java host process `javaw.exe`) with a C++ device
  adapter built on Sapera++ (`SapAcqDevice` + `SapBufferWithTrash(3, …)` +
  `SapAcqDeviceToBuf` with transfer callback). Adapter source is public:
  Micro-Manager `mmCoreAndDevices` repository, `DeviceAdapters/SaperaGigE`.

## 3. Complete call chain (from the 2026-07-06 Active memory dump)

Bugcheck: `0x0000001A (0x1230, 0xffff978c409d38c0, 0x000001fb60b70000, 0x2)`,
process `javaw.exe`.

```
Application (Java) sets the camera's PixelFormat property (Mono8 -> Mono10)
  adapter: SaperaGigE::OnPixelType
  adapter: SaperaGigE::SynchronizeBuffers
  adapter: SaperaGigE::DestroySaperaPipelineForReconfigure_   ; first call: Xfer->Destroy()
  SapClassBasic91!SapTransfer::Destroy+0x6a
  SapClassBasic91!SapTransfer::Disconnect+0x6c
  CorApi!CorXferDisconnect+0x7e
  cor_cam_SapGige_s   (internal frames; module has no unwind info)
  CorMem!CorMemUnmapPhysMemory+0x83
  KERNELBASE!DeviceIoControl
  ntdll!NtDeviceIoControlFile
  --- kernel ---
  nt!IofCallDriver
  cormem+0x32df                       ; identical call sites in every dump
  cormem+0x6cbb
  nt!MmUnmapLockedPages+0x34a
  nt!MiUnmapLockedPagesInUserSpace+0x138
  nt!KeBugCheckEx (0x1A / 0x1230)
```

Memory state of the VA being unmapped (bugcheck parameter 3, `0x1fb60b70000`), from
the same dump: the **first page is valid** (a plain user read/write page, PFN
0x289217 — i.e., the freed VA has already been partially reused by an unrelated
allocation) and **every following page of the range is not valid (PTE = 0)**. A live
locked-pages mapping would have all pages valid; this is the footprint of a second
unmap of an already-released mapping. A full thread sweep of the process shows all
other Sapera threads parked in waits (`CorXferWait`, `CorGetControl`, GigE server
threads) — no concurrent mapping activity at crash time.

## 4. Reproduction scenario — minimal known case

The 2026-07-06 crash occurred in a **12-second-old process** with a trivially simple
history, which should narrow the search considerably:

1. Fresh `javaw.exe` starts; Sapera objects created **once**:
   `SapAcqDevice::Create`, `SapBufferWithTrash(3, acqDevice)::Create`,
   `SapAcqDeviceToBuf::Create` (transfer connected). No acquisition was started
   (no Snap, no Grab).
2. ~9 seconds later, the application changes `PixelFormat` Mono8 → Mono10, which
   requires the standard reconfigure sequence. The **first call** of that sequence —
   `SapTransfer::Destroy()` on the transfer created in step 1 — crashes the machine
   inside `Disconnect`'s unmap of a buffer mapping.

In other words: one Connect, zero acquisitions, one Disconnect → bugcheck. The same
machine had completed six identical open/use/close cycles in the preceding 8 minutes
without incident, so the defect is intermittent (timing/allocation dependent), but
when it fires it is always this exact chain. Historical crashes "when closing the
application" match the same chain via the shutdown teardown (which also begins with
`SapTransfer::Destroy()`).

The second Active-dump occurrence (2026-07-06 13:41) adds a possibly relevant detail:
the **preceding** application session was closed while a live grab was running; that
session's teardown completed cleanly (transfer frozen, all objects destroyed, no SDK
errors), and the crashing process re-opened the same camera ~3 seconds later, ran a
short live grab, and then hit the bugcheck on its first `SapTransfer::Destroy()` (a
PixelFormat-change reconfigure, ~80 seconds in). So the defect reproduces both on a
connection that never carried a frame (first Active dump) and on one that had grabbed
live frames (second Active dump), including under rapid close/reopen cycling of the
same camera; the only invariant across all occurrences is the transfer Disconnect
itself.

Notes that may help triage:

- Buffer type is the SDK default (`SapDefBufferType`). The crash occurs with both
  `SapBufferWithTrash(3, …)` and plain `SapBuffer(3, …)` created against the
  `SapAcqDevice` — we changed the adapter to plain `SapBuffer` on 2026-07-06 as an
  experiment and hit the identical bugcheck 15 minutes later (`070626-7312-01.dmp`),
  so the trash resource's mapping is not the double-released record.
- The camera exposes TurboDrive (`turboTransferEnable` present); we have not
  correlated its state with the crashes.
- Teledyne's own CamExpert has never produced this bugcheck on the same machine and
  cameras.

## 5. Occurrences and dumps

19 identical bugchecks (WER event 1001) between 2026-06-24 and 2026-07-06. Retained
dumps (available on request; the Active dump is 6.3 GB):

| Dump | Date/time (UTC-5) | Type |
|------|-------------------|------|
| `062726-7000-01.dmp` | 2026-06-27 18:21 | minidump |
| `063026-8031-01.dmp` | 2026-06-30 12:36 | minidump |
| `063026-7171-01.dmp` | 2026-06-30 12:55 | minidump |
| `070126-7296-01.dmp` | 2026-07-01 11:18 | minidump |
| `070126-7093-01.dmp` | 2026-07-01 11:53 | minidump |
| `070626-7250-01.dmp` + `MEMORY.DMP` | 2026-07-06 12:55 | minidump + **Active dump (full user-mode state)** |
| `070626-7312-01.dmp` + second `MEMORY.DMP` | 2026-07-06 13:41 | minidump + **Active dump** (plain-`SapBuffer` build; identical user-mode chain, triggered by a PixelFormat reconfigure) |

All six minidumps contain the byte-identical kernel stack
(`cormem+0x32df → cormem+0x6cbb → MmUnmapLockedPages`, reached from
`NtDeviceIoControlFile` on a `javaw.exe` thread), so we believe every occurrence is
the same defect.

## 6. Expected behavior / requests

1. `SapTransfer::Disconnect` (GigE path) must not issue an unmap for a mapping record
   that has already been released — please review the buffer-mapping bookkeeping in
   `cor_cam_SapGige_s.dll` / `CorMem.dll` for a double-release on the
   connect/disconnect path. We have already ruled out the trash-buffer resource
   (reproduces with plain `SapBuffer`); remaining candidates are the frame-buffer
   mappings themselves and any TurboDrive-related internal buffers.
2. Independently: `cormem.sys` should validate unmap requests (or track its own live
   mappings) so that a stale request fails the IOCTL instead of calling
   `MmUnmapLockedPages` on invalid state and bugchecking the machine. At most this
   should leak a buffer until process exit — never `KeBugCheckEx`.
3. Please advise whether a known issue matching this signature is fixed in any release
   newer than the versions in section 2, and whether there is a recommended workaround
   (e.g., specific buffer type, disabling TurboDrive, alternative teardown sequence)
   until a fix is available.

## 7. Contact

- Reporter: `[FILL IN name / institution]`
- Email: `[FILL IN]`
- Sapera LT license/registration: `[FILL IN if applicable]`

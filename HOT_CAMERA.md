# Camera running hot during acquisition

## Symptom

The Teledyne DALSA Genie GigE camera(s) get noticeably hot to the touch during
use with the `SaperaGigE` device adapter.

## Finding

`StartSequenceAcquisition()` puts the camera into **free-running continuous
acquisition** via `Xfer_->Grab()` (`SaperaGigE.cpp:738`) -- the same call
`GigeCameraDemoDlg::OnGrab()` uses in the Sapera SDK's own demo. In this mode
the sensor exposes/reads out and the GigE Vision link transmits continuously
at the camera's native frame rate for as long as the sequence is running.

**Fixed:** `interval_ms` is now applied as a software frame-pacing gate in
`XferCallback` (`SaperaGigE.cpp:1349`) -- frames arriving before the requested
interval has elapsed since the last *delivered* frame are dropped, rather than
all being pushed into the circular buffer at the camera's native rate. This
does not change the camera's free-run readout itself (the sensor/link still
run continuously at native rate during the sequence -- see the Caveat below
for why that's expected for this class of camera); it only changes how many
of those frames MMCore actually receives. Genuine rate-limiting at the
sensor/transmitter would need `TriggerMode`/`AcquisitionFrameRate` (see below).

`SnapImage()` (`SaperaGigE.cpp:493`) arms the transfer for exactly one frame
via `Xfer_->Snap(1)`, waits for it, and then the *transfer* goes idle again.
It was originally expected that this would avoid the heat. **In practice the
camera is reported to run hot during single snaps as well.** That observation
downgrades free-running acquisition from "the mechanism" to "at most a
contributing factor" -- see the Caveat below. It is consistent with how a
GigE Vision camera works: once connected and initialized, the sensor analog
front-end, FPGA, GigE PHY, and (on the color sensor) the onboard ISP are all
powered continuously. `Snap` vs `Grab` only changes whether frames are
exposed/read-out/transmitted, not whether that silicon is powered. So the
camera being warm even when not actively transferring frames is expected.

## What's in the MM property interface

`Initialize()`'s `deviceFeatures` map (`SaperaGigE.cpp:294-318`) exposes:
`PixelFormat`, `ExposureTime`, `Gain`, various read-only identity/sensor
fields, binning, ROI (`OffsetX/Y`, `Width/Height`), `ImageTimeout`, and
`SensorTemperature`.

**Fixed:** `AcquisitionFrameRate` (a standard GenICam SFNC feature, same
naming convention as `"ExposureTime"`/`"Gain"`/`"PixelFormat"` above) is now
exposed as a real, writable `MM::Float` property via
`SetUpFrameRateProperty()`/`OnAcquisitionFrameRate()`, caps the free-run rate
directly if the camera supports rate-limiting in continuous mode. It is
self-contained and fail-soft -- absence on a given camera is logged and
skipped, never fails `Initialize()`. **This could not be exercised against
real hardware**: `AcquisitionFrameRate`/`AcquisitionFrameRateEnable`/
`AcquisitionFrameRateControlMode` are camera-firmware-defined GenICam names,
not Sapera SDK constants, and none of them could be found anywhere in the
local SDK install. Confirm against a real camera (Teledyne's CamExpert, under
"Acquisition Control", shows whether a given camera exposes these and what
values they accept) before relying on it.

**Not yet implemented:** `TriggerMode` (`Off`/`On`) + `TriggerSource` --
switching the sensor from continuous free-run to only exposing on a trigger
(software or external) would keep it idle between frames instead of running
continuously during a sequence. Same caveat as above: whether a given camera
supports this is a per-device question, checkable via CamExpert. If
supported, it could be added as a `deviceFeatures` map entry the same way
every other feature already is -- the existing pattern guards each entry
with `IsFeatureAvailable()` (`SaperaGigE.cpp:343-350`) and silently skips it
if unsupported.

## Caveat

GigE PoE cameras (especially color sensors with an onboard ISP) run warm
from baseline power dissipation regardless of software -- the sensor and
electronics are powered the whole time the camera is connected, not just
while acquiring. Because heat is observed during single `SnapImage()` calls
and not only during sequence acquisition, this baseline dissipation is the
most likely dominant cause, and it **cannot be addressed from this adapter**.

The free-running behavior described above remains real: the sensor/link
still run continuously at native rate during a sequence even with the
`interval_ms` delivery-pacing fix above (it only changes what reaches MMCore,
not the camera's own readout). `TriggerMode`/`AcquisitionFrameRate` are what
would shave the *incremental* heat from continuous readout during long
sequences -- `AcquisitionFrameRate` is now wired up (untested against
hardware, see above); `TriggerMode` is not yet implemented. But none of that
will make a continuously connected camera run cool: that is expected
behavior for this class of camera, not a software bug.

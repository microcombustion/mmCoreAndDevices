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

Both overloads of `StartSequenceAcquisition` accept an `interval_ms`
parameter, but it is **never read or applied anywhere in the function body**:

```cpp
int SaperaGigE::StartSequenceAcquisition(double interval_ms)               // SaperaGigE.cpp:713
int SaperaGigE::StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow)  // SaperaGigE.cpp:729
```

No throttling, no `Sleep`, no GenICam frame-rate-limit feature is set. So
regardless of whether the caller requested a fast burst or a sparse
time-lapse (e.g. one frame every 30 s), the sensor and transmitter run
flat-out for the full duration the sequence is armed; `XferCallback`
(`SaperaGigE.cpp:1211`) just keeps pushing every captured frame into the
circular buffer. Pacing/dropping is left entirely to whatever called
`StartSequenceAcquisition`.

`SnapImage()` (`SaperaGigE.cpp:466`) arms the transfer for exactly one frame
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

This also means actual frame timing during a "timed" sequence isn't what a
user would expect: frames arrive back-to-back at the camera's native rate
rather than spaced by `interval_ms` -- a correctness issue independent of the
heat.

## What's missing from the MM property interface

`Initialize()`'s `deviceFeatures` map (`SaperaGigE.cpp:276-317`) currently
exposes: `PixelFormat`, `ExposureTime`, `Gain`, various read-only
identity/sensor fields, binning, ROI (`OffsetX/Y`, `Width/Height`),
`ImageTimeout`, and `SensorTemperature`. Nothing is wired up for trigger mode
or frame-rate limiting, so there is currently no way -- via this adapter --
to keep the sensor idle between frames during a sequence, or to cap its
free-run rate.

## Suggested next steps (not yet implemented)

GenICam's standard feature set (SFNC) -- the same naming convention every
existing property here already uses (`"ExposureTime"`, `"Gain"`,
`"PixelFormat"` are all SFNC names passed straight to
`AcqDevice_.SetFeatureValue`/`GetFeatureValue`) -- defines the controls that
would address this:

- **`TriggerMode`** (`Off`/`On`) + **`TriggerSource`** -- switch the sensor
  from continuous free-run to only exposing on a trigger (software or
  external), so it is idle between frames instead of running continuously
  during a sequence.
- **`AcquisitionFrameRate`** -- caps the free-run rate directly, if the
  camera supports rate-limiting in continuous mode.

Whether the specific connected camera implements these is a per-device
question. Teledyne's CamExpert (see `reference_sapera_sdk` notes) shows,
under its "Acquisition Control" feature category, whether `TriggerMode` /
`AcquisitionFrameRate` exist for a given camera and what values they accept
-- no code change needed to check.

If they are supported, candidate properties could be added to the
`deviceFeatures` map the same way every other feature already is. The
existing pattern guards each entry with `IsFeatureAvailable()`
(`SaperaGigE.cpp:326-333`) and silently skips it if unsupported, so adding
candidates for `TriggerMode`/`TriggerSource`/`AcquisitionFrameRate` would be
safe even if a given camera doesn't expose all three.

## Caveat

GigE PoE cameras (especially color sensors with an onboard ISP) run warm
from baseline power dissipation regardless of software -- the sensor and
electronics are powered the whole time the camera is connected, not just
while acquiring. Because heat is observed during single `SnapImage()` calls
and not only during sequence acquisition, this baseline dissipation is the
most likely dominant cause, and it **cannot be addressed from this adapter**.

The free-running behavior described above remains real and is still worth
fixing for the *frame-timing correctness* reason (a "timed" sequence does not
actually honor `interval_ms`), and `TriggerMode`/`AcquisitionFrameRate` would
shave the *incremental* heat from continuous readout during long sequences.
But none of that will make a continuously connected camera run cool: that is
expected behavior for this class of camera, not a software bug.

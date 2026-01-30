# Cross-check of AlliedVisionCamera Analyses (No Vimba SDK Available)

This assessment cross-checks `_EXPOSURE_GLITCH.md`, `_FIX_ROI.md`, and
`_POTENTIAL_CRASH_ISSUES.md` against the current source code in this repo.
Because the Vimba SDK is not available here, any points that depend on SDK
ownership or callback threading are noted as **needs SDK confirmation**.

## Confirmed Findings (from code)

1. **SetExposure ignores SetProperty errors**
   - `SetExposure` calls `SetProperty(...)` and immediately fires
     `OnExposureChanged` with the requested value, even if `SetProperty`
     fails.  
   - Code: `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp:428`.

2. **Exposure value adjustment can diverge from reported value**
   - In `onProperty` (AfterSet), invalid values are adjusted and written
     via `setFeatureValue`, and `OnPropertyChanged` is fired with the
     adjusted value.  
   - `SetExposure` does not read back the adjusted value and still reports
     the requested value via `OnExposureChanged`.  
   - Code: `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp:654`.

3. **GetROI returns copies, not output parameters**
   - `GetROI` builds a `std::map<const char*, unsigned>` with copies of the
     output parameters and writes to the map values, not the caller
     references.  
   - This explains `GetROI` returning `0,0,0,0` regardless of actual values.  
   - Code: `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp:503`.

4. **Buffers are reallocated on every SnapImage call**
   - `SnapImage` always calls `resizeImageBuffer()`.  
   - `resizeImageBuffer()` deletes and reallocates all buffers, even when
     the payload size is unchanged.  
   - Code: `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp:1144`
     and `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp:216`.

## Plausible but Requires Vimba SDK Confirmation

1. **Potential use-after-free from buffer reallocation**
   - Risk depends on whether Vimba retains references to buffers after
     frame revoke/queue flush in the snap path.  
   - Without SDK docs, cannot confirm whether immediate delete/realloc
     is safe.

2. **Invalidation callback race**
   - Feature invalidation callbacks are registered and call
     `UpdateProperty()` without any mutex protection around
     `m_buffer`, `m_bufferSize`, or `m_currentPixelFormat`.  
   - Whether this is a real race depends on Vimba callback threading
     guarantees.  
   - Code: `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp:278`.

3. **Stack-allocated VmbFrame_t lifetime**
   - `SnapImage` uses a stack `VmbFrame_t` and passes it into SDK calls.  
   - Safe only if the SDK does not retain the frame pointer beyond the
     synchronous `VmbCaptureFrameWait_t` and subsequent revoke path.  
   - Requires SDK ownership rules to confirm.

## Next Steps (Recommended)

1. **Fix confirmed logic bugs**
   - Update `GetROI` to write into caller outputs (e.g., map of pointers).
   - In `SetExposure`, check the return value of `SetProperty` and consider
     reading back the actual exposure before firing
     `OnExposureChanged`.

2. **Reduce buffer churn**
   - Only reallocate buffers when size changes; avoid delete/realloc on
     every `SnapImage`.

3. **Validate Vimba SDK behavior**
   - Confirm:
     - whether `VmbFrame_t` must remain valid after `VmbFrameAnnounce_t`
       until revoke.
     - whether invalidation callbacks can fire on background threads.
     - whether buffers may be referenced after `VmbFrameRevokeAll_t`.

4. **Consider synchronization**
   - If callbacks are asynchronous, add a mutex around buffer/pixel format
     state used by `SnapImage`, `resizeImageBuffer`, and callbacks.

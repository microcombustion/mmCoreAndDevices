# AlliedVisionCamera Stochastic Segfault Analysis

## Symptom

Stochastic segfaults during image capture loops (hundreds of images) where exposure time is changed before each capture. Hard crashes without stack traces in Python (pymmcore).

## Critical Issues

### 1. Buffer Reallocation on Every Snap (Most Likely Cause)

In `SnapImage()` at `AlliedVisionCamera.cpp:1159`:

```cpp
int AlliedVisionCamera::SnapImage()
{
    if (IsCapturing())
        return DEVICE_CAMERA_BUSY_ACQUIRING;
    resizeImageBuffer();  // Called unconditionally EVERY time
    // ...
}
```

And `resizeImageBuffer()` at line 226:

```cpp
VmbError_t AlliedVisionCamera::resizeImageBuffer()
{
    // ...
    for (size_t i = 0; i < MAX_FRAMES; i++)
    {
        delete[] m_buffer[i];                        // Deletes old buffer
        m_buffer[i] = new VmbUint8_t[m_bufferSize];  // Allocates new
    }
    return VmbErrorSuccess;
}
```

**Every single `SnapImage()` call deletes and reallocates all 7 frame buffers**, even when the size hasn't changed. If the Vimba SDK still holds references to previously announced frames (from a prior capture), this is a **use-after-free**.

### 2. Race Between Frame Revoke and Buffer Deletion

The flow in `SnapImage` is:
1. `resizeImageBuffer()` - deletes buffers
2. Announce frame with new buffer
3. Capture
4. `StopSequenceAcquisition()` → calls `VmbFrameRevokeAll_t`

On the *next* `SnapImage` call:
1. `resizeImageBuffer()` deletes buffers **before confirming** previous frames are fully revoked

The Vimba SDK may still be referencing the old buffer when it's deleted.

### 3. No Thread Synchronization

The code registers feature invalidation callbacks (`AlliedVisionCamera.cpp:298`):

```cpp
err = m_sdk->VmbFeatureInvalidationRegister_t(m_handle, feature->name, vmbCallback, this);
```

When exposure is changed before each capture, the callback fires and calls `UpdateProperty()`. There's no mutex protecting:

- `m_currentPixelFormat` (read during capture, written during property changes)
- `m_buffer` array (reallocated during snap, potentially accessed by callbacks)
- `m_bufferSize` / `m_payloadSize`

### 4. Stack-Allocated Frame Structure

In `SnapImage()` at line 1161:

```cpp
VmbFrame_t frame;  // Stack allocated!
frame.buffer = m_buffer[0];
err = m_sdk->VmbFrameAnnounce_t(m_handle, &frame, sizeof(VmbFrame_t));
```

If any async SDK behavior references this frame after `SnapImage` returns, it accesses invalid stack memory.

## Recommended Fixes

### Fix 1: Only resize buffers when necessary

```cpp
VmbError_t AlliedVisionCamera::resizeImageBuffer()
{
    VmbError_t err = m_sdk->VmbPayloadSizeGet_t(m_handle, &m_payloadSize);
    if (err != VmbErrorSuccess)
    {
        LOG_ERROR(err, "Error while reading payload size");
        return err;
    }

    VmbUint32_t newBufferSize = std::max(
        GetImageWidth() * GetImageHeight() * m_currentPixelFormat.getBytesPerPixel(),
        m_payloadSize);

    // Only reallocate if size increased or buffers not yet allocated
    if (newBufferSize <= m_bufferSize && m_buffer[0] != nullptr)
    {
        return VmbErrorSuccess;
    }

    m_bufferSize = newBufferSize;

    for (size_t i = 0; i < MAX_FRAMES; i++)
    {
        delete[] m_buffer[i];
        m_buffer[i] = new VmbUint8_t[m_bufferSize];
    }

    return VmbErrorSuccess;
}
```

### Fix 2: Add mutex for thread safety

In `AlliedVisionCamera.h`, add:

```cpp
#include <mutex>
// ...
private:
    mutable std::mutex m_bufferMutex;
```

Then protect buffer access in `SnapImage`, `resizeImageBuffer`, and property callbacks.

### Fix 3: Use class member for SnapImage frame

In `AlliedVisionCamera.h`:

```cpp
private:
    VmbFrame_t m_snapFrame;  // Instead of stack allocation
```

### Fix 4: Ensure frames are revoked before buffer deletion

```cpp
int AlliedVisionCamera::SnapImage()
{
    if (IsCapturing())
        return DEVICE_CAMERA_BUSY_ACQUIRING;

    // Ensure any previous frames are fully revoked
    m_sdk->VmbFrameRevokeAll_t(m_handle);

    // Now safe to resize
    resizeImageBuffer();
    // ...
}
```

Or check if buffer size actually changed before calling `resizeImageBuffer()` at all.

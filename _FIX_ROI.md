# AlliedVisionCamera GetROI Bug Analysis

## Problem

`GetROI` always returns `0, 0, 0, 0` even though `SetROI` works and individual properties (OffsetX, OffsetY, Width, Height) return correct values.

## Root Cause

In `DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp:510-528`, the map stores **copies** of the output parameters, not references:

```cpp
int AlliedVisionCamera::GetROI(unsigned &x, unsigned &y, unsigned &xSize, unsigned &ySize)
{
    std::map<const char *, unsigned> fields = { { g_OffsetX, x }, { g_OffsetY, y }, { g_Width, xSize }, { g_Height, ySize } };

    VmbError_t err = VmbErrorSuccess;
    for (auto &field : fields)
    {
        std::string value{};
        err = getFeatureValue(field.first, value);
        if (err != VmbErrorSuccess)
        {
            LOG_ERROR(err, "Error while getting ROI!");
            break;
        }
        field.second = atoi(value.data());  // Writes to map copy, not original!
    }

    return err;
}
```

The map value type `unsigned` creates copies on initialization. Writing to `field.second` modifies the map's internal copy but never propagates back to the caller's variables.

## Fix

Use pointers to the output parameters:

```cpp
int AlliedVisionCamera::GetROI(unsigned &x, unsigned &y, unsigned &xSize, unsigned &ySize)
{
    std::map<const char *, unsigned *> fields = {
        { g_OffsetX, &x }, { g_OffsetY, &y }, { g_Width, &xSize }, { g_Height, &ySize }
    };

    VmbError_t err = VmbErrorSuccess;
    for (auto &field : fields)
    {
        std::string value{};
        err = getFeatureValue(field.first, value);
        if (err != VmbErrorSuccess)
        {
            LOG_ERROR(err, "Error while getting ROI!");
            break;
        }
        *field.second = atoi(value.data());
    }

    return err;
}
```

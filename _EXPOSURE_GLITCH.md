# AlliedVisionCamera Exposure Setting Issues

## Symptom

Occasional failure to set exposure times - `GetExposure()` doesn't return the value previously set via `SetExposure()`.

## Issues

### 1. SetExposure Ignores Return Value

At `AlliedVisionCamera.cpp:435-439`:

```cpp
void AlliedVisionCamera::SetExposure(double exp_ms)
{
    SetProperty(m_exposureFeatureName.c_str(), CDeviceUtils::ConvertToString(exp_ms * MS_TO_US));
    GetCoreCallback()->OnExposureChanged(this, exp_ms);  // Reports success regardless!
}
```

`SetProperty` returns an error code that is **completely ignored**. If the property set fails, `OnExposureChanged` is still called with the requested value, not the actual value.

### 2. Silent Value Adjustment

In `onProperty` at lines 701-727, when setting a value outside valid range or not matching the increment:

```cpp
case MM::ActionType::AfterSet:
    err = setFeatureValue(&featureInfo, featureName.c_str(), propertyValue);
    if (err == VmbErrorInvalidValue)
    {
        // Get limits and adjust
        std::string adjustedValue = adjustValue(featureInfo, min, max, std::stod(propertyValue));
        err = setFeatureValue(&featureInfo, featureName.c_str(), adjustedValue);
        // Value is silently changed to adjustedValue!
    }
```

If you request 15.3ms but the camera only supports increments of 1ms, it silently sets 15.0ms. `SetExposure` never knows about this adjustment.

### 3. Race with Invalidation Callbacks

The camera registers invalidation callbacks for all features (line 298). The sequence can be:

1. `SetExposure(X)` sets value X via property system
2. Camera hardware adjusts internally to Y (due to timing constraints)
3. Invalidation callback fires asynchronously
4. Callback calls `UpdateProperty()`, which reads Y from camera
5. `GetExposure()` returns Y, not X

### 4. No Verification of Set Value

`SetExposure` is fire-and-forget. It doesn't read back the actual value from the camera to confirm the setting took effect.

## Recommended Fixes

### Fix 1: Check return value and verify

```cpp
void AlliedVisionCamera::SetExposure(double exp_ms)
{
    int err = SetProperty(m_exposureFeatureName.c_str(),
                          CDeviceUtils::ConvertToString(exp_ms * MS_TO_US));
    if (err != DEVICE_OK)
    {
        LOG_ERROR(err, "Failed to set exposure");
        return;
    }

    // Read back actual value and report that
    double actualExposure = GetExposure();
    GetCoreCallback()->OnExposureChanged(this, actualExposure);
}
```

### Fix 2: Return actual exposure after adjustment

Modify `onProperty` to propagate the adjusted value back, or have `SetExposure` always read back the value after setting.

### Fix 3: Add synchronization

Add mutex protection around exposure get/set operations to prevent races with invalidation callbacks:

```cpp
void AlliedVisionCamera::SetExposure(double exp_ms)
{
    std::lock_guard<std::mutex> lock(m_propertyMutex);
    SetProperty(m_exposureFeatureName.c_str(), CDeviceUtils::ConvertToString(exp_ms * MS_TO_US));
    // Read back and report actual value
    double actual = GetExposure();
    GetCoreCallback()->OnExposureChanged(this, actual);
}
```

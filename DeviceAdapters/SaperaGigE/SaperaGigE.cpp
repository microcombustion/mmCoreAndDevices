/////////////////////////////////////////////////////////
// FILE:		  SaperaGigE.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-------------------------------------------------------
// DESCRIPTION:   Device adapter for GigE Vision cameras exposed through the
//                Teledyne DALSA Sapera LT / Sapera++ SDK. Users and developers
//                need a compatible Sapera SDK/runtime installation.
//
// AUTHOR:        Robert Frazee, rfraze1@lsu.edu
//                Ingmar Schoegl, ischoegl@lsu.edu
//
// LICENSE:       This file is distributed under the BSD license.
//                License text is included with the source distribution.
//
//                This file is distributed in the hope that it will be useful,
//                but WITHOUT ANY WARRANTY; without even the implied warranty
//                of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
//                IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//                CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
//                INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.

#include "SaperaGigE.h"

#include "ModuleInterface.h"

using namespace std;

const char* g_CameraDeviceName = "Sapera GigE camera adapter";
const char* g_CameraServer = "AcquisitionDevice";

///////////////////////////////////////////////////////////////////////////////
// Exported MMDevice API
///////////////////////////////////////////////////////////////////////////////

/**
 * List all supported hardware devices here
 */
MODULE_API void InitializeModuleData()
{
    RegisterDevice(g_CameraDeviceName, MM::CameraDevice, "Sapera GigE camera device adapter");
}

MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
    if (deviceName == 0)
        return 0;

    // decide which device class to create based on the deviceName parameter
    if (strcmp(deviceName, g_CameraDeviceName) == 0)
    {
        // create camera
        return new SaperaGigE();
    }

    // Supplied name not recognized.
    return 0;
}

MODULE_API void DeleteDevice(MM::Device* pDevice)
{
    delete pDevice;
}

std::wstring s2ws(const std::string& s)
{
    int len;
    int slength = (int)s.length() + 1;
    len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, 0, 0);
    wchar_t* buf = new wchar_t[len];
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, buf, len);
    std::wstring r(buf);
    delete[] buf;
    return r;
}

int ErrorBox(std::string text, std::string caption)
{
    return MessageBox(NULL, s2ws(caption).c_str(), s2ws(text).c_str(), (MB_ICONERROR | MB_OK));
}

///////////////////////////////////////////////////////////////////////////////
// SaperaGigE implementation
// ~~~~~~~~~~~~~~~~~~~~~~~

/**
* SaperaGigE constructor.
* Initializes Micro-Manager defaults and enumerates Sapera acquisition servers
* to create the pre-initialization camera-selection property. Sapera device,
* buffer, and transfer objects are created later in Initialize().
*/
SaperaGigE::SaperaGigE() :
    bytesPerPixel_(1),
    bitsPerPixel_(8),
    initialized_(false),
    sequenceStarted_(false),
    transferActive_(false),
    imageCounter_(0),
    intervalMs_(0.0),
    numImages_(0),
    stopRequested_(false),
    Buffers_(NULL),
    Roi_(NULL),
    roiX_(0),
    roiY_(0),
    roiW_(-1),
    roiH_(-1),
    AcqDeviceToBuf_(NULL),
    Xfer_(NULL),
    isColor_(false),
    Conv_(NULL)
{

    // call the base class method to set-up default error codes/messages
    InitializeDefaultErrorMessages();

    if (GetListOfAvailableCameras() != DEVICE_OK)
        LogMessage("No Sapera camera found!", false);
}

/**
* SaperaGigE destructor.
* If this device used as intended within the Micro-Manager system,
* Shutdown() will be always called before the destructor. But in any case
* we need to make sure that all resources are properly released even if
* Shutdown() was not called.
*/
SaperaGigE::~SaperaGigE()
{
    if (initialized_)
        Shutdown();

    // Defensive: a joinable std::thread destructor calls std::terminate. Shutdown() above
    // already requests and joins the stop worker on every normal path, but guard here too
    // in case this object is destroyed without going through Shutdown().
    if (stopWorker_.joinable())
    {
        RequestStop();
        stopWorker_.join();
    }

    NumberOfWorkableCameras_ = 0;
}

int SaperaGigE::GetListOfAvailableCameras()

{
    CreateProperty(MM::g_Keyword_Name, g_CameraDeviceName, MM::String, true);

    // Query Sapera acquisition servers and expose them as selectable camera endpoints.
    if (!(SapManager::DetectAllServers(SapManager::DetectServerAll)))
    {
        LogMessage("No Sapera acquisition servers detected", false);
        return DEVICE_NOT_CONNECTED;
    }

    acqDeviceList_.clear();
    NumberOfAvailableCameras_ = SapManager::GetServerCount();
    char serverName[CORSERVER_MAX_STRLEN];
    for (int serverIndex = 0; serverIndex < NumberOfAvailableCameras_; serverIndex++)
    {
        if (SapManager::GetResourceCount(serverIndex, SapManager::ResourceAcqDevice) != 0)
        {
            // Get Server Name Value
            SapManager::GetServerName(serverIndex, serverName, sizeof(serverName));
            acqDeviceList_.push_back(serverName);
        }
    }

    if (acqDeviceList_.size() == 0)
    {
        return DEVICE_NOT_CONNECTED;
    }
    else {
        // add available servers to property and set active device to first server in the list
        activeDevice_ = acqDeviceList_[0];
        CPropertyAction* pAct = new CPropertyAction(this, &SaperaGigE::OnCamera);
        int nRet = CreateProperty(g_CameraServer, acqDeviceList_[0].c_str(), MM::String, false, pAct, true);
        assert(nRet == DEVICE_OK);
        nRet = SetAllowedValues(g_CameraServer, acqDeviceList_);
        return DEVICE_OK;
        
    }
}

/**
* Obtains device name.
* Required by the MM::Device API.
*/
void SaperaGigE::GetName(char* name) const
{
    // We just return the name we use for referring to this
    // device adapter.
    CDeviceUtils::CopyLimitedString(name, g_CameraDeviceName);
}

/**
  * Set camera
  */
int SaperaGigE::OnCamera(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        string CameraName;
        pProp->Get(CameraName);

        for (auto servername : acqDeviceList_) {

            if (servername.compare(CameraName) == 0) 
            {
                initialized_ = false;
                activeDevice_ = CameraName;
                return DEVICE_OK;
            }

        }
        assert(!"Unrecognized Camera");
    }
    return DEVICE_OK;
}

/**
   * Camera Name
   */
int SaperaGigE::OnCameraName(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
    }
    else if (eAct == MM::BeforeGet)
    {
        pProp->Set(activeDevice_.c_str());
    }
    return DEVICE_OK;
}

/**
* Intializes the hardware.
* Typically we access and initialize hardware at this point.
* Device properties are typically created here as well.
* Required by the MM::Device API.
*/
int SaperaGigE::Initialize()
{
    if (initialized_)
        return DEVICE_OK;

    //CPropertyAction* pAct;
    int ret;

    LogMessage((std::string)"Initialize device '" + activeDevice_ + "'");
    // Assign the member loc_ (declared in the header) -- do not shadow it with a local of the
    // same name, or Shutdown()'s loc_.GetServerName() log below ends up reading a
    // default-constructed SapLocation.
    loc_ = SapLocation(activeDevice_.c_str());
    AcqDevice_ = SapAcqDevice(loc_, false);
    if (!AcqDevice_.Create())
    {
        ret = FreeHandles();
        if (ret != DEVICE_OK)
            return ret;
        return DEVICE_INVALID_INPUT_PARAM;
    }
    AcqFeature_ = SapFeature(loc_);
    if (!AcqFeature_.Create())
    {
        ret = FreeHandles();
        if (ret != DEVICE_OK)
            return ret;
        return DEVICE_NATIVE_MODULE_FAILED;
    }
    const auto failInitialize = [this](int error) {
        // Initialization can fail after Sapera handles or DMA buffers were created, before
        // initialized_ is set. Clean up Sapera kernel handles here; FreeHandles()
        // intentionally abandons Sapera++ wrapper allocations after Destroy() to avoid
        // cormem.sys crashes in their destructors.
        int cleanup = FreeHandles();
        return cleanup != DEVICE_OK ? cleanup : error;
    };

    NumberOfWorkableCameras_++;

    // Detect a color sensor the same way Sapera's own CamExpert/demo apps do, so
    // SynchronizeBuffers() knows whether to set up Bayer->RGB conversion.
    isColor_ = AcqDevice_.IsRawBayerOutput();
    CreateProperty("CameraColorType", isColor_ ? "Color" : "Monochrome", MM::String, true);

    // set up feature type correspondence
    std::map<SapFeature::Type, MM::PropertyType> featureTypes;

    featureTypes[SapFeature::TypeString] = MM::String;
    featureTypes[SapFeature::TypeEnum] = MM::String;
    featureTypes[SapFeature::TypeInt32] = MM::Integer;
    featureTypes[SapFeature::TypeFloat] = MM::Float;
    featureTypes[SapFeature::TypeDouble] = MM::Float;
    featureTypes[SapFeature::TypeUndefined] = MM::String;

    // set property list
    // -----------------

    std::map< const char*, feature > deviceFeatures;

    deviceFeatures[MM::g_Keyword_PixelType] = define_feature("PixelFormat", false,
        new CPropertyAction(this, &SaperaGigE::OnPixelType));
    deviceFeatures[MM::g_Keyword_Exposure] = define_feature("ExposureTime", false,
        new CPropertyAction(this, &SaperaGigE::OnExposure));
    deviceFeatures[MM::g_Keyword_Gain] = define_feature("Gain", false,
        new CPropertyAction(this, &SaperaGigE::OnGain));
    deviceFeatures["DeviceVendorName"] = define_feature("DeviceVendorName", true, NULL);
    deviceFeatures["DeviceFamilyName"] = define_feature("DeviceFamilyName", true, NULL);
    deviceFeatures[MM::g_Keyword_CameraName] = define_feature("DeviceModelName", true, NULL);
    deviceFeatures["DeviceVersion"] = define_feature("DeviceVersion", true, NULL);
    deviceFeatures["DeviceManufacturerInfo"] = define_feature("DeviceManufacturerInfo", true, NULL);
    deviceFeatures["deviceManufacturerPartNumber"] = define_feature("deviceManufacturerPartNumber", true, NULL);
    deviceFeatures["DeviceFirmwareVersion"] = define_feature("DeviceFirmwareVersion", true, NULL);
    deviceFeatures["DeviceSerialNumber"] = define_feature("DeviceSerialNumber", true, NULL);
    deviceFeatures[MM::g_Keyword_CameraID] = define_feature("DeviceUserID", true, NULL);
    deviceFeatures["deviceMacAddress"] = define_feature("deviceMacAddress", true, NULL);
    deviceFeatures["sensorColorType"] = define_feature("sensorColorType", true, NULL);
    deviceFeatures["PixelCoding"] = define_feature("PixelCoding", true, NULL);
    deviceFeatures["BlackLevelSelector"] = define_feature("BlackLevelSelector", false,
        new CPropertyAction(this, &SaperaGigE::OnBlackLevelSelector));
    deviceFeatures["BlackLevel"] = define_feature("BlackLevel", false,
        new CPropertyAction(this, &SaperaGigE::OnBlackLevel));
    deviceFeatures["pixelSizeInput"] = define_feature("pixelSizeInput", true, NULL);
    deviceFeatures["SensorShutterMode"] = define_feature("SensorShutterMode", true, NULL);
    deviceFeatures["binningMode"] = define_feature("binningMode", false,
        new CPropertyAction(this, &SaperaGigE::OnBinningMode));
    deviceFeatures["SensorWidth"] = define_feature("SensorWidth", true, NULL);
    deviceFeatures["SensorHeight"] = define_feature("SensorHeight", true, NULL);
    deviceFeatures["PixelSize"] = define_feature("PixelSize", true,
        new CPropertyAction(this, &SaperaGigE::OnPixelSize));
    deviceFeatures["OffsetX"] = define_feature("OffsetX", false,
        new CPropertyAction(this, &SaperaGigE::OnOffsetX));
    deviceFeatures["OffsetY"] = define_feature("OffsetY", false,
        new CPropertyAction(this, &SaperaGigE::OnOffsetY));
    deviceFeatures["Width"] = define_feature("Width", false,
        new CPropertyAction(this, &SaperaGigE::OnWidth));
    deviceFeatures["Height"] = define_feature("Height", false,
        new CPropertyAction(this, &SaperaGigE::OnHeight));
    deviceFeatures["ImageTimeout"] = define_feature("ImageTimeout", false,
        new CPropertyAction(this, &SaperaGigE::OnImageTimeout));
    deviceFeatures["TurboTransferEnable"] = define_feature("turboTransferEnable", true, NULL);
    deviceFeatures["DeviceTemperature"] = define_feature("DeviceTemperature", true,
        new CPropertyAction(this, &SaperaGigE::OnTemperature));


    // device features
    //for (auto const& x : deviceFeatures)
    std::map< const char*, feature >::iterator x;
    for (x = deviceFeatures.begin(); x != deviceFeatures.end(); x++)
    {
        feature f = x->second;
        BOOL isAvailable;
        AcqDevice_.IsFeatureAvailable(f.name, &isAvailable);
        if (!isAvailable)
        {
            LogMessage((std::string)"Feature '" + f.name
                + "' is not supported");
            continue;
        }

        LogMessage((std::string)"Adding feature '" + f.name
            + "' as property '" + x->first + "'");
        AcqDevice_.GetFeatureInfo(f.name, &AcqFeature_);
        SapFeature::Type sapType;
        AcqFeature_.GetType(&sapType);
        std::map< SapFeature::Type, MM::PropertyType>::iterator it;
        it = featureTypes.find(sapType);
        MM::PropertyType eType;
        if (it == featureTypes.end())
            eType = MM::String;
        else
            eType = it->second;

        char value[MM::MaxStrLength];
        if (!AcqDevice_.GetFeatureValue(f.name, value, sizeof(value)))
        {
            LogMessage((std::string)"Failed to read initial value for '" + f.name
                + "'; using cached default");
            snprintf(value, sizeof(value), "%s", eType == MM::String ? "" : "0");
        }

        if (f.action == NULL)
            ret = CreateProperty(x->first, value, eType, f.readOnly);
        else
            ret = CreateProperty(x->first, value, eType, f.readOnly, f.action);
        assert(ret == DEVICE_OK);

        if (sapType == SapFeature::TypeEnum)
        {
            // GetEnumCount()/GetEnumString() list every entry statically defined in the
            // camera's GenICam XML, regardless of whether it is currently selectable given
            // the camera's other current feature settings (a standard GenICam concept).
            // IsEnumEnabled() is the SDK's per-entry availability check (the same one
            // GigEFlatFieldDemo uses before offering a value) -- skip entries it reports as
            // disabled, or SetFeatureValue() rejects them later with a GenApi AccessException.
            vector<string> allowed;
            int count;
            AcqFeature_.GetEnumCount(&count);
            for (int i = 0; i < count; i++)
            {
                BOOL enabled;
                if (!AcqFeature_.IsEnumEnabled(i, &enabled) || !enabled)
                    continue;
                AcqFeature_.GetEnumString(i, value, sizeof(value));
                allowed.push_back(value);
            }
            ret = SetAllowedValues(x->first, allowed);
            assert(ret == DEVICE_OK);
        }
    }

    // binning
    ret = SetUpBinningProperties();
    if (ret != DEVICE_OK)
        return failInitialize(ret);

    // frame rate cap (best-effort; never fails Initialize -- see SetUpFrameRateProperty)
    ret = SetUpFrameRateProperty();
    if (ret != DEVICE_OK)
        return failInitialize(ret);

    // set up Sapera / Micro-Manager buffers
    LogMessage((std::string) "Setting up buffers");
    ret = SynchronizeBuffers();
    if (ret != DEVICE_OK)
        return failInitialize(ret);

    double low = 0.0;
    double high = 0.0;

    // Set up gain
    AcqDevice_.GetFeatureInfo("Gain", &AcqFeature_);
    AcqFeature_.GetMax(&high);
    AcqFeature_.GetMin(&low);
    SetPropertyLimits(MM::g_Keyword_Gain, low, high);

    // Set up exposure
    AcqDevice_.GetFeatureInfo("ExposureTime", &AcqFeature_);
    AcqFeature_.GetMin(&low); // us
    AcqFeature_.GetMax(&high); // us
    SetPropertyLimits(MM::g_Keyword_Exposure, low / 1000., high / 1000.);

    // synchronize all properties
    // --------------------------
    ret = UpdateStatus();
    if (ret != DEVICE_OK)
        return failInitialize(ret);

    initialized_ = true;
    return DEVICE_OK;
}

/**
* Shuts down (unloads) the device.
* Ideally this method will completely unload the device and release all resources.
* Shutdown() may be called multiple times in a row.
* Required by the MM::Device API.
*/
int SaperaGigE::Shutdown()
{
    if (!initialized_)
        return DEVICE_OK;
    LogMessage((std::string)"Shutting down device '" + loc_.GetServerName() + "'");

    initialized_ = false;

    // Tear down any live sequence (and join its worker) before touching the SDK objects below.
    RequestStop();
    if (stopWorker_.joinable())
        stopWorker_.join();

    // Guard: after a failed SynchronizeBuffers() rebuild, Xfer_ may be NULL even with
    // initialized_ true. FreeHandles() is still safe to call (it null-checks everything).
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (Xfer_)
    {
        Xfer_->Freeze();
        // Log a timeout but always proceed to FreeHandles(): leaving Sapera kernel objects
        // live (and cormem.sys holding DMA-locked pages) is exactly the state that produces
        // a 0x1a/0x1230 BSOD on the next process that initializes the driver.
        if (!Xfer_->Wait(5000))
            LogMessage("Timed out waiting for transfer to stop during shutdown");
    }
    return FreeHandles();
}

/**
* Frees Sapera buffers and such
*/
int SaperaGigE::FreeHandles()
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    LogMessage((std::string)"Destroy Sapera buffers and devices");
    int ret = DestroySaperaPipeline_();
    if (!AcqFeature_.Destroy()) ret = DEVICE_ERR;
    if (!AcqDevice_.Destroy()) ret = DEVICE_ERR;
    return ret;
}

int SaperaGigE::DestroySaperaPipeline_()
{
    // Accumulate errors but attempt every destroy: an early return on first failure leaves
    // later kernel objects live, which is exactly the state cormem.sys cannot safely clean
    // up after a process exit (0x1a/0x1230 BSOD on the next init).
    int ret = DEVICE_OK;
    if (Xfer_ && *Xfer_ && !Xfer_->Destroy()) ret = DEVICE_ERR;
    if (Conv_ && *Conv_ && !Conv_->Destroy()) ret = DEVICE_ERR;
    // Roi_ (SapBufferRoi child) must be destroyed after Xfer_ but before its parent Buffers_.
    // Xfer_ holds references to all child buffer handles including Roi_'s m_hTrashChild;
    // destroying Roi_ while Xfer_ is still live causes cormem.sys to BSOD (0x1230).
    if (Roi_ && *Roi_ && !Roi_->Destroy()) ret = DEVICE_ERR;
    if (Buffers_ && !Buffers_->Destroy()) ret = DEVICE_ERR;

    // Do not delete these Sapera++ wrappers on final shutdown. Their Destroy() methods
    // have released the driver/kernel handles above; running the wrapper destructors during
    // application exit or after a disturbed camera link has repeatedly reached cormem.sys'
    // fragile MmUnmapLockedPages path (bugcheck 0x1a/0x1230). Intentionally leaking this
    // small amount of process memory is preferable to a system crash, and the OS reclaims it
    // when the Python/MMCore process exits.
    AcqDeviceToBuf_ = NULL;
    Xfer_ = NULL;
    Conv_ = NULL;
    Roi_ = NULL;
    Buffers_ = NULL;
    return ret;
}

int SaperaGigE::DestroySaperaPipelineForReconfigure_()
{
    // Reconfiguration needs the SDK objects Destroy()ed in dependency order, but deleting
    // and reconstructing SapAcqDeviceToBuf/SapBufferWithTrash has proven to exercise an
    // unstable cormem.sys unmap path. Keep the long-lived C++ wrappers and re-Create() them
    // below; only the ROI wrapper changes because its geometry is constructor-only.
    int ret = DEVICE_OK;
    if (Xfer_ && *Xfer_ && !Xfer_->Destroy()) ret = DEVICE_ERR;
    if (Conv_ && *Conv_ && !Conv_->Destroy()) ret = DEVICE_ERR;
    if (Roi_ && *Roi_ && !Roi_->Destroy()) ret = DEVICE_ERR;
    delete Roi_;
    Roi_ = NULL;
    if (Buffers_ && *Buffers_ && !Buffers_->Destroy()) ret = DEVICE_ERR;
    return ret;
}

/**
* Performs exposure and grabs a single image.
* This function blocks during the actual exposure and returns immediately afterwards
* Required by the MM::Camera API.
*/
int SaperaGigE::SnapImage()
{
    // Reject snap while a callback-driven sequence is live; snap and grab share img_.
    {
        MMThreadGuard g(seqLock_);
        if (sequenceStarted_ || transferActive_)
            return DEVICE_CAMERA_BUSY_ACQUIRING;
        transferActive_ = true;
    }
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    int ret = DEVICE_OK;
    // Start image capture
    Xfer_->SetCommandTimeout(1000);
    if (!Xfer_->Snap(1))
    {
        LogMessage("Failure occurred while capturing a single image");
        ret = DEVICE_ERR;
    }
    else if (!Xfer_->Wait(16000))
    {
        // Wait for either the capture to finish or 16 seconds, whichever is first.
        ret = DEVICE_ERR;
    }
    {
        MMThreadGuard g(seqLock_);
        transferActive_ = false;
    }
    return ret;
}



/**
* Returns pixel data.
* Required by the MM::Camera API.
* The calling program will assume the size of the buffer based on the values
* obtained from GetImageBufferSize(), which in turn should be consistent with
* values returned by GetImageWidth(), GetImageHeight() and GetImageBytesPerPixel().
* The calling program also assumes that camera never changes the size of
* the pixel buffer on its own. In other words, the buffer can change only if
* appropriate properties are set (such as binning, pixel type, etc.)
*/
const unsigned char* SaperaGigE::GetImageBuffer()
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    // For a color sensor, demosaic the just-acquired raw Bayer buffer into Conv_'s RGB
    // output buffer before reading pixels out of it. The SDK demos instead drive Convert()
    // asynchronously through a SapProcessing helper (Execute()/ProCallback) so a live-preview
    // UI thread stays unblocked; this adapter has no live preview and both call sites
    // (here and XferCallback) need the converted buffer ready immediately, so it calls
    // Convert() synchronously instead. SapProcessing exposes no Wait() on a specific buffer
    // index, so adopting it would mean adding new synchronization, not just swapping the call.
    SapBuffer* src = Buffers_;
    if (isColor_)
    {
        Conv_->Convert();
        src = Conv_->GetOutputBuffer();
    }
    // Put Sapera buffer into Micro-Manager Buffer
    src->ReadRect(Roi_->GetXMin(), Roi_->GetYMin(), img_.Width(), img_.Height(),
        const_cast<unsigned char*>(img_.GetPixels()));
    // Return location of the Micro-Manager Buffer
    return const_cast<unsigned char*>(img_.GetPixels());
}

/**
* Returns image buffer X-size in pixels.
* Required by the MM::Camera API.
*/
unsigned SaperaGigE::GetImageWidth() const
{
    return img_.Width();
}

/**
* Returns image buffer Y-size in pixels.
* Required by the MM::Camera API.
*/
unsigned SaperaGigE::GetImageHeight() const
{
    return img_.Height();
}

/**
* Returns image buffer pixel depth in bytes.
* Required by the MM::Camera API.
*/
unsigned SaperaGigE::GetImageBytesPerPixel() const
{
    return img_.Depth();
}

/**
* Returns the number of components (1 for monochrome, 4 for the 32-bit BGRA
* produced by Conv_ on a detected color sensor).
* Required by the MM::Camera API.
*/
unsigned SaperaGigE::GetNumberOfComponents() const
{
    return isColor_ ? 4 : 1;
}

/**
* Returns the bit depth (dynamic range) of the pixel.
* This does not affect the buffer size, it just gives the client application
* a guideline on how to interpret pixel values.
* Required by the MM::Camera API.
*/
unsigned SaperaGigE::GetBitDepth() const
{
    return bitsPerPixel_;
}

/**
* Returns the size in bytes of the image buffer.
* Required by the MM::Camera API.
*/
long SaperaGigE::GetImageBufferSize() const
{
    return img_.Width() * img_.Height() * img_.Depth();
}

/**
* Sets the camera Region Of Interest.
* Required by the MM::Camera API.
* This command will change the dimensions of the image.
* Depending on the hardware capabilities the camera may not be able to configure the
* exact dimensions requested - but should try do as close as possible.
* If the hardware does not have this capability the software should simulate the ROI by
* appropriately cropping each frame.
* This adapter rebuilds the Sapera ROI/buffer/transfer chain so the Micro-Manager
* image buffer matches the requested ROI.
* @param x - top-left corner coordinate
* @param y - top-left corner coordinate
* @param xSize - width
* @param ySize - height
*/
int SaperaGigE::SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    // img_ geometry must stay stable while the callback is streaming into it.
    if (IsCapturing())
        return DEVICE_CAMERA_BUSY_ACQUIRING;
    LogMessage((std::string)"Setting Region of Interest");
    if (xSize == 0 && ySize == 0)
        return ClearROI();
    // Validate against the current full-frame dimensions before tearing down. An out-of-range
    // ROI would make SapBufferRoi::Create() fail mid-rebuild, leaving the adapter in a broken
    // state (Xfer_/Roi_ null, initialized_ true). Also catches xSize==0 or ySize==0 alone.
    UINT32 fullWidth = 0, fullHeight = 0;
    if (!IsFeatureAvailable("Width") || !IsFeatureAvailable("Height"))
        return DEVICE_INVALID_PROPERTY;
    if (!AcqDevice_.GetFeatureValue("Width", &fullWidth) ||
        !AcqDevice_.GetFeatureValue("Height", &fullHeight))
        return DEVICE_ERR;
    if (xSize == 0 || ySize == 0 ||
        (UINT32)x + xSize > fullWidth || (UINT32)y + ySize > fullHeight)
        return DEVICE_INVALID_INPUT_PARAM;
    // Changing ROI requires a full Sapera chain teardown/rebuild (Roi_ → Xfer_ → Conv_ →
    // Buffers_). Destroying/creating Roi_ alone while Buffers_ is live causes cormem.sys
    // to call MmUnmapLockedPages on still-active DMA pages → BSOD. SynchronizeBuffers()
    // performs the full safe sequence and calls ResizeImageBuffer() at the end, which
    // (after the update below) uses roiW_/roiH_ to size img_ correctly.
    roiX_ = (int)x; roiY_ = (int)y; roiW_ = (int)xSize; roiH_ = (int)ySize;
    return SynchronizeBuffers();
}

/**
* Returns the actual dimensions of the current ROI
* Required by the MM::Camera API.
*/
int SaperaGigE::GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    // Roi_ can be null if SynchronizeBuffers() failed mid-rebuild; fall back to stored coords.
    if (!Roi_)
    {
        x = (unsigned)roiX_;
        y = (unsigned)roiY_;
        xSize = (roiW_ > 0) ? (unsigned)roiW_ : 0;
        ySize = (roiH_ > 0) ? (unsigned)roiH_ : 0;
        return DEVICE_OK;
    }
    x = Roi_->GetXMin();
    y = Roi_->GetYMin();
    xSize = Roi_->GetWidth();
    ySize = Roi_->GetHeight();
    return DEVICE_OK;
}

/**
* Resets the Region of Interest to full frame.
* Required by the MM::Camera API.
*/
int SaperaGigE::ClearROI()
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    // img_ geometry must stay stable while the callback is streaming into it.
    if (IsCapturing())
        return DEVICE_CAMERA_BUSY_ACQUIRING;
    // Same teardown requirement as SetROI: full chain rebuild via SynchronizeBuffers().
    roiX_ = 0; roiY_ = 0; roiW_ = -1; roiH_ = -1;
    return SynchronizeBuffers();
}

/**
* Returns the current exposure setting in milliseconds.
* Required by the MM::Camera API.
*/
double SaperaGigE::GetExposure() const
{
    char buf[MM::MaxStrLength];
    int ret = GetProperty(MM::g_Keyword_Exposure, buf);
    if (ret != DEVICE_OK)
        return 0.0;
    return atof(buf);
}

/**
* Sets exposure in milliseconds.
* Required by the MM::Camera API.
*/
void SaperaGigE::SetExposure(double exp)
{
    int ret = SetProperty(MM::g_Keyword_Exposure, CDeviceUtils::ConvertToString(exp));
}

/**
* Returns the current binning factor.
* Required by the MM::Camera API.
*/
int SaperaGigE::GetBinning() const
{
    char buf[MM::MaxStrLength];
    int ret = GetProperty(MM::g_Keyword_Binning, buf);
    if (ret != DEVICE_OK)
        return 1;
    return atoi(buf);
}

/**
* Sets binning factor.
* Required by the MM::Camera API.
*/
int SaperaGigE::SetBinning(int binF)
{
    return SetProperty(MM::g_Keyword_Binning, CDeviceUtils::ConvertToString(binF));
}

/**
* Idempotent, non-blocking stop signal. Safe to call from the callback thread
* (self-stop) or the MMCore thread (user stop): sets stopRequested_ and wakes the stop
* worker, which performs the actual teardown off-thread. stopRequested_ is guarded
* solely by stopMutex_ -- see the member declaration in the header for why.
*/
void SaperaGigE::RequestStop()
{
    std::lock_guard<std::mutex> lock(stopMutex_);
    stopRequested_ = true;
    stopCv_.notify_one();
}

/**
* Body of the stop worker thread started by StartSequenceAcquisition(): waits for
* RequestStop() to signal, then runs the actual teardown. Checking the predicate while
* holding stopMutex_ means a stop requested before this wait is reached is still observed
* immediately (no lost wakeup). stopMutex_ is released before performTeardown_() runs, so
* RequestStop() is never blocked behind the (potentially slow) teardown.
*/
void SaperaGigE::StopWorkerLoop_()
{
    std::unique_lock<std::mutex> lock(stopMutex_);
    stopCv_.wait(lock, [this] { return stopRequested_.load(); });
    lock.unlock();
    performTeardown_();
}

/**
* Single teardown path for streaming: the only place that flips sequenceStarted_ to
* false, stops the hardware, and fires AcqFinished. Idempotent (a stop requested with no
* sequence running is a no-op). Runs only on the stop worker thread -- never call this
* from XferCallback (see that function's comment for why).
*/
void SaperaGigE::performTeardown_()
{
    seqLock_.Lock();
    if (!sequenceStarted_ && !transferActive_)
    {
        seqLock_.Unlock();
        return;
    }
    bool notifyCore = sequenceStarted_;
    seqLock_.Unlock();

    // Never hold seqLock_ while calling into the Sapera SDK or MMCore (they can block or
    // re-enter).
    {
        std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
        Xfer_->Freeze();
        if (!Xfer_->Wait(5000))
            LogMessage("Timed out waiting for transfer to stop");
    }
    {
        MMThreadGuard g(seqLock_);
        sequenceStarted_ = false;
        transferActive_ = false;
    }
    if (notifyCore)
        GetCoreCallback()->AcqFinished(this, 0);
}

/**
* Stop a running sequence acquisition.
* Called by the client / acquisition engine (an early user stop -- a finite acquisition
* that reaches its count, or an InsertImage() error, self-stops via RequestStop() from
* XferCallback instead, see there). Requests the stop and joins the worker thread, so the
* hardware is guaranteed stopped before this returns. No-op if no worker exists or the
* sequence already finished.
* Required by the MM::Camera API.
*/
int SaperaGigE::StopSequenceAcquisition()
{
    RequestStop();
    if (stopWorker_.joinable() && std::this_thread::get_id() != stopWorker_.get_id())
        stopWorker_.join();
    return DEVICE_OK;
}

/**
* Start a free-running (live) sequence acquisition.
* Required by the MM::Camera API.
*/
int SaperaGigE::StartSequenceAcquisition(double interval_ms)
{
    // Live view: an effectively unbounded frame count (no self-stop on count) and no
    // overflow-stop override -- the actionable overflow rule is "stop on any InsertImage
    // error" regardless (see XferCallback), so stopOnOverflow itself is not stored.
    return StartSequenceAcquisition((std::numeric_limits<long>::max)(), interval_ms, false);
}

/**
* Start a sequence acquisition driven by the native Sapera transfer callback. The camera
* free-runs; XferCallback paces frames to interval_ms, tags delivered frames with the
* correct component count, and self-stops (via RequestStop(), on a dedicated worker
* thread -- never inline in the callback) once numImages frames have been delivered or
* InsertImage() reports an error.
* Required by the MM::Camera API.
*/
int SaperaGigE::StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow)
{
    // stopOnOverflow is intentionally not stored: MMCore's InsertImage() contract is to
    // stop on any error regardless of this flag (see XferCallback).
    (void)stopOnOverflow;

    if (numImages <= 0)
        return DEVICE_INVALID_INPUT_PARAM;

    {
        MMThreadGuard g(seqLock_);
        if (sequenceStarted_ || transferActive_)
            return DEVICE_CAMERA_BUSY_ACQUIRING;
    }

    // Join any prior (already finished, not-yet-joined) worker. Safe here: the check above
    // guarantees no sequence is currently active, so a *live* worker cannot still be
    // blocked on the stop condition variable -- joining one of those would hang forever.
    if (stopWorker_.joinable())
        stopWorker_.join();

    {
        std::lock_guard<std::mutex> lock(stopMutex_);
        stopRequested_ = false;
    }

    // Start the Sapera transfer first; only arm the Micro-Manager sequence on success.
    {
        MMThreadGuard g(seqLock_);
        transferActive_ = true;
    }
    {
        std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
        if (!Xfer_->Grab())
        {
            MMThreadGuard g(seqLock_);
            transferActive_ = false;
            LogMessage("Failed to start continuous acquisition");
            return DEVICE_ERR;
        }
    }

    int ret = GetCoreCallback()->PrepareForAcq(this);
    if (ret != DEVICE_OK)
    {
        // PrepareForAcq failed after the transfer started: undo the start. The sequence
        // never armed, so there is nothing to AcqFinish (AcqFinished pairs only with a
        // successful PrepareForAcq).
        {
            std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
            Xfer_->Freeze();
            Xfer_->Wait(5000);
        }
        {
            MMThreadGuard g(seqLock_);
            transferActive_ = false;
        }
        return ret;
    }

    {
        MMThreadGuard g(seqLock_);
        imageCounter_ = 0;
        intervalMs_ = interval_ms;
        numImages_ = numImages;
        nextFrameTime_ = GetCurrentMMTime(); // first frame is due immediately
        sequenceStarted_ = true;
    }

    // Start the fresh worker outside seqLock_.
    stopWorker_ = std::thread(&SaperaGigE::StopWorkerLoop_, this);
    return DEVICE_OK;
}

bool SaperaGigE::IsCapturing() {
    // Reflects both sequence acquisition and synchronous SnapImage()/teardown transfer
    // windows, so callers do not touch Sapera buffers while the driver is active.
    MMThreadGuard g(seqLock_);
    return sequenceStarted_ || transferActive_;
}

bool SaperaGigE::IsFeatureAvailable(const char* featureName)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    BOOL isAvailable = FALSE;
    if (!AcqDevice_.IsFeatureAvailable(featureName, &isAvailable) || !isAvailable)
    {
        LogMessage((std::string)"Feature '" + featureName + "' is not supported or currently unavailable");
        return false;
    }
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// SaperaGigE Action handlers
///////////////////////////////////////////////////////////////////////////////

/**
* Handles "Binning" property.
*/
int SaperaGigE::OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (eAct == MM::AfterSet)
    {
        // Reconfiguration reallocates buffers; reject while streaming (see img_ invariant).
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;
        if (!IsFeatureAvailable("BinningVertical") || !IsFeatureAvailable("BinningHorizontal"))
            return DEVICE_INVALID_PROPERTY;
        long binSize;
        pProp->Get(binSize);
        if (!AcqDevice_.SetFeatureValue("BinningVertical", int(binSize)))
            return DEVICE_ERR;
        if (!AcqDevice_.SetFeatureValue("BinningHorizontal", int(binSize)))
            return DEVICE_ERR;
        roiX_ = 0; roiY_ = 0; roiW_ = -1; roiH_ = -1;
        return SynchronizeBuffers();
    }
    // MM::BeforeGet returns the value cached in the property.
    return DEVICE_OK;
}

int SaperaGigE::OnBinningMode(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (eAct == MM::AfterSet)
    {
        if (!IsFeatureAvailable("binningMode"))
            return DEVICE_INVALID_PROPERTY;
        std::string value;
        pProp->Get(value);
        AcqDevice_.SetFeatureValue("binningMode", value.c_str());
    }
    // MM::BeforeGet returns the value cached in the property.
    return DEVICE_OK;
}

int SaperaGigE::OnPixelSize(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (eAct == MM::AfterSet)
    {
        return DEVICE_CAN_NOT_SET_PROPERTY;
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!IsFeatureAvailable("PixelSize"))
            return DEVICE_OK;
        UINT32 value;
        if (!AcqDevice_.GetFeatureValue("PixelSize", &value))
            return DEVICE_ERR;
        pProp->Set((long)value);
    }
    return DEVICE_OK;
}

long SaperaGigE::CheckValue(const char* key, long value)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (!IsFeatureAvailable(key))
        return value;
    INT64 minVal, maxVal, inc;
    AcqDevice_.GetFeatureInfo(key, &AcqFeature_);
    AcqFeature_.GetInc(&inc);
    AcqFeature_.GetMin(&minVal);
    AcqFeature_.GetMax(&maxVal);

    long out = (value / (long)inc) * (long)inc;
    out = std::clamp(out, (long)minVal, (long)maxVal);

    if (value != out)
        LogMessage((std::string)"Encountered invalid value for '" + key
            + "': corrected " + std::to_string((INT64)value) + " to " + std::to_string((INT64)out));
    return out;
}

int SaperaGigE::OnOffsetX(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (eAct == MM::AfterSet)
    {
        long value;
        pProp->Get(value);

        if (!IsFeatureAvailable("OffsetX"))
            return DEVICE_INVALID_PROPERTY;
        value = CheckValue("OffsetX", value);
        if (!AcqDevice_.SetFeatureValue("OffsetX", value))
            return DEVICE_ERR;
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!IsFeatureAvailable("OffsetX"))
            return DEVICE_OK;
        UINT32 value;
        if (!AcqDevice_.GetFeatureValue("OffsetX", &value))
            return DEVICE_ERR;
        pProp->Set((long)value);
    }
    return DEVICE_OK;
}

int SaperaGigE::OnOffsetY(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (eAct == MM::AfterSet)
    {
        long value;
        pProp->Get(value);

        if (!IsFeatureAvailable("OffsetY"))
            return DEVICE_INVALID_PROPERTY;
        value = CheckValue("OffsetY", value);
        if (!AcqDevice_.SetFeatureValue("OffsetY", value))
            return DEVICE_ERR;
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!IsFeatureAvailable("OffsetY"))
            return DEVICE_OK;
        UINT32 value;
        if (!AcqDevice_.GetFeatureValue("OffsetY", &value))
            return DEVICE_ERR;
        pProp->Set((long)value);
    }
    return DEVICE_OK;
}

int SaperaGigE::OnWidth(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (eAct == MM::AfterSet)
    {
        // Reconfiguration reallocates buffers; reject while streaming (see img_ invariant).
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;
        if (!IsFeatureAvailable("Width"))
            return DEVICE_INVALID_PROPERTY;
        long value;
        pProp->Get(value);

        value = CheckValue("Width", value);
        roiX_ = 0; roiY_ = 0; roiW_ = -1; roiH_ = -1;
        int ret = SynchronizeBuffers("", value, -1);
        if (ret != DEVICE_OK)
            return ret;
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!IsFeatureAvailable("Width"))
            return DEVICE_OK;
        UINT32 value;
        if (!AcqDevice_.GetFeatureValue("Width", &value))
            return DEVICE_ERR;
        pProp->Set((long)value);
    }
    return DEVICE_OK;
}

int SaperaGigE::OnHeight(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (eAct == MM::AfterSet)
    {
        // Reconfiguration reallocates buffers; reject while streaming (see img_ invariant).
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;
        if (!IsFeatureAvailable("Height"))
            return DEVICE_INVALID_PROPERTY;
        long value;
        pProp->Get(value);

        value = CheckValue("Height", value);
        roiX_ = 0; roiY_ = 0; roiW_ = -1; roiH_ = -1;
        int ret = SynchronizeBuffers("", -1, value);
        if (ret != DEVICE_OK)
            return ret;
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!IsFeatureAvailable("Height"))
            return DEVICE_OK;
        UINT32 value;
        if (!AcqDevice_.GetFeatureValue("Height", &value))
            return DEVICE_ERR;
        pProp->Set((long)value);
    }
    return DEVICE_OK;
}

int SaperaGigE::OnImageTimeout(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (eAct == MM::AfterSet)
    {
        // Reconfiguration reallocates buffers; reject while streaming (see img_ invariant).
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;
        if (!IsFeatureAvailable("ImageTimeout"))
            return DEVICE_INVALID_PROPERTY;
        double value;
        pProp->Get(value);
        int ret = SynchronizeBuffers("", -1, -1, value);
        if (ret != DEVICE_OK)
            return ret;
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!IsFeatureAvailable("ImageTimeout"))
            return DEVICE_OK;
        double value;
        if (!AcqDevice_.GetFeatureValue("ImageTimeout", &value))
            return DEVICE_ERR;
        pProp->Set(value);
    }
    return DEVICE_OK;
}

int SaperaGigE::OnTemperature(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        return DEVICE_CAN_NOT_SET_PROPERTY;
    }
    else if (eAct == MM::BeforeGet)
    {
        if (IsCapturing())
            return DEVICE_OK;
        if (!IsFeatureAvailable("DeviceTemperature"))
            return DEVICE_OK;
        std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
        double value;
        if (!AcqDevice_.GetFeatureValue("DeviceTemperature", &value))
        {
            LogMessage("Failed to get feature value for 'DeviceTemperature'; keeping cached property value");
            return DEVICE_OK;
        }
        pProp->Set(value);
    }
    return DEVICE_OK;
}

/**
* Handles "PixelType" property.
*/
int SaperaGigE::OnPixelType(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    char pixelFormat[10];
    if (!IsFeatureAvailable("PixelFormat"))
        return DEVICE_INVALID_PROPERTY;
    if (!AcqDevice_.GetFeatureValue("PixelFormat", pixelFormat, sizeof(pixelFormat)))
        return DEVICE_ERR;
    if (eAct == MM::AfterSet)
    {
        // Reconfiguration reallocates buffers; reject while streaming (see img_ invariant).
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;
        std::string value;
        pProp->Get(value);

        if (!(value.compare(pixelFormat) == 0))
        {
            //resize the SapBuffer
            int ret = SynchronizeBuffers(value);
            if (ret != DEVICE_OK)
                return ret;
        }
    }
    else if (eAct == MM::BeforeGet)
    {
        pProp->Set(pixelFormat);
    }

    return DEVICE_OK;
}

/**
* Handles "Gain" property.
*/
int SaperaGigE::OnGain(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    double gain = 1.;
    if (eAct == MM::AfterSet)
    {
        if (!IsFeatureAvailable("Gain"))
            return DEVICE_INVALID_PROPERTY;
        pProp->Get(gain);
        if (!AcqDevice_.SetFeatureValue("Gain", gain))
        {
            LogMessage("Failed to set feature value for 'Gain'");
            return DEVICE_ERR;
        }
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!IsFeatureAvailable("Gain"))
            return DEVICE_OK;
        if (!AcqDevice_.GetFeatureValue("Gain", &gain))
        {
            LogMessage("Failed to get feature value for 'Gain'");
            return DEVICE_ERR;
        }
        pProp->Set(gain);
    }

    return DEVICE_OK;
}

int SaperaGigE::OnBlackLevelSelector(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (eAct == MM::AfterSet)
    {
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;
        if (!IsFeatureAvailable("BlackLevelSelector"))
            return DEVICE_INVALID_PROPERTY;
        std::string value;
        pProp->Get(value);
        if (!AcqDevice_.SetFeatureValue("BlackLevelSelector", value.c_str()))
        {
            LogMessage("Failed to set feature value for 'BlackLevelSelector'");
            return DEVICE_ERR;
        }
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!IsFeatureAvailable("BlackLevelSelector"))
            return DEVICE_OK;
        char value[MM::MaxStrLength];
        if (!AcqDevice_.GetFeatureValue("BlackLevelSelector", value, sizeof(value)))
        {
            LogMessage("Failed to get feature value for 'BlackLevelSelector'");
            return DEVICE_ERR;
        }
        pProp->Set(value);
    }
    return DEVICE_OK;
}

int SaperaGigE::OnBlackLevel(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    double level;
    if (eAct == MM::AfterSet)
    {
        if (!IsFeatureAvailable("BlackLevel"))
            return DEVICE_INVALID_PROPERTY;
        pProp->Get(level);
        if (!AcqDevice_.SetFeatureValue("BlackLevel", level))
        {
            LogMessage("Failed to set feature value for 'BlackLevel'");
            return DEVICE_ERR;
        }
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!IsFeatureAvailable("BlackLevel"))
            return DEVICE_OK;
        if (!AcqDevice_.GetFeatureValue("BlackLevel", &level))
        {
            LogMessage("Failed to get feature value for 'BlackLevel'");
            return DEVICE_ERR;
        }
        pProp->Set(level);
    }
    return DEVICE_OK;
}

int SaperaGigE::OnExposure(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    // note that GigE units of exposure are us; umanager uses ms
    double exposure;
    if (eAct == MM::AfterSet)
    {
        if (!IsFeatureAvailable("ExposureTime"))
            return DEVICE_INVALID_PROPERTY;
        pProp->Get(exposure);  // ms
        if (!AcqDevice_.SetFeatureValue("ExposureTime", exposure * 1000.0)) // ms to us
        {
            LogMessage("Failed to set feature value for 'ExposureTime'");
            return DEVICE_ERR;
        }
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!IsFeatureAvailable("ExposureTime"))
            return DEVICE_OK;
        if (!AcqDevice_.GetFeatureValue("ExposureTime", &exposure)) // us
        {
            LogMessage("Failed to get feature value for 'ExposureTime'");
            return DEVICE_ERR;
        }
        pProp->Set(exposure / 1000.0);
    }
    return DEVICE_OK;
}

/**
* Handles "AcquisitionFrameRate" property. Set up as a fail-soft optional GenICam/SFNC
* feature by SetUpFrameRateProperty().
*/
int SaperaGigE::OnAcquisitionFrameRate(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    double rate;
    if (eAct == MM::AfterSet)
    {
        if (!IsFeatureAvailable("AcquisitionFrameRate"))
            return DEVICE_INVALID_PROPERTY;
        pProp->Get(rate);

        // Best-effort: a camera may require the rate control to be explicitly enabled
        // before SetFeatureValue("AcquisitionFrameRate", ...) actually takes effect.
        // Absence of either feature is fine -- the rate write below is attempted regardless.
        BOOL hasEnable;
        if (AcqDevice_.IsFeatureAvailable("AcquisitionFrameRateEnable", &hasEnable) && hasEnable)
            AcqDevice_.SetFeatureValue("AcquisitionFrameRateEnable", true);
        BOOL hasMode;
        if (AcqDevice_.IsFeatureAvailable("AcquisitionFrameRateControlMode", &hasMode) && hasMode)
            AcqDevice_.SetFeatureValue("AcquisitionFrameRateControlMode", "Programmable");

        if (!AcqDevice_.SetFeatureValue("AcquisitionFrameRate", rate))
        {
            LogMessage("Failed to set feature value for 'AcquisitionFrameRate'");
            return DEVICE_ERR;
        }
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!IsFeatureAvailable("AcquisitionFrameRate"))
            return DEVICE_OK;
        if (!AcqDevice_.GetFeatureValue("AcquisitionFrameRate", &rate))
        {
            LogMessage("Failed to get feature value for 'AcquisitionFrameRate'");
            return DEVICE_ERR;
        }
        pProp->Set(rate);
    }
    return DEVICE_OK;
}

///////////////////////////////////////////////////////////////////////////////
// Private SaperaGigE methods
///////////////////////////////////////////////////////////////////////////////

/**
* Sync internal image buffer size to the chosen property values.
*/
int SaperaGigE::ResizeImageBuffer()
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    UINT32 width, height;
    if (!IsFeatureAvailable("Height") || !IsFeatureAvailable("Width"))
        return DEVICE_INVALID_PROPERTY;
    if (!AcqDevice_.GetFeatureValue("Height", &height))
        return DEVICE_INVALID_PROPERTY;
    if (!AcqDevice_.GetFeatureValue("Width", &width))
        return DEVICE_INVALID_PROPERTY;

    // When a software ROI is active, img_ must match the ROI dimensions so that
    // XferCallback's ReadRect(roiX_, roiY_, img_.Width(), img_.Height(), ...) reads only
    // the ROI region. Full-frame when roiW_/roiH_ are -1 (no active ROI).
    unsigned roiWidth  = (roiW_ > 0) ? (unsigned)roiW_ : width;
    unsigned roiHeight = (roiH_ > 0) ? (unsigned)roiH_ : height;
    img_.Resize(roiWidth, roiHeight, bytesPerPixel_);

    return DEVICE_OK;
}

/**
 * Generate an image with fixed value for all pixels
 */
void SaperaGigE::GenerateImage()
{
    const int maxValue = (1 << MAX_BIT_DEPTH) - 1; // max for the 12 bit camera
    const double maxExp = 1000;
    double step = maxValue / maxExp;
    unsigned char* pBuf = const_cast<unsigned char*>(img_.GetPixels());
    double exposureMs = GetExposure();
    memset(pBuf, (int)(step * (std::max)(exposureMs, maxExp)), GetImageBufferSize());
}

/*
 * Reformat Sapera Buffer Object
 */
int SaperaGigE::SynchronizeBuffers(std::string pixelFormat, int width, int height, double timeout)
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);

    if (pixelFormat.size() && !IsFeatureAvailable("PixelFormat"))
        return DEVICE_INVALID_PROPERTY;
    if (width > 0 && !IsFeatureAvailable("Width"))
        return DEVICE_INVALID_PROPERTY;
    if (height > 0 && !IsFeatureAvailable("Height"))
        return DEVICE_INVALID_PROPERTY;
    if (timeout > 0 && !IsFeatureAvailable("ImageTimeout"))
        return DEVICE_INVALID_PROPERTY;
    if (!IsFeatureAvailable("PixelSize"))
        return DEVICE_INVALID_PROPERTY;

    // Callers only reach here with the transfer idle: OnPixelType/OnWidth/OnHeight etc.
    // reject via IsCapturing() while a sequence is running, and SnapImage() already blocks
    // on Wait() before returning. Freeze()/Wait() is still used as a conservative Sapera
    // driver barrier before cormem.sys is asked to unmap the old buffers.
    if (Xfer_ && *Xfer_)
    {
        Xfer_->Freeze();
        if (!Xfer_->Wait(5000))
            LogMessage("Timed out waiting for transfer to stop during buffer reconfiguration");
    }
    int destroyRet = DestroySaperaPipelineForReconfigure_();

    // default value
    //
    // The camera can reject a value the enum lists as a possible entry but that isn't
    // currently selectable given other feature settings (GenApi AccessException). The old
    // Sapera pipeline was already torn down above, so do NOT return early here -- fall
    // through to rebuild them against the camera's actual (unchanged) current PixelFormat,
    // and only report the failure (after that rebuild leaves the device in a working state)
    // via pixelFormatFailed below.
    bool pixelFormatFailed = false;
    if (pixelFormat.size())
    {
        if (!AcqDevice_.SetFeatureValue("PixelFormat", pixelFormat.c_str()))
        {
            LogMessage((std::string)"Failed to set feature value for 'PixelFormat' to '"
                + pixelFormat + "'");
            pixelFormatFailed = true;
        }
    }
    if (width > 0)
    {
        if (!AcqDevice_.SetFeatureValue("Width", width))
            LogMessage("Failed to set feature value for 'Width'");
    }
    if (height > 0)
    {
        if (!AcqDevice_.SetFeatureValue("Height", height))
            LogMessage("Failed to set feature value for 'Height'");
    }
    if (timeout > 0)
    {
        if (!AcqDevice_.SetFeatureValue("ImageTimeout", timeout))
            LogMessage("Failed to set feature value for 'ImageTimeout'");
    }

    // synchronize bit depth with camera
    AcqDevice_.GetFeatureValue("PixelSize", &bitsPerPixel_);
    bytesPerPixel_ = (bitsPerPixel_ + 7) / 8;
    if (isColor_)
        bytesPerPixel_ = 4; // Conv_ always normalizes to 32-bit BGRA (SapFormatRGB8888)

    if (Buffers_ == NULL)
    {
        Buffers_ = new SapBufferWithTrash(3, &AcqDevice_);
        AcqDeviceToBuf_ = new SapAcqDeviceToBuf(&AcqDevice_, Buffers_, XferCallback, this);
        Xfer_ = AcqDeviceToBuf_;
    }
    if (isColor_ && Conv_ == NULL)
        Conv_ = new SapColorConversion(&AcqDevice_, Buffers_);
    // Use whatever ROI coordinates are currently stored. Callers that change frame geometry
    // (OnBinning, OnWidth, OnHeight) reset roiX_/roiY_/roiW_/roiH_ to 0,0,-1,-1 before
    // calling here. SetROI()/ClearROI() set the desired values before calling here.
    Roi_ = new SapBufferRoi(Buffers_, roiX_, roiY_, roiW_, roiH_);
    if (isColor_)
    {
        // Per the SDK's own GigEBayerDemo: Enable() may need to modify the acquisition's
        // output format, so it must run before the buffer is created. SetAlign()/
        // SetOutputFormat() are the opposite -- the SDK rejects them ("cannot be called
        // before the Create method") until Conv_->Create() has run, so those are deferred
        // until after Conv_->Create() below.
        if (!Conv_->Enable(TRUE, FALSE))
        {
            LogMessage("Color conversion not supported on this camera; falling back to raw passthrough");
            isColor_ = false;
            bytesPerPixel_ = (bitsPerPixel_ + 7) / 8;
        }
    }
    if (!Buffers_->Create())
    {
        int ret = DestroySaperaPipelineForReconfigure_();
        if (ret != DEVICE_OK)
            return ret;
        return DEVICE_NATIVE_MODULE_FAILED;
    }
    if (!Roi_->Create())
    {
        int ret = DestroySaperaPipelineForReconfigure_();
        if (ret != DEVICE_OK)
            return ret;
        return DEVICE_NATIVE_MODULE_FAILED;
    }
    if (isColor_ && !Conv_->Create())
    {
        int ret = DestroySaperaPipelineForReconfigure_();
        if (ret != DEVICE_OK)
            return ret;
        return DEVICE_NATIVE_MODULE_FAILED;
    }
    if (isColor_)
    {
        Conv_->SetAlign(SapColorConversion::GetAlignModeFromAcqDevice(&AcqDevice_));
        Conv_->SetOutputFormat(SapFormatRGB8888);
    }
    if (Xfer_ && !Xfer_->Create())
    {
        int ret = DestroySaperaPipelineForReconfigure_();
        if (ret != DEVICE_OK)
            return ret;
        return DEVICE_NATIVE_MODULE_FAILED;
    }
    int resizeRet = ResizeImageBuffer();
    if (resizeRet != DEVICE_OK)
        return resizeRet;

    // Reported only now: buffers/transfer/conversion above were already rebuilt against the
    // camera's actual (unchanged) PixelFormat, so the device is left in a working state
    // either way -- this only tells the caller the requested value didn't take effect.
    if (destroyRet != DEVICE_OK)
        return destroyRet;
    if (pixelFormatFailed)
        return DEVICE_INVALID_PROPERTY_VALUE;

    return DEVICE_OK;
}

/*
 * Native Sapera transfer callback: invoked (serially, one completed buffer at a time)
 * when the transfer finishes a frame. This is the streaming path -- it paces frames to
 * intervalMs_, reads the just-completed buffer, tags it with the correct component count,
 * and pushes it into the MMCore circular buffer. On a finite-length completion or an
 * InsertImage() error it calls RequestStop() -- it does not call Freeze()/Wait()/AcqFinished()
 * itself; Sapera serializes transfer callbacks, so blocking on this same transfer's Wait()
 * from in here risks deadlock. Actual teardown runs on the stop worker thread
 * (performTeardown_()).
 *
 * Locking rule: seqLock_ protects state only. We read/update state under the lock, release
 * it, then make the SDK buffer read and the InsertImage call outside the lock.
 */
void SaperaGigE::XferCallback(SapXferCallbackInfo* pInfo)
{
    // The static callback has no instance; recover it from the context passed at
    // SapAcqDeviceToBuf(..., this) construction. The instance is needed for LogMessage.
    SaperaGigE* self = static_cast<SaperaGigE*>(pInfo->GetContext());

    self->seqLock_.Lock();
    bool started = self->sequenceStarted_;
    self->seqLock_.Unlock();
    if (!started)
        return; // performTeardown_() already flipped the flag: drop in-flight frames

    std::unique_lock<std::recursive_mutex> saperaGuard(self->saperaMutex_, std::try_to_lock);
    if (!saperaGuard.owns_lock())
    {
        self->LogMessage("Sapera transfer callback could not acquire SDK lock; stopping sequence");
        self->RequestStop();
        return;
    }

    // Buffer overflow: drop the frame and log it (no blocking MessageBox dialog).
    if (pInfo->IsTrash())
    {
        self->LogMessage((std::string)"Frame(s) acquired in trash buffer: "
            + std::to_string((INT64)pInfo->GetEventCount()));
        return;
    }

    // Software frame-pacing gate: drop frames that arrive before the requested interval has
    // elapsed. Advance nextFrameTime_ from "now" (not the previous deadline) to avoid
    // catch-up bursts. Only a frame that is actually delivered below advances the deadline.
    self->seqLock_.Lock();
    if (self->intervalMs_ > 0.0 && self->GetCurrentMMTime() < self->nextFrameTime_)
    {
        self->seqLock_.Unlock();
        return;
    }
    self->nextFrameTime_ = self->GetCurrentMMTime() + MM::MMTime::fromMs(self->intervalMs_);
    self->seqLock_.Unlock();

    // For a color sensor, demosaic the just-completed raw Bayer buffer into Conv_'s RGB
    // output buffer before reading pixels out of it (synchronously -- see the same note in
    // GetImageBuffer() for why this differs from the SDK demos' SapProcessing-based approach).
    SapBuffer* src = self->Buffers_;
    if (self->isColor_)
    {
        self->Conv_->Convert();
        src = self->Conv_->GetOutputBuffer();
    }

    // Read the just-completed buffer (the no-index ReadRect reads at GetIndex(), the last
    // grabbed buffer) into the single staging buffer img_, then push to the core.
    src->ReadRect(self->Roi_->GetXMin(), self->Roi_->GetYMin(),
        self->img_.Width(), self->img_.Height(),
        const_cast<unsigned char*>(self->img_.GetPixels()));
    saperaGuard.unlock();
    int ret = self->GetCoreCallback()->InsertImage(self, self->img_.GetPixels(),
        self->GetImageWidth(), self->GetImageHeight(), self->GetImageBytesPerPixel(),
        self->GetNumberOfComponents());
    if (ret != DEVICE_OK)
    {
        // Per the MM::Core contract, stop on any InsertImage() error (this is also how
        // a circular-buffer overflow is reported). A frame that never reached MMCore must
        // not count toward the finite total below.
        self->LogMessage("InsertImage failed in transfer callback; stopping sequence");
        self->RequestStop();
        return;
    }

    self->seqLock_.Lock();
    ++self->imageCounter_;
    bool done = self->imageCounter_ >= self->numImages_;
    self->seqLock_.Unlock();
    if (done)
        self->RequestStop();
}


int SaperaGigE::SetUpBinningProperties()
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    BOOL hasHorzBinning;
    BOOL hasVertBinning;
    hasHorzBinning = IsFeatureAvailable("BinningHorizontal");
    hasVertBinning = IsFeatureAvailable("BinningVertical");
    if (!hasHorzBinning || !hasVertBinning)
    {
        if (!hasHorzBinning)
            LogMessage((std::string) "Feature 'BinningHorizontal' is not supported");
        if (!hasVertBinning)
            LogMessage((std::string) "Feature 'BinningVertical' is not supported");
        return DEVICE_OK;
    }

    // note that the GenICam spec separates vertical and horizontal binning and does
    // not provide a single, unified binning property.
    LogMessage((std::string)"Set up binning properties");
    CPropertyAction* pAct = new CPropertyAction(this, &SaperaGigE::OnBinning);
    int ret = CreateProperty(MM::g_Keyword_Binning, "1", MM::Integer, false, pAct);
    if (DEVICE_OK != ret)
        return ret;

    INT64 bin, min, max, inc;
    std::vector<std::string> vValues, hValues, binValues;

    // vertical binning
    if (!AcqDevice_.SetFeatureValue("BinningVertical", 1))
    {
        LogMessage((std::string)"Failed to set 'BinningVertical'");
        return DEVICE_INVALID_PROPERTY;
    }
    AcqDevice_.GetFeatureValue("BinningVertical", &bin);
    AcqDevice_.GetFeatureInfo("BinningVertical", &AcqFeature_);
    AcqFeature_.GetMin(&min);
    AcqFeature_.GetMax(&max);
    AcqFeature_.GetInc(&inc);
    for (INT64 i = min; i <= max; i += inc)
        vValues.push_back(std::to_string(i));

    // horizontal binning
    if (!AcqDevice_.SetFeatureValue("BinningHorizontal", 1))
    {
        LogMessage((std::string)"Failed to set 'BinningHorizontal'");
        return DEVICE_INVALID_PROPERTY;
    }
    AcqDevice_.GetFeatureValue("BinningHorizontal", &bin);
    AcqDevice_.GetFeatureInfo("BinningHorizontal", &AcqFeature_);
    AcqFeature_.GetMin(&min);
    AcqFeature_.GetMax(&max);
    AcqFeature_.GetInc(&inc);
    for (INT64 i = min; i <= max; i += inc)
        hValues.push_back(std::to_string(i));

    // possible uniform binning values.
    if (vValues.empty() && hValues.empty())
        binValues.push_back("1");
    else if (vValues.empty())
        binValues = hValues;
    else if (hValues.empty())
        binValues = vValues;
    else {
        binValues.reserve(vValues.size() + hValues.size());
        std::set_union(vValues.begin(), vValues.end(),
            hValues.begin(), hValues.end(),
            std::back_inserter(binValues));
    }

    return SetAllowedValues(MM::g_Keyword_Binning, binValues);
}

/**
* Sets up "AcquisitionFrameRate" as a best-effort GenICam/SFNC property when
* the connected camera exposes it through Sapera. Absence of this feature is
* normal for some cameras or configurations, so failures here are logged and
* initialization continues without the property.
*/
int SaperaGigE::SetUpFrameRateProperty()
{
    std::lock_guard<std::recursive_mutex> saperaGuard(saperaMutex_);
    if (!IsFeatureAvailable("AcquisitionFrameRate"))
    {
        LogMessage("Feature 'AcquisitionFrameRate' is not supported");
        return DEVICE_OK;
    }

    double rate;
    if (!AcqDevice_.GetFeatureValue("AcquisitionFrameRate", &rate))
    {
        LogMessage("Failed to get feature value for 'AcquisitionFrameRate'; skipping property");
        return DEVICE_OK;
    }

    CPropertyAction* pAct = new CPropertyAction(this, &SaperaGigE::OnAcquisitionFrameRate);
    int ret = CreateProperty("AcquisitionFrameRate", CDeviceUtils::ConvertToString(rate),
        MM::Float, false, pAct);
    if (ret != DEVICE_OK)
        return ret;

    double low, high;
    AcqDevice_.GetFeatureInfo("AcquisitionFrameRate", &AcqFeature_);
    if (AcqFeature_.GetMin(&low) && AcqFeature_.GetMax(&high))
        SetPropertyLimits("AcquisitionFrameRate", low, high);

    return DEVICE_OK;
}

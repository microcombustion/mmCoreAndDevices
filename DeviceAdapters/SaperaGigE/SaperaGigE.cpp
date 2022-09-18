/////////////////////////////////////////////////////////
// FILE:		  SaperaGigE.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-------------------------------------------------------
// DESCRIPTION:   An adapter for Gigbit-Ethernet cameras using an
//                SDK from JAI, Inc.  Users and developers will
//                need to download and install the JAI SDK and control tool.
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

using namespace std;

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

    // ...supplied name not recognized
    // to heck with it, return a device anyway
    return new SaperaGigE();
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
* Setup default all variables and create device properties required to exist
* before intialization. In this case, no such properties were required. All
* properties will be created in the Initialize() method.
*
* As a general guideline Micro-Manager devices do not access hardware in the
* the constructor. We should do as little as possible in the constructor and
* perform most of the initialization in the Initialize() method.
*/
SaperaGigE::SaperaGigE() :
    bytesPerPixel_(1),
    bitsPerPixel_(8),
    initialized_(false),
    thd_(0),
    sequenceRunning_(false),
    Roi_(NULL)
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

    NumberOfWorkableCameras_ = 0;
}

int SaperaGigE::GetListOfAvailableCameras()

{
    CreateProperty(MM::g_Keyword_Name, g_CameraDeviceName, MM::String, true);

    // Sapera++ library stuff
    if (!(SapManager::DetectAllServers(SapManager::DetectServerAll)))
    {
        LogMessage("No CameraLink camera servers detected", false);
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
    else if (eAct == MM::BeforeGet) {
        // Empty path
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
    // create live video thread
    thd_ = new SequenceThread(this);

    LogMessage((std::string)"Initialize device '" + activeDevice_ + "'");
    SapLocation loc_(activeDevice_.c_str());
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

    NumberOfWorkableCameras_++;

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
    deviceFeatures["CameraVendor"] = define_feature("DeviceVendorName", true, NULL);
    deviceFeatures["CameraFamily"] = define_feature("DeviceFamilyName", true, NULL);
    deviceFeatures[MM::g_Keyword_CameraName] = define_feature("DeviceModelName", true, NULL);
    deviceFeatures["CameraVersion"] = define_feature("DeviceVersion", true, NULL);
    deviceFeatures["CameraInfo"] = define_feature("DeviceManufacturerInfo", true, NULL);
    deviceFeatures["CameraPartNumber"] = define_feature("deviceManufacturerPartNumber", true, NULL);
    deviceFeatures["CameraFirmwareVersion"] = define_feature("DeviceFirmwareVersion", true, NULL);
    deviceFeatures["CameraSerialNumber"] = define_feature("DeviceSerialNumber", true, NULL);
    deviceFeatures[MM::g_Keyword_CameraID] = define_feature("DeviceUserID", true, NULL);
    deviceFeatures["CameraMacAddress"] = define_feature("deviceMacAddress", true, NULL);
    deviceFeatures["SensorColorType"] = define_feature("sensorColorType", true, NULL);
    deviceFeatures["SensorPixelCoding"] = define_feature("PixelCoding", true, NULL);
    deviceFeatures["SensorBlackLevel"] = define_feature("BlackLevel", true, NULL);
    deviceFeatures["SensorPixelInput"] = define_feature("pixelSizeInput", true, NULL);
    deviceFeatures["SensorShutterMode"] = define_feature("SensorShutterMode", false, NULL);
    deviceFeatures["SensorBinningMode"] = define_feature("binningMode", false,
        new CPropertyAction(this, &SaperaGigE::OnBinningMode));
    deviceFeatures["SensorWidth"] = define_feature("SensorWidth", true, NULL);
    deviceFeatures["SensorHeight"] = define_feature("SensorHeight", true, NULL);
    deviceFeatures["ImagePixelSize"] = define_feature("PixelSize", true,
        new CPropertyAction(this, &SaperaGigE::OnPixelSize));
    deviceFeatures["ImageHorizontalOffset"] = define_feature("OffsetX", false,
        new CPropertyAction(this, &SaperaGigE::OnOffsetX));
    deviceFeatures["ImageVerticalOffset"] = define_feature("OffsetY", false,
        new CPropertyAction(this, &SaperaGigE::OnOffsetY));
    deviceFeatures["ImageWidth"] = define_feature("Width", false,
        new CPropertyAction(this, &SaperaGigE::OnWidth));
    deviceFeatures["ImageHeight"] = define_feature("Height", false,
        new CPropertyAction(this, &SaperaGigE::OnHeight));
    deviceFeatures["ImageTimeout"] = define_feature("ImageTimeout", false,
        new CPropertyAction(this, &SaperaGigE::OnImageTimeout));
    deviceFeatures["TurboTransferEnable"] = define_feature("turboTransferEnable", true, NULL);
    deviceFeatures["SensorTemperature"] = define_feature("DeviceTemperature", true,
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
        char value[MM::MaxStrLength];
        if (!AcqDevice_.GetFeatureValue(f.name, value, sizeof(value)))
            return DEVICE_ERR;

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

        if (f.action == NULL)
            ret = CreateProperty(x->first, value, eType, f.readOnly);
        else
            ret = CreateProperty(x->first, value, eType, f.readOnly, f.action);
        assert(ret == DEVICE_OK);

        if (sapType == SapFeature::TypeEnum)
        {
            vector<string> allowed;
            int count;
            AcqFeature_.GetEnumCount(&count);
            for (int i = 0; i < count; i++)
            {
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
        return ret;

    // set up Sapera / Micro-Manager buffers
    LogMessage((std::string) "Setting up buffers");
    ret = SynchronizeBuffers();
    if (ret != DEVICE_OK)
        return ret;

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
        return ret;

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
    Xfer_->Freeze();
    if (!Xfer_->Wait(5000))
        return DEVICE_NATIVE_MODULE_FAILED;
    int ret;
    ret = FreeHandles();
    if (ret != DEVICE_OK)
        return ret;
    return DEVICE_OK;
}

/**
* Frees Sapera buffers and such
*/
int SaperaGigE::FreeHandles()
{
    LogMessage((std::string)"Destroy Sapera buffers and devices");
    if (Xfer_ && *Xfer_ && !Xfer_->Destroy()) return DEVICE_ERR;
    if (!Buffers_.Destroy()) return DEVICE_ERR;
    if (!AcqFeature_.Destroy()) return DEVICE_ERR;
    if (!AcqDevice_.Destroy()) return DEVICE_ERR;
    return DEVICE_OK;
}

/**
* Performs exposure and grabs a single image.
* This function blocks during the actual exposure and returns immediately afterwards
* Required by the MM::Camera API.
*/
int SaperaGigE::SnapImage()
{
    // This will always be false, as no sequences will ever run
    if (sequenceRunning_)
        return DEVICE_CAMERA_BUSY_ACQUIRING;
    // Start image capture
    Xfer_->SetCommandTimeout(1000);
    if (!Xfer_->Snap(1))
    {
        LogMessage("Failure occurred while capturing a single image");
        return DEVICE_ERR;
    }
    // Wait for either the capture to finish or 16 seconds, whichever is first
    if (!Xfer_->Wait(16000))
    {
        return DEVICE_ERR;
    }
    return DEVICE_OK;
}



/**
* Returns pixel data.
* Required by the MM::Camera API.
* The calling program will assume the size of the buffer based on the values
* obtained from GetImageBufferSize(), which in turn should be consistent with
* values returned by GetImageWidth(), GetImageHeight() and GetImageBytesPerPixel().
* The calling program allso assumes that camera never changes the size of
* the pixel buffer on its own. In other words, the buffer can change only if
* appropriate properties are set (such as binning, pixel type, etc.)
*/
const unsigned char* SaperaGigE::GetImageBuffer()
{
    // Put Sapera buffer into Micro-Manager Buffer
    Buffers_.ReadRect(Roi_->GetXMin(), Roi_->GetYMin(), img_.Width(), img_.Height(),
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
* This demo implementation ignores the position coordinates and just crops the buffer.
* @param x - top-left corner coordinate
* @param y - top-left corner coordinate
* @param xSize - width
* @param ySize - height
*/
int SaperaGigE::SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize)
{
    LogMessage((std::string)"Setting Region of Interest");
    if (xSize == 0 && ySize == 0)
        return ClearROI();
    else
    {
        // apply ROI
        Roi_->SetRoi(x, y, xSize, ySize);
        img_.Resize(xSize, ySize);
    }
    return DEVICE_OK;
}

/**
* Returns the actual dimensions of the current ROI
* Required by the MM::Camera API.
*/
int SaperaGigE::GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize)
{
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
    Roi_->ResetRoi();
    ResizeImageBuffer();
    return DEVICE_OK;
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

//i/**
// * Required by the MM::Camera API
// * Please implement this yourself and do not rely on the base class implementation
// * The Base class implementation is deprecated and will be removed shortly
// */
////int SaperaGigE::StartSequenceAcquisition(double interval_ms)
//int SaperaGigE::StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow);
//{
//    //@TODO: Implement Sequence Acquisition
//    return DEVICE_ERR;
//    //int ret = StartSequenceAcquisition((long)(interval_ms/exposureMs_), interval_ms, true);
//    //return ret;
//}

/**
* Stop and wait for the Sequence thread finished
*/
int SaperaGigE::StopSequenceAcquisition()
{
    //@TODO: Implement Sequence Acquisition
    return DEVICE_NOT_YET_IMPLEMENTED;
    /*thd_->Stop();
    thd_->wait();
    sequenceRunning_ = false;
    return DEVICE_OK;*/
}

/**
* Simple implementation of Sequence Acquisition
* A sequence acquisition should run on its own thread and transport new images
* coming of the camera into the MMCore circular buffer.
*/
int SaperaGigE::StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow)
{
    //@TODO: Implement Sequence Acquisition
    return DEVICE_NOT_YET_IMPLEMENTED;
    /*if (sequenceRunning_)
    {
        return DEVICE_CAMERA_BUSY_ACQUIRING;
    }
    int ret = GetCoreCallback()->PrepareForAcq(this);
    if (ret != DEVICE_OK)
    {
        return ret;
    }
    sequenceRunning_ = true;
    thd_->SetLength(10);
    thd_->Start();
   return DEVICE_OK; */
}

/*
 * Inserts Image and MetaData into MMCore circular Buffer
 */
int SaperaGigE::InsertImage()
{
    //@TODO: Implement Sequence Acquisition
    return GetCoreCallback()->InsertImage(this, const_cast<unsigned char*>(img_.GetPixels()), GetImageWidth(), GetImageHeight(), GetImageBytesPerPixel());
}

bool SaperaGigE::IsCapturing() {
    //@TODO: Implement Sequence Acquisition
    return sequenceRunning_;
}

///////////////////////////////////////////////////////////////////////////////
// SaperaGigE Action handlers
///////////////////////////////////////////////////////////////////////////////

/**
* Handles "Binning" property.
*/
int SaperaGigE::OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        long binSize;
        pProp->Get(binSize);
        if (!AcqDevice_.SetFeatureValue("BinningVertical", int(binSize)))
            return DEVICE_ERR;
        if (!AcqDevice_.SetFeatureValue("BinningHorizontal", int(binSize)))
            return DEVICE_ERR;
        return SynchronizeBuffers();
    }
    // MM::BeforeGet returns the value cached in the property.
    return DEVICE_OK;
}

int SaperaGigE::OnBinningMode(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        std::string value;
        pProp->Get(value);
        AcqDevice_.SetFeatureValue("binningMode", value.c_str());
    }
    // MM::BeforeGet returns the value cached in the property.
    return DEVICE_OK;
}

int SaperaGigE::OnPixelSize(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        return DEVICE_CAN_NOT_SET_PROPERTY;
    }
    else if (eAct == MM::BeforeGet)
    {
        UINT32 value;
        if (!AcqDevice_.GetFeatureValue("PixelSize", &value))
            return DEVICE_ERR;
        pProp->Set((long)value);
    }
    return DEVICE_OK;
}

long SaperaGigE::CheckValue(const char* key, long value)
{
    INT64 min, max, inc;
    AcqDevice_.GetFeatureInfo(key, &AcqFeature_);
    AcqFeature_.GetInc(&inc);
    AcqFeature_.GetMin(&min);
    AcqFeature_.GetMax(&max);

    long out = (value / (long)inc) * (long)inc;
    out = max((long)min, min((long)max, out));

    if (value != out)
        LogMessage((std::string)"Encountered invalid value for '" + key
            + "': corrected " + std::to_string((INT64)value) + " to " + std::to_string((INT64)out));
    return out;
}

int SaperaGigE::OnOffsetX(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        long value;
        pProp->Get(value);

        value = CheckValue("OffsetX", value);
        if (!AcqDevice_.SetFeatureValue("OffsetX", value))
            return DEVICE_ERR;
    }
    else if (eAct == MM::BeforeGet)
    {
        UINT32 value;
        if (!AcqDevice_.GetFeatureValue("OffsetX", &value))
            return DEVICE_ERR;
        pProp->Set((long)value);
    }
    return DEVICE_OK;
}

int SaperaGigE::OnOffsetY(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        long value;
        pProp->Get(value);

        value = CheckValue("OffsetY", value);
        if (!AcqDevice_.SetFeatureValue("OffsetY", value))
            return DEVICE_ERR;
    }
    else if (eAct == MM::BeforeGet)
    {
        UINT32 value;
        if (!AcqDevice_.GetFeatureValue("OffsetY", &value))
            return DEVICE_ERR;
        pProp->Set((long)value);
    }
    return DEVICE_OK;
}

int SaperaGigE::OnWidth(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        long value;
        pProp->Get(value);

        value = CheckValue("Width", value);
        int ret = SynchronizeBuffers("", value, -1);
        if (ret != DEVICE_OK)
            return ret;
    }
    else if (eAct == MM::BeforeGet)
    {
        UINT32 value;
        if (!AcqDevice_.GetFeatureValue("Width", &value))
            return DEVICE_ERR;
        pProp->Set((long)value);
    }
    return DEVICE_OK;
}

int SaperaGigE::OnHeight(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        long value;
        pProp->Get(value);

        value = CheckValue("Height", value);
        int ret = SynchronizeBuffers("", -1, value);
        if (ret != DEVICE_OK)
            return ret;
    }
    else if (eAct == MM::BeforeGet)
    {
        UINT32 value;
        if (!AcqDevice_.GetFeatureValue("Height", &value))
            return DEVICE_ERR;
        pProp->Set((long)value);
    }
    return DEVICE_OK;
}

int SaperaGigE::OnImageTimeout(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        double value;
        pProp->Get(value);
        int ret = SynchronizeBuffers("", -1, -1, value);
        if (ret != DEVICE_OK)
            return ret;
    }
    else if (eAct == MM::BeforeGet)
    {
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
        double value;
        if (!AcqDevice_.GetFeatureValue("DeviceTemperature", &value))
        {
            LogMessage("Failed to get feature value for 'DeviceTemperature'");
            return DEVICE_ERR;
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
    char pixelFormat[10];
    AcqDevice_.GetFeatureValue("PixelFormat", pixelFormat, sizeof(pixelFormat));
    if (eAct == MM::AfterSet)
    {
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
    double gain = 1.;
    if (eAct == MM::AfterSet)
    {
        pProp->Get(gain);
        if (!AcqDevice_.SetFeatureValue("Gain", gain))
        {
            LogMessage("Failed to set feature value for 'Gain'");
            return DEVICE_ERR;
        }
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!AcqDevice_.GetFeatureValue("Gain", &gain))
        {
            LogMessage("Failed to get feature value for 'Gain'");
            return DEVICE_ERR;
        }
        pProp->Set(gain);
    }

    return DEVICE_OK;
}

int SaperaGigE::OnExposure(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    // note that GigE units of exposure are us; umanager uses ms
    double exposure;
    if (eAct == MM::AfterSet)
    {
        pProp->Get(exposure);  // ms
        if (!AcqDevice_.SetFeatureValue("ExposureTime", exposure * 1000.0)) // ms to us
        {
            LogMessage("Failed to set feature value for 'ExposureTime'");
            return DEVICE_ERR;
        }
    }
    else if (eAct == MM::BeforeGet)
    {
        if (!AcqDevice_.GetFeatureValue("ExposureTime", &exposure)) // us
        {
            LogMessage("Failed to get feature value for 'ExposureTime'");
            return DEVICE_ERR;
        }
        pProp->Set(exposure / 1000.0);
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
    UINT32 width, height;
    if (!AcqDevice_.GetFeatureValue("Height", &height))
        return DEVICE_INVALID_PROPERTY;
    if (!AcqDevice_.GetFeatureValue("Width", &width))
        return DEVICE_INVALID_PROPERTY;

    img_.Resize(width, height, bytesPerPixel_);

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
    memset(pBuf, (int)(step * max(exposureMs, maxExp)), GetImageBufferSize());
}

/*
 * Reformat Sapera Buffer Object
 */
int SaperaGigE::SynchronizeBuffers(std::string pixelFormat, int width, int height, double timeout)
{
    // destroy transfer and buffer
    if (Roi_ != NULL)
    {
        delete Roi_;
        Xfer_->Destroy();
        Buffers_.Destroy();
    }

    // default value
    if (pixelFormat.size())
        AcqDevice_.SetFeatureValue("PixelFormat", pixelFormat.c_str());
    if (width > 0)
        AcqDevice_.SetFeatureValue("Width", width);
    if (height > 0)
        AcqDevice_.SetFeatureValue("Height", height);
    if (timeout > 0)
        AcqDevice_.SetFeatureValue("ImageTimeout", timeout);

    // synchronize bit depth with camera
    AcqDevice_.GetFeatureValue("PixelSize", &bitsPerPixel_);
    bytesPerPixel_ = (bitsPerPixel_ + 7) / 8;

    // re-create  transfer and buffer
    Buffers_ = SapBufferWithTrash(3, &AcqDevice_);
    Roi_ = new SapBufferRoi(&Buffers_);
    AcqDeviceToBuf_ = SapAcqDeviceToBuf(&AcqDevice_, &Buffers_, XferCallback, this);
    Xfer_ = &AcqDeviceToBuf_;
    if (!Buffers_.Create())
    {
        int ret = FreeHandles();
        if (ret != DEVICE_OK)
            return ret;
        return DEVICE_NATIVE_MODULE_FAILED;
    }
    if (Xfer_ && !Xfer_->Create())
    {
        int ret = FreeHandles();
        if (ret != DEVICE_OK)
            return ret;
        return DEVICE_NATIVE_MODULE_FAILED;
    }
    ResizeImageBuffer();

    return DEVICE_OK;
}

void SaperaGigE::XferCallback(SapXferCallbackInfo* pInfo)
{
    // If grabbing in trash buffer, log a message
    if (pInfo->IsTrash())
    {
        ErrorBox((std::string)"Frames acquired in trash buffer: "
            + std::to_string((INT64)pInfo->GetEventCount()), "Xfer");
    }
}


int SaperaGigE::SetUpBinningProperties()
{
    BOOL hasHorzBinning;
    BOOL hasVertBinning;
    AcqDevice_.IsFeatureAvailable("BinningHorizontal", &hasHorzBinning);
    AcqDevice_.IsFeatureAvailable("BinningVertical", &hasVertBinning);
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

///////////////////////////////////////////////////////////////////////////////
// Threading methods
///////////////////////////////////////////////////////////////////////////////

int SequenceThread::svc()
{
    //SapManager::DisplayMessage("SequenceThread Start");
    long count(0);
    while (!stop_)//&& count < numImages_)
    {
        /*int ret = camera_->SnapImage();
        if (ret != DEVICE_OK)
        {
            //SapManager::DisplayMessage("SequenceThread Snap failed");
           camera_->StopSequenceAcquisition();
           return 1;
        }*/

        int ret = camera_->InsertImage();
        if (ret != DEVICE_OK)
        {
            //SapManager::DisplayMessage("SequenceThread InsertFailed");
            camera_->StopSequenceAcquisition();
            return 1;
        }
        //count++;
    }
    //SapManager::DisplayMessage("SequenceThread End");
    return 0;
}

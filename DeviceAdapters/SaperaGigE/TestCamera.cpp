/////////////////////////////////////////////////////////
// FILE:		TestCamera.cpp
// PROJECT:		Teledyne DALSA Micro-Manager Glue Library
//-------------------------------------------------------
// AUTHOR: Robert Frazee, rfraze1@lsu.edu

#include "TestCamera.h"
#include "../MMDevice/ModuleInterface.h"
#include "stdio.h"
#include "conio.h"
#include "math.h"
#include "sapclassbasic.h"
#include <string>

using namespace std;

const char* g_CameraName = "GigE Nano";
const char* g_PixelType_8bit = "8bit";
const char* g_PixelType_10bit = "10bit";
const char* g_PixelType_12bit = "12bit";

const char* g_CameraModelProperty = "Model";
const char* g_CameraModel_A = "Nano-M1930-NIR";

// g_CameraAcqDeviceNumberProperty
// g_CameraServerNameProperty
// g_CameraConfigFilenameProperty
const char* g_CameraAcqDeviceNumberProperty = "Acquisition Device Number";
const char* g_CameraAcqDeviceNumber_Def = "0";
const char* g_CameraServerNameProperty = "Server Name";
const char* g_CameraServerName_Def = "Nano-M1930-NIR_1";
const char* g_CameraConfigFilenameProperty = "Config Filename";
const char* g_CameraConfigFilename_Def = "NoFile";


///////////////////////////////////////////////////////////////////////////////
// Exported MMDevice API
///////////////////////////////////////////////////////////////////////////////

/**
 * List all supported hardware devices here
 */
MODULE_API void InitializeModuleData()
{
   RegisterDevice(g_CameraName, MM::CameraDevice, "GigE Nano Camera Device");
}

MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
   if (deviceName == 0)
      return 0;

   // decide which device class to create based on the deviceName parameter
   if (strcmp(deviceName, g_CameraName) == 0)
   {
      // create camera
      return new TestCamera();
   }

   // ...supplied name not recognized
   // to heck with it, return a device anyway
   return new TestCamera();
}

MODULE_API void DeleteDevice(MM::Device* pDevice)
{
   delete pDevice;
}

///////////////////////////////////////////////////////////////////////////////
// TestCamera implementation
// ~~~~~~~~~~~~~~~~~~~~~~~

/**
* TestCamera constructor.
* Setup default all variables and create device properties required to exist
* before intialization. In this case, no such properties were required. All
* properties will be created in the Initialize() method.
*
* As a general guideline Micro-Manager devices do not access hardware in the
* the constructor. We should do as little as possible in the constructor and
* perform most of the initialization in the Initialize() method.
*/
TestCamera::TestCamera() :
   binning_ (1),
   gain_(1),
   bytesPerPixel_(1),
   bitsPerPixel_(8),
   initialized_(false),
   roiX_(0),
   roiY_(0),
   thd_(0),
   sequenceRunning_(false),
   SapFormatBytes_(1)
{
   // call the base class method to set-up default error codes/messages
   InitializeDefaultErrorMessages();

   // Description property
   int ret = CreateProperty(MM::g_Keyword_Description, "GigE Nano Camera Adapter", MM::String, true);
   assert(ret == DEVICE_OK);

   // camera type pre-initialization property
   ret = CreateProperty(g_CameraModelProperty, g_CameraModel_A, MM::String, false, 0, true);
   assert(ret == DEVICE_OK);

   vector<string> modelValues;
   modelValues.push_back(g_CameraModel_A);
   modelValues.push_back(g_CameraModel_A); 

   ret = SetAllowedValues(g_CameraModelProperty, modelValues);
   assert(ret == DEVICE_OK);

   // Sapera++ library stuff

   int serverCount = SapManager::GetServerCount();
   if(serverCount == 0)
   {
	   ErrorBox((LPCWSTR)L"Initialization Error", (LPCWSTR)L"No servers!");
   }

   acqServerName_ = new char[CORSERVER_MAX_STRLEN];
   configFilename_ = new char[MAX_PATH];

   ret = CreateProperty(g_CameraAcqDeviceNumberProperty, g_CameraAcqDeviceNumber_Def, MM::Integer, false, 0, true);
   assert(ret == DEVICE_OK);

   ret = CreateProperty(g_CameraServerNameProperty, g_CameraServerName_Def, MM::String, false, 0, true);
   assert(ret == DEVICE_OK);

   ret = CreateProperty(g_CameraConfigFilenameProperty, g_CameraConfigFilename_Def, MM::String, false, 0, true);
   assert(ret == DEVICE_OK);

   // create live video thread
   thd_ = new SequenceThread(this);
}

/**
* TestCamera destructor.
* If this device used as intended within the Micro-Manager system,
* Shutdown() will be always called before the destructor. But in any case
* we need to make sure that all resources are properly released even if
* Shutdown() was not called.
*/
TestCamera::~TestCamera()
{
   if (initialized_)
      Shutdown();
}

/**
* Obtains device name.
* Required by the MM::Device API.
*/
void TestCamera::GetName(char* name) const
{
   // We just return the name we use for referring to this
   // device adapter.
   CDeviceUtils::CopyLimitedString(name, g_CameraName);
}

/**
* Intializes the hardware.
* Typically we access and initialize hardware at this point.
* Device properties are typically created here as well.
* Required by the MM::Device API.
*/
int TestCamera::Initialize()
{
   if (initialized_)
      return DEVICE_OK;

   //SapManager::DisplayMessage("This plugin logs debug messages.  Press no so they don't all pop up like this.");
   // set property list
   // -----------------

   // binning
   CPropertyAction *pAct = new CPropertyAction (this, &TestCamera::OnBinning);
   //@TODO: Check what the actual binning value is and set that
   // For now, set binning to 1 for MM and set that on the camera later
   int ret = CreateProperty(MM::g_Keyword_Binning, "1", MM::Integer, false, pAct);
   assert(ret == DEVICE_OK);

   vector<string> binningValues;
   binningValues.push_back("1");
   binningValues.push_back("2"); 
   binningValues.push_back("4");

   ret = SetAllowedValues(MM::g_Keyword_Binning, binningValues);
   assert(ret == DEVICE_OK);

   //Sapera stuff
   // g_CameraAcqDeviceNumberProperty
   // g_CameraServerNameProperty
   // g_CameraConfigFilenameProperty
   long tmpDeviceNumber;
   if(GetProperty(g_CameraAcqDeviceNumberProperty, tmpDeviceNumber) != DEVICE_OK)
   {
	   //SapManager::DisplayMessage("Failed to retrieve AcqDeviceNumberProperty");
	   return DEVICE_ERR;
   }
   acqDeviceNumber_ = (UINT32)tmpDeviceNumber;

   if(false)//GetProperty(g_CameraServerNameProperty, acqServerName_) != DEVICE_OK)
   {
	   //SapManager::DisplayMessage("Failed to retrieve ServerNameProperty");
	   return DEVICE_ERR;
   }
   acqServerName_ = (char *)g_CameraServerName_Def;

   if(false)//GetProperty(g_CameraConfigFilenameProperty, configFilename_) != DEVICE_OK)
   {
	   //SapManager::DisplayMessage("Failed to retrieve ConfigFilenameProperty");
	   return DEVICE_ERR;
   }
   configFilename_ = "NoFile";

   //SapManager::DisplayMessage("(Sapera app)Creating loc_ object");
   SapLocation loc_(acqServerName_, acqDeviceNumber_);
   //SapManager::DisplayMessage("(Sapera app)Created loc_ object");
   //SapManager::DisplayMessage("(Sapera app)GetResourceCount for ResourceAcqDevice starting"); 
   if(SapManager::GetResourceCount(acqServerName_, SapManager::ResourceAcqDevice) > 0)
   {
	   //SapManager::DisplayMessage("(Sapera app)GetResourceCount for ResourceAcqDevice found something");
	   if(strcmp(configFilename_, "NoFile") == 0)
		   AcqDevice_ = SapAcqDevice(loc_, false);
	   else
		   AcqDevice_ = SapAcqDevice(loc_, configFilename_);

	   Buffers_ = SapBufferWithTrash(2, &AcqDevice_);
	   AcqDeviceToBuf_ = SapAcqDeviceToBuf(&AcqDevice_, &Buffers_);
	   Xfer_ = &AcqDeviceToBuf_;

	   if(!AcqDevice_.Create())
	   {
		   ret = FreeHandles();
		   if (ret != DEVICE_OK)
		   {
			   //SapManager::DisplayMessage("Failed to FreeHandles during Acq_.Create() for ResourceAcqDevice");
			   return ret;
		   }
		   //SapManager::DisplayMessage("Failed to create Acq_ for ResourceAcqDevice");
		   return DEVICE_INVALID_INPUT_PARAM;
	   }
   }
   //SapManager::DisplayMessage("(Sapera app)GetResourceCount for ResourceAcqDevice done");
   //SapManager::DisplayMessage("(Sapera app)Creating Buffers_");
   if(!Buffers_.Create())
	{
		ret = FreeHandles();
		if (ret != DEVICE_OK)
			return ret;
		return DEVICE_NATIVE_MODULE_FAILED;
	}
   //SapManager::DisplayMessage("(Sapera app)Creating Xfer_");
   if(Xfer_ && !Xfer_->Create())
	{
		//SapManager::DisplayMessage("Xfer_ creation failed");
		ret = FreeHandles();
		if (ret != DEVICE_OK)
			return ret;
		return DEVICE_NATIVE_MODULE_FAILED;
	}
	//SapManager::DisplayMessage("(Sapera app)Starting Xfer");
	//Start continuous grab
	//Xfer_->Grab();
	//SapManager::DisplayMessage("(Sapera app)Sapera Initialization for TestCamera complete");

	if(!AcqDevice_.GetFeatureValue("ExposureTime", &exposureMs_))
		return DEVICE_ERR;
	exposureMs_ = exposureMs_ / 1000;

	// synchronize bit depth with camera

	char acqFormat[10];
	AcqDevice_.GetFeatureValue("PixelFormat", acqFormat, 10);
	if(strcmp(acqFormat, "Mono8") == 0)
	{
		// Setup Micro-Manager for 8bit pixels
		SapFormatBytes_ = 1;
		bitsPerPixel_ = 8;
		bytesPerPixel_ = 1;
		//resize the SapBuffer
		int ret = SapBufferReformat(SapFormatMono8, "Mono8");
		if(ret != DEVICE_OK)
		{
			return ret;
		}
		ResizeImageBuffer();
		pAct = new CPropertyAction (this, &TestCamera::OnPixelType);
		ret = CreateProperty(MM::g_Keyword_PixelType, g_PixelType_8bit, MM::String, false, pAct);
		assert(ret == DEVICE_OK);
	}
	if(strcmp(acqFormat, "Mono10") == 0)
	{
		// Setup Micro-Manager for 8bit pixels
		SapFormatBytes_ = 2;
		bitsPerPixel_ = 10;
		bytesPerPixel_ = 2;
		//resize the SapBuffer
		int ret = SapBufferReformat(SapFormatMono10, "Mono10");
		if(ret != DEVICE_OK)
		{
			return ret;
		}
		ResizeImageBuffer();
		pAct = new CPropertyAction (this, &TestCamera::OnPixelType);
		ret = CreateProperty(MM::g_Keyword_PixelType, g_PixelType_10bit, MM::String, false, pAct);
		assert(ret == DEVICE_OK);
	}


	// pixel type
   

   vector<string> pixelTypeValues;
   pixelTypeValues.push_back(g_PixelType_8bit);
   pixelTypeValues.push_back(g_PixelType_10bit);

   
   ret = SetAllowedValues(MM::g_Keyword_PixelType, pixelTypeValues);
   assert(ret == DEVICE_OK);

   // Set Binning to 1
   if(!AcqDevice_.SetFeatureValue("BinningVertical", 1))
	   return DEVICE_ERR;
   if(!AcqDevice_.SetFeatureValue("BinningHorizontal", 1))
	   return DEVICE_ERR;

  
   // Setup gain
   pAct = new CPropertyAction(this, &TestCamera::OnGain);
   ret = CreateProperty(MM::g_Keyword_Gain, "1.0", MM::Float, false, pAct);
   assert(ret == DEVICE_OK);
   if(!AcqDevice_.SetFeatureValue("Gain", 1.0))
	   return DEVICE_ERR;
   SapFeature SapGain_(loc_);
   if(!SapGain_.Create())
	   return DEVICE_ERR;
   AcqDevice_.GetFeatureInfo("Gain", &SapGain_);
   double g_low = 0.0;
   double g_high = 0.0;
   SapGain_.GetMax(&g_high);
   SapGain_.GetMin(&g_low);
   SetPropertyLimits(MM::g_Keyword_Gain, g_low, g_high);



   // synchronize all properties
   // --------------------------
   ret = UpdateStatus();
   if (ret != DEVICE_OK)
      return ret;

   // setup the buffer
   // ----------------
   ret = ResizeImageBuffer();
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
int TestCamera::Shutdown()
{
	if(!initialized_)
		return DEVICE_OK;
	initialized_ = false;
	Xfer_->Freeze();
	if(!Xfer_->Wait(5000))
		return DEVICE_NATIVE_MODULE_FAILED;
	int ret;
	ret = FreeHandles();
	if(ret != DEVICE_OK)
		return ret;
	return DEVICE_OK;
}

/**
* Frees Sapera buffers and such
*/
int TestCamera::FreeHandles()
{
	if(Xfer_ && *Xfer_ && !Xfer_->Destroy()) return DEVICE_ERR;
	if(!Buffers_.Destroy()) return DEVICE_ERR;
	if(!Acq_.Destroy()) return DEVICE_ERR;
	if(!AcqDevice_.Destroy()) return DEVICE_ERR;
	return DEVICE_OK;
}

int TestCamera::ErrorBox(LPCWSTR text, LPCWSTR caption)
{
	return MessageBox(NULL, caption, text, (MB_ICONERROR | MB_OK));
}

/**
* Performs exposure and grabs a single image.
* This function blocks during the actual exposure and returns immediately afterwards 
* Required by the MM::Camera API.
*/
int TestCamera::SnapImage()
{
	// This will always be false, as no sequences will ever run
	if(sequenceRunning_)
		return DEVICE_CAMERA_BUSY_ACQUIRING;
	// Start image capture
	if(!Xfer_->Snap(1))
	{
		return DEVICE_ERR;
	}
	// Wait for either the capture to finish or 2.5 seconds, whichever is first
	if(!Xfer_->Wait(2500))
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
* values returned by GetImageWidth(), GetImageHight() and GetImageBytesPerPixel().
* The calling program allso assumes that camera never changes the size of
* the pixel buffer on its own. In other words, the buffer can change only if
* appropriate properties are set (such as binning, pixel type, etc.)
*/
const unsigned char* TestCamera::GetImageBuffer()
{
	// Put Sapera buffer into Micro-Manager Buffer
	Buffers_.ReadRect(roiX_, roiY_, img_.Width(), img_.Height(), const_cast<unsigned char*>(img_.GetPixels()));
	// Return location of the Micro-Manager Buffer
	return const_cast<unsigned char*>(img_.GetPixels());
}

/**
* Returns image buffer X-size in pixels.
* Required by the MM::Camera API.
*/
unsigned TestCamera::GetImageWidth() const
{
   return img_.Width();
}

/**
* Returns image buffer Y-size in pixels.
* Required by the MM::Camera API.
*/
unsigned TestCamera::GetImageHeight() const
{
   return img_.Height();
}

/**
* Returns image buffer pixel depth in bytes.
* Required by the MM::Camera API.
*/
unsigned TestCamera::GetImageBytesPerPixel() const
{
   return img_.Depth();
} 

/**
* Returns the bit depth (dynamic range) of the pixel.
* This does not affect the buffer size, it just gives the client application
* a guideline on how to interpret pixel values.
* Required by the MM::Camera API.
*/
unsigned TestCamera::GetBitDepth() const
{
   return bitsPerPixel_;
}

/**
* Returns the size in bytes of the image buffer.
* Required by the MM::Camera API.
*/
long TestCamera::GetImageBufferSize() const
{
   return img_.Width() * img_.Height() * GetImageBytesPerPixel();
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
int TestCamera::SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize)
{
   if (xSize == 0 && ySize == 0)
   {
      // effectively clear ROI
      ResizeImageBuffer();
      roiX_ = 0;
      roiY_ = 0;
   }
   else
   {
      // apply ROI
      img_.Resize(xSize, ySize);
      roiX_ = x;
      roiY_ = y;
   }
   return DEVICE_OK;
}

/**
* Returns the actual dimensions of the current ROI.
* Required by the MM::Camera API.
*/
int TestCamera::GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize)
{
   x = roiX_;
   y = roiY_;

   xSize = img_.Width();
   ySize = img_.Height();

   return DEVICE_OK;
}

/**
* Resets the Region of Interest to full frame.
* Required by the MM::Camera API.
*/
int TestCamera::ClearROI()
{
   ResizeImageBuffer();
   roiX_ = 0;
   roiY_ = 0;
      
   return DEVICE_OK;
}

/**
* Returns the current exposure setting in milliseconds.
* Required by the MM::Camera API.
*/
double TestCamera::GetExposure() const
{
   return exposureMs_;
}

/**
* Sets exposure in milliseconds.
* Required by the MM::Camera API.
*/
void TestCamera::SetExposure(double exp)
{
   exposureMs_ = exp;
   // Micromanager deals with exposure time in ms
   // Sapera deals with exposure time in us
   // As such, we convert between the two
   AcqDevice_.SetFeatureValue("ExposureTime", (exposureMs_ * 1000));
}

/**
* Returns the current binning factor.
* Required by the MM::Camera API.
*/
int TestCamera::GetBinning() const
{
   return binning_;
}

/**
* Sets binning factor.
* Required by the MM::Camera API.
*/
int TestCamera::SetBinning(int binF)
{
   return SetProperty(MM::g_Keyword_Binning, CDeviceUtils::ConvertToString(binF));
}

int TestCamera::PrepareSequenceAcqusition()
{
	return DEVICE_ERR;
}


/**
 * Required by the MM::Camera API
 * Please implement this yourself and do not rely on the base class implementation
 * The Base class implementation is deprecated and will be removed shortly
 */
int TestCamera::StartSequenceAcquisition(double interval_ms)
{
	//@TODO: Implement Sequence Acquisition
	return DEVICE_ERR;
	//int ret = StartSequenceAcquisition((long)(interval_ms/exposureMs_), interval_ms, true);
	//return ret;
}

/**                                                                       
* Stop and wait for the Sequence thread finished                                   
*/                                                                        
int TestCamera::StopSequenceAcquisition()                                     
{
	//@TODO: Implement Sequence Acquisition
	return DEVICE_ERR;
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
int TestCamera::StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow)
{
	//@TODO: Implement Sequence Acquisition
	return DEVICE_ERR;
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
int TestCamera::InsertImage()
{
	//@TODO: Implement Sequence Acquisition
	return GetCoreCallback()->InsertImage(this, const_cast<unsigned char*>(img_.GetPixels()), GetImageWidth(), GetImageHeight(), GetImageBytesPerPixel());
}


bool TestCamera::IsCapturing() {
	//@TODO: Implement Sequence Acquisition
   return sequenceRunning_;
}


///////////////////////////////////////////////////////////////////////////////
// TestCamera Action handlers
///////////////////////////////////////////////////////////////////////////////

/**
* Handles "Binning" property.
*/
int TestCamera::OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::AfterSet)
   {
      long binSize;
      pProp->Get(binSize);
      binning_ = (int)binSize;
	  if(!AcqDevice_.SetFeatureValue("BinningVertical", binning_))
		  return DEVICE_ERR;
	  if(!AcqDevice_.SetFeatureValue("BinningHorizontal", binning_))
		  return DEVICE_ERR;
      return ResizeImageBuffer();
   }
   else if (eAct == MM::BeforeGet)
   {
      pProp->Set((long)binning_);
   }

   return DEVICE_OK;
}

/**
* Handles "PixelType" property.
*/
int TestCamera::OnPixelType(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	//bytesPerPixel_ = 1;
	//ResizeImageBuffer();
	//return DEVICE_OK;
   if (eAct == MM::AfterSet)
   {
	   //SapManager::DisplayMessage("(Sapera app)OnPixelType MM:AfterSet");
      string val;
      pProp->Get(val);
      if (val.compare(g_PixelType_8bit) == 0)
	  {
		  if(SapFormatBytes_ != 1)
		  {
			  SapFormatBytes_ = 1;
			  bitsPerPixel_ = 8;
			  //resize the SapBuffer
			  int ret = SapBufferReformat(SapFormatMono8, "Mono8");
			  if(ret != DEVICE_OK)
			  {
				  return ret;
			  }
		  }
         bytesPerPixel_ = 1;
	  }
      else if (val.compare(g_PixelType_10bit) == 0)
	  {
		  if(SapFormatBytes_ != 2)
		  {
			  SapFormatBytes_ = 2;
			  bitsPerPixel_ = 10;
			  //resize the SapBuffer
			  int ret = SapBufferReformat(SapFormatMono16, "Mono10");
			  if(ret != DEVICE_OK)
			  {
				  return ret;
			  }
		  }
         bytesPerPixel_ = 2;
	  }
      else
         assert(false);

      ResizeImageBuffer();
   }
   else if (eAct == MM::BeforeGet)
   {
      if (bytesPerPixel_ == 1)
         pProp->Set(g_PixelType_8bit);
      else if (bytesPerPixel_ == 2)
         pProp->Set(g_PixelType_10bit);
      else
         assert(false); // this should never happen
   }

   return DEVICE_OK;
}

/**
* Handles "Gain" property.
*/
int TestCamera::OnGain(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::AfterSet)
   {
	   //SapManager::DisplayMessage("(Sapera app)OnGain MM:AfterSet");
      pProp->Get(gain_);
	  AcqDevice_.SetFeatureValue("Gain", gain_);
   }
   else if (eAct == MM::BeforeGet)
   {
      pProp->Set(gain_);
   }

   return DEVICE_OK;
}


///////////////////////////////////////////////////////////////////////////////
// Private TestCamera methods
///////////////////////////////////////////////////////////////////////////////

/**
* Sync internal image buffer size to the chosen property values.
*/
int TestCamera::ResizeImageBuffer()
{
   img_.Resize(IMAGE_WIDTH/binning_, IMAGE_HEIGHT/binning_, bytesPerPixel_);

   return DEVICE_OK;
}

/**
 * Generate an image with fixed value for all pixels
 */
void TestCamera::GenerateImage()
{
   const int maxValue = (1 << MAX_BIT_DEPTH) - 1; // max for the 12 bit camera
   const double maxExp = 1000;
   double step = maxValue/maxExp;
   unsigned char* pBuf = const_cast<unsigned char*>(img_.GetPixels());
   memset(pBuf, (int) (step * max(exposureMs_, maxExp)), img_.Height()*img_.Width()*img_.Depth());
}

/*
 * Reformat Sapera Buffer Object
 */
int TestCamera::SapBufferReformat(SapFormat format, const char * acqFormat)
{
	Xfer_->Destroy();
	AcqDevice_.SetFeatureValue("PixelFormat", acqFormat);
	Buffers_.Destroy();
	Buffers_ = SapBufferWithTrash(2, &AcqDevice_);
	Buffers_.SetFormat(format);
	AcqDeviceToBuf_ = SapAcqDeviceToBuf(&AcqDevice_, &Buffers_);
	Xfer_ = &AcqDeviceToBuf_;
	if(!Buffers_.Create())
	{
		//SapManager::DisplayMessage("Failed to recreate Buffer - SapBufferReformat");
		int ret = FreeHandles();
		if (ret != DEVICE_OK)
			return ret;
		return DEVICE_NATIVE_MODULE_FAILED;
	}
	if(Xfer_ && !Xfer_->Create())
	{
		//SapManager::DisplayMessage("Xfer_ recreation failed - SapBufferReformat");
		int ret = FreeHandles();
		if (ret != DEVICE_OK)
			return ret;
		return DEVICE_NATIVE_MODULE_FAILED;
	}
	return DEVICE_OK;
}

///////////////////////////////////////////////////////////////////////////////
// Threading methods
///////////////////////////////////////////////////////////////////////////////

int SequenceThread::svc()
{
	//SapManager::DisplayMessage("SequenceThread Start");
   long count(0);
   while (!stop_ )//&& count < numImages_)
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
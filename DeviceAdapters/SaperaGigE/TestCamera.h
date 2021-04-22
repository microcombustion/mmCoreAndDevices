///////////////////////////////////////////////////////////////////////////////
// FILE:          TestCamera.h
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Skeleton code for the micro-manager camera adapter. Use it as
//                starting point for writing custom device adapters
//                
// AUTHOR:        Nenad Amodaj, http://nenad.amodaj.com
//                
// COPYRIGHT:     University of California, San Francisco, 2011
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
//

#ifndef _TestCamera_H_
#define _TestCamera_H_

#include "DeviceBase.h"
#include "ImgBuffer.h"
#include "DeviceThreads.h"
#include "ImgBuffer.h"
#include "stdio.h"
#include "conio.h"
#include "math.h"
#include "sapclassbasic.h"
#include <string>

//////////////////////////////////////////////////////////////////////////////
// Error codes
//
#define ERR_UNKNOWN_MODE         102

class SequenceThread;

class TestCamera : public CCameraBase<TestCamera>  
{
public:
   TestCamera();
   ~TestCamera();
  
   // MMDevice API
   // ------------
   int Initialize();
   int Shutdown();
  
   void GetName(char* name) const;      
   
   // TestCamera API
   // ------------
   int SnapImage();
   const unsigned char* GetImageBuffer();
   unsigned GetImageWidth() const;
   unsigned GetImageHeight() const;
   unsigned GetImageBytesPerPixel() const;
   unsigned GetBitDepth() const;
   long GetImageBufferSize() const;
   double GetExposure() const;
   void SetExposure(double exp);
   int SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize); 
   int GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize); 
   int ClearROI();
   int PrepareSequenceAcqusition();
   int StartSequenceAcquisition(double interval);
   int StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow);
   int StopSequenceAcquisition();
   bool IsCapturing();
   int GetBinning() const;
   int SetBinning(int binSize);
   int IsExposureSequenceable(bool& seq) const {seq = false; return DEVICE_OK;}

   // action interface
   // ----------------
   int OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnPixelType(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnGain(MM::PropertyBase* pProp, MM::ActionType eAct);

private:
   friend class SequenceThread;
   static const int IMAGE_WIDTH = 1920;
   static const int IMAGE_HEIGHT = 1200;
   static const int MAX_BIT_DEPTH = 12;

   SequenceThread* thd_;
   int binning_;
   int bytesPerPixel_;
   int bitsPerPixel_;
   double gain_;
   double exposureMs_;
   bool initialized_;
   ImgBuffer img_;
   int roiX_, roiY_;
   bool sequenceRunning_;

   int ResizeImageBuffer();
   void GenerateImage();
   int InsertImage();

   UINT32 acqDeviceNumber_;
   char* acqServerName_;
   char* configFilename_;
   SapAcquisition Acq_;
   SapAcqDevice AcqDevice_;
   SapBufferWithTrash Buffers_;
   SapTransfer AcqToBuf_;
   SapTransfer AcqDeviceToBuf_;
   SapTransfer* Xfer_;
   SapLocation loc_;
   SapFeature SapGain_;
   int SapFormatBytes_;

   int FreeHandles();
   int ErrorBox(LPCWSTR text, LPCWSTR caption);
   LPCWSTR TestCamera::string2winstring(const std::string& s);
   int SapBufferReformat(SapFormat format, const char * acqFormat);
};


//threading stuff.  Tread lightly
class SequenceThread : public MMDeviceThreadBase
{
    public:
        SequenceThread(TestCamera* pCam) : stop_(false), numImages_(0) {camera_ = pCam;}
        ~SequenceThread() {}

        int svc (void);

        void Stop() {stop_ = true;}

        void Start()
        {
        stop_ = false;
        activate();
        }

        void SetLength(long images) {numImages_ = images;}
		long GetLength(void) {return numImages_;};

    private:
        TestCamera* camera_;
        bool stop_;
        long numImages_;
};

#endif //_TestCamera_H_

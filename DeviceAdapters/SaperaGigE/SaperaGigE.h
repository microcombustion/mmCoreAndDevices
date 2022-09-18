/////////////////////////////////////////////////////////
// FILE:		SaperaGigE.h
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

#ifndef _SaperaGigE_H_
#define _SaperaGigE_H_

#include "DeviceBase.h"
#include "DeviceThreads.h"
#include "ImgBuffer.h"
#include "stdio.h"
#include "conio.h"
#include "math.h"
#include "SapClassBasic.h"
#include "../MMDevice/ModuleInterface.h"
#include <string>
#include <iterator>
#include <map>

//////////////////////////////////////////////////////////////////////////////
// Error codes
//
#define ERR_UNKNOWN_MODE         102

class SequenceThread;

const char* g_CameraDeviceName = "Sapera GigE camera adapter";
const char* g_CameraServer = "AcquisitionDevice";

std::wstring s2ws(const std::string&);
int ErrorBox(std::string text, std::string caption);

class SaperaGigE : public CCameraBase<SaperaGigE>
{
private:

    struct feature
    {
        char* name;
        bool readOnly;
        CPropertyAction* action;
    };

    feature define_feature(char* name, bool readOnly, CPropertyAction* action) {
        feature out = { name, readOnly, action };
        return out;
    }

public:
    SaperaGigE();
    ~SaperaGigE();

    // MMDevice API
    // ------------
    int Initialize();
    int Shutdown();

    void GetName(char* name) const;

    // SaperaGigE API
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

    // ROI-related functions
    int SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize);
    int GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize);
    int ClearROI();

    // sequence-acquisition-related functions
    int PrepareSequenceAcqusition() { return DEVICE_OK; }
    //int StartSequenceAcquisition(double interval);
    int StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow);
    int StopSequenceAcquisition();
    bool IsCapturing();

    // pixel-size-related functions
    // the GenICam spec and the JAI sdk have no way to query sensor pixel size.
    double GetNominalPixelSizeUm() const { return 1.0; }
    double GetPixelSizeUm() const { return 1.0 * GetBinning(); }

    int GetBinning() const;
    int SetBinning(int binSize);
    int IsExposureSequenceable(bool& seq) const { seq = false; return DEVICE_OK; }

    // action interface
    // ----------------
    int OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnBinningMode(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnPixelSize(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnOffsetX(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnOffsetY(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnWidth(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnHeight(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnImageTimeout(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnTemperature(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnPixelType(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnGain(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnExposure(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnCamera(MM::PropertyBase* pProp, MM::ActionType eAct);//for multiple camera support
    int OnCameraName(MM::PropertyBase* pProp, MM::ActionType eAct);

private:

    friend class SequenceThread;
    static const int MAX_BIT_DEPTH = 12;

    ImgBuffer img_;
    SequenceThread* thd_;
    int bytesPerPixel_;
    int bitsPerPixel_;
    bool initialized_;
    bool sequenceRunning_;

    int ResizeImageBuffer();
    void GenerateImage();
    int InsertImage();

    std::vector<std::string> acqDeviceList_;
    std::string activeDevice_;

    int NumberOfAvailableCameras_;
    int NumberOfWorkableCameras_;
    int GetListOfAvailableCameras();
    SapAcqDevice AcqDevice_;
    SapAcqDevice CurrentDevice_;
    SapBufferWithTrash Buffers_;
    SapBufferRoi* Roi_;
    SapTransfer AcqToBuf_;
    SapTransfer AcqDeviceToBuf_;
    SapTransfer* Xfer_;
    SapLocation loc_;
    SapFeature AcqFeature_;

    int FreeHandles();
    int SetUpBinningProperties();
    int SynchronizeBuffers(std::string pixelFormat = "", int width = -1, int height = -1, double timeout = -1.);
    long CheckValue(const char*, long);
    static void XferCallback(SapXferCallbackInfo*);
};

//threading stuff.  Tread lightly
class SequenceThread : public MMDeviceThreadBase
{
public:
    SequenceThread(SaperaGigE* pCam) : stop_(false), numImages_(0) { camera_ = pCam; }
    ~SequenceThread() {}

    int svc(void);

    void Stop() { stop_ = true; }

    void Start()
    {
        stop_ = false;
        activate();
    }

    void SetLength(long images) { numImages_ = images; }
    long GetLength(void) { return numImages_; };

private:
    SaperaGigE* camera_;
    bool stop_;
    long numImages_;
};

#endif //_SaperaGigE_H_

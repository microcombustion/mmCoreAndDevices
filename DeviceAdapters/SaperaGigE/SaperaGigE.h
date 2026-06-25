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
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <string>
#include <iterator>
#include <map>
#include <thread>

//////////////////////////////////////////////////////////////////////////////
// Error codes
//
#define ERR_UNKNOWN_MODE         102

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
    unsigned GetNumberOfComponents() const;
    unsigned GetBitDepth() const;
    long GetImageBufferSize() const;
    double GetExposure() const;
    void SetExposure(double exp);

    // ROI-related functions
    int SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize);
    int GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize);
    int ClearROI();

    // sequence-acquisition-related functions
    int StartSequenceAcquisition(double interval_ms);
    int StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow);
    int StopSequenceAcquisition();
    bool IsCapturing();
    // Busy() reflects blocking synchronous device operations, of which this
    // callback-driven adapter has none during a sequence. Sequence/streaming state is
    // reported solely through IsCapturing() -- do NOT "fix" Busy() to track the sequence.
    bool Busy() { return false; }

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
    int OnBlackLevelSelector(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnBlackLevel(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnExposure(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnAcquisitionFrameRate(MM::PropertyBase* pProp, MM::ActionType eAct);
    int OnCamera(MM::PropertyBase* pProp, MM::ActionType eAct);//for multiple camera support
    int OnCameraName(MM::PropertyBase* pProp, MM::ActionType eAct);

private:

    static const int MAX_BIT_DEPTH = 12;

    // img_ is the single staging buffer shared by snap (GetImageBuffer) and the
    // streaming XferCallback. During a sequence, ONLY the callback writes img_: snap is
    // rejected (sequenceStarted_), buffer-resizing property changes are rejected (the
    // OnXxx guards), and GetImageBuffer() is not on the streaming path (frames go
    // straight to InsertImage). A maintainer must not read img_ from the MMCore thread
    // while a sequence is running.
    ImgBuffer img_;
    int bytesPerPixel_;
    int bitsPerPixel_;
    bool initialized_;
    // Single load-bearing lifecycle flag: a transfer is live and not yet torn down. Its
    // flip to false inside performTeardown_() is the once-only guard for both the
    // hardware stop and AcqFinished. Also what SnapImage() checks to reject snap during a
    // sequence. Read/written only under seqLock_.
    bool sequenceStarted_;
    long imageCounter_;       // counts only *delivered* frames; drives the numImages_ self-stop
    double intervalMs_;       // requested min frame spacing (<= 0: deliver every frame)
    MM::MMTime nextFrameTime_; // earliest delivery time of the next frame
    long numImages_;          // requested finite sequence length (LONG_MAX = unbounded/live)
    MMThreadLock seqLock_;    // guards sequenceStarted_, imageCounter_, intervalMs_,
                              // nextFrameTime_, and numImages_ together

    // Self-stop (numImages_ reached, or an InsertImage() error) must not call
    // Freeze()/Wait()/AcqFinished() from inside XferCallback -- Sapera serializes transfer
    // callbacks, so blocking on the same transfer's Wait() there risks deadlock (see the
    // callback's own comment). RequestStop() instead signals this worker thread, which runs
    // performTeardown_() off the callback thread. stopRequested_ is guarded SOLELY by
    // stopMutex_ -- never by seqLock_ -- so the worker's wait/notify can never miss a signal.
    std::thread stopWorker_;
    std::atomic<bool> stopRequested_;
    std::mutex stopMutex_;
    std::condition_variable stopCv_;
    void RequestStop();
    void StopWorkerLoop_();
    void performTeardown_();

    int ResizeImageBuffer();
    void GenerateImage();

    std::vector<std::string> acqDeviceList_;
    std::string activeDevice_;

    int NumberOfAvailableCameras_;
    int NumberOfWorkableCameras_;
    int GetListOfAvailableCameras();
    SapAcqDevice AcqDevice_;
    SapAcqDevice CurrentDevice_;
    // Buffers_/AcqDeviceToBuf_ are constructed exactly once (in SynchronizeBuffers(), on
    // first call) and torn down via Destroy()/Create() in place from then on. Never
    // reassign them from a freshly-constructed temporary: SapAcqDeviceToBuf's constructor
    // registers a transfer pair that stores a pointer back to the constructed instance
    // (see AddPair() in the Sapera++ SDK), so assigning from a temporary leaves that
    // pointer dangling the moment the temporary is destroyed at the end of the statement.
    SapBufferWithTrash* Buffers_;
    // Roi_ is a child of Buffers_ (its parent), rebuilt every SynchronizeBuffers() call and
    // on every SetROI()/ClearROI() call -- unlike Buffers_/AcqDeviceToBuf_ it is NOT
    // construct-once. Must go through the same Create()/Destroy() discipline as the SDK's own
    // SapBufferRoi demos. SapBufferRoi::SetRoi()/ResetRoi() are pre-Create() only (the SDK
    // rejects them after Create() with "Cannot be called after the Create Method"); ROI
    // geometry is therefore stored in roiX_/roiY_/roiW_/roiH_ and passed to the constructor
    // every time a new Roi_ is allocated (-1 for roiW_/roiH_ means "full parent extent").
    SapBufferRoi* Roi_;
    int roiX_, roiY_, roiW_, roiH_;
    SapAcqDeviceToBuf* AcqDeviceToBuf_;
    SapTransfer* Xfer_;
    // Set once in Initialize() from AcqDevice_.IsRawBayerOutput() (the same call Sapera's
    // own CamExpert/demo apps use to detect a color sensor). When true, SynchronizeBuffers()
    // constructs Conv_ with the same construct-once/Destroy()-Create()-in-place discipline
    // as Buffers_/AcqDeviceToBuf_ above -- same dangling-pointer hazard applies.
    bool isColor_;
    SapColorConversion* Conv_;
    SapLocation loc_;
    SapFeature AcqFeature_;

    int FreeHandles();
    int SetUpBinningProperties();
    int SetUpFrameRateProperty();
    int SynchronizeBuffers(std::string pixelFormat = "", int width = -1, int height = -1, double timeout = -1.);
    long CheckValue(const char*, long);
    static void XferCallback(SapXferCallbackInfo*);
};

#endif //_SaperaGigE_H_

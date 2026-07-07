/////////////////////////////////////////////////////////
// FILE:		SaperaGigE.h
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
// COPYRIGHT:     Louisiana State University, 2026
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
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <iterator>
#include <map>
#include <thread>

//////////////////////////////////////////////////////////////////////////////
// Error codes
//
#define ERR_UNKNOWN_MODE         102

class SaperaGigE : public CCameraBase<SaperaGigE>
{
private:

    struct feature
    {
        const char* name;
        bool readOnly;
        CPropertyAction* action;
    };

    feature define_feature(const char* name, bool readOnly, CPropertyAction* action) {
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

    // MM::Camera API
    // --------------
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

private:

    // img_ is the single staging buffer shared by snap (GetImageBuffer) and the
    // streaming XferCallback. During a sequence, only the callback writes img_: snap is
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
    bool transferActive_;
    long imageCounter_;       // counts only *delivered* frames; drives the numImages_ self-stop
    double intervalMs_;       // requested min frame spacing (<= 0: deliver every frame)
    MM::MMTime nextFrameTime_; // earliest delivery time of the next frame
    long numImages_;          // requested finite sequence length (LONG_MAX = unbounded/live)
    MMThreadLock seqLock_;    // guards sequenceStarted_, imageCounter_, intervalMs_,
                              // nextFrameTime_, numImages_, and transferActive_ together
    mutable std::recursive_mutex saperaMutex_; // serializes calls into Sapera SDK objects

    // Self-stop (numImages_ reached, or an InsertImage() error) must not call
    // Freeze()/Wait()/AcqFinished() from inside XferCallback -- Sapera serializes transfer
    // callbacks, so blocking on the same transfer's Wait() there risks deadlock (see the
    // callback's own comment). RequestStop() instead signals this worker thread, which runs
    // performTeardown_() off the callback thread. stopRequested_ is guarded only by
    // stopMutex_ -- never by seqLock_ -- so the worker's wait/notify can never miss a signal.
    std::thread stopWorker_;
    std::atomic<bool> stopRequested_;
    std::mutex stopMutex_;
    std::condition_variable stopCv_;
    void RequestStop();
    void StopWorkerLoop_();
    void performTeardown_();

    int ResizeImageBuffer();

    std::vector<std::string> acqDeviceList_;
    std::string activeDevice_;

    int GetListOfAvailableCameras();
    SapAcqDevice AcqDevice_;
    // Buffers_/AcqDeviceToBuf_/Xfer_ are owned as a unit and rebuilt together whenever
    // SynchronizeBuffers() changes a feature that affects Sapera buffer layout. Never
    // reassign SapAcqDeviceToBuf from a freshly-constructed temporary: its constructor
    // registers a transfer pair that stores a pointer back to the constructed instance
    // (see AddPair() in the Sapera++ SDK), so assigning from a temporary leaves that
    // pointer dangling the moment the temporary is destroyed at the end of the statement.
    // Plain SapBuffer (no trash buffer): the trash resource is an extra buffer mapping
    // registered with the transfer connection, and the 2026-07-06 Active-dump analysis
    // showed the BSOD is a double-unmap of a connection mapping record inside
    // SapTransfer::Disconnect (see BSOD.md section 5). Dropping the trash resource
    // removes one aliased record from that bookkeeping; DalsaPythonConnector uses plain
    // SapBuffer on this same stack without crashes. Overflow now shows up as dropped/
    // overwritten frames within the 3-buffer ring instead of trash-buffer events.
    SapBuffer* Buffers_;
    // Software ROI geometry. The transfer always delivers full frames into Buffers_;
    // cropping happens in GetImageBuffer()/XferCallback via ReadRect(roiX_, roiY_, ...)
    // with img_ sized to the ROI by ResizeImageBuffer(). -1 for roiW_/roiH_ means
    // "full frame". No SapBufferRoi is used: it was never connected to the transfer or
    // conversion pipeline, and its per-reconfigure child-buffer/trash-child kernel
    // objects exercised the fragile cormem.sys locked-page unmap path (see BSOD.md).
    int roiX_, roiY_, roiW_, roiH_;
    SapAcqDeviceToBuf* AcqDeviceToBuf_;
    SapTransfer* Xfer_;
    // Set once in Initialize() from AcqDevice_.IsRawBayerOutput() (the same call Sapera's
    // own CamExpert/demo apps use to detect a color sensor). When true, SynchronizeBuffers()
    // owns Conv_ as part of the same rebuildable Sapera pipeline as Buffers_/Xfer_.
    bool isColor_;
    SapColorConversion* Conv_;
    SapLocation loc_;
    SapFeature AcqFeature_;

    int FreeHandles();
    int DestroySaperaPipeline_();
    int DestroySaperaPipelineForReconfigure_();
    int SetUpBinningProperties();
    int SetUpFrameRateProperty();
    bool IsFeatureAvailable(const char* featureName);
    int SynchronizeBuffers(std::string pixelFormat = "", int width = -1, int height = -1, double timeout = -1.);
    long CheckValue(const char*, long);
    static void XferCallback(SapXferCallbackInfo*);
};

#endif //_SaperaGigE_H_

•	Files changed
•	DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.h
•	DeviceAdapters/AlliedVisionCamera/AlliedVisionCamera.cpp
•	High-level goal
    •	Remove likely use-after-free and races during repeated SnapImage() calls while exposure (or other features) change asynchronously via Vimba callbacks.
•	Concrete changes
1.	Buffer reallocation
    •	resizeImageBuffer() now computes newBufferSize and only reallocates when the size actually increases or buffers are unallocated. (avoids deleting/reallocating on every snap)
2.	Thread-safety
    •	Added mutable std::mutex m_bufferMutex to AlliedVisionCamera.
    •	Protected accesses/mutations of m_buffer, m_bufferSize, m_payloadSize, and m_currentPixelFormat with std::lock_guard<std::mutex>.
    •	Functions using the protected state now either hold the lock while reading/writing or copy needed state under the lock to local variables (e.g., transformImage, insertFrame, getters).
3.	Snap frame lifetime
    •	Introduced a class-owned VmbFrame_t m_snapFrame (in the header) and use it in SnapImage() instead of a stack-allocated VmbFrame_t. (eliminates stack lifetime risk)
4.	Ensure frames revoked before resize
    •	SnapImage() calls VmbFrameRevokeAll_t(m_handle) (best-effort, ignore return) before calling resizeImageBuffer() to reduce risk that SDK still references old buffers when they are deleted.
5.	StartSequenceAcquisition buffer assignment
    •	Assignment of m_frames[i].buffer and .bufferSize is done under the buffer mutex to avoid a race with concurrent property callbacks or resizeImageBuffer().
6.	Pixel-format mutation protection
    •	handlePixelFormatChange() now sets the pixel type under the m_bufferMutex.
7.	Safe copying in transform/insert
    •	transformImage() copies m_bufferSize and destination VMB pixel format into local variables under lock and uses those locals for allocation and transform steps.
    •	insertFrame() copies bytes-per-pixel and component counts under lock before calling GetCoreCallback()->InsertImage.
8.	Minor include
    •	Added <mutex> and <iomanip> where required.
•	Rationale
    •	Reducing buffer churn removes the primary source of use-after-free when the SDK may still hold references to previously-announced frames.
    •	The mutex prevents races between asynchronous Vimba feature invalidation callbacks (which update pixel format / properties) and capture code that reads or reallocates buffers.
    •	Replacing the stack frame with a class member avoids passing a pointer to a stack object into the SDK.
•	What I did not change
    •	I preserved overall capture logic and Vimba API calls (no semantic changes to start/stop flow beyond the revoke-before-resize attempt).
    •	I did not add global locking across all property callbacks or the entire capture pipeline to keep the changes minimal and low risk.
•	Recommended next steps (optional)
    •	Confirm Vimba threading/ownership guarantees (whether announced frames and their buffers must remain valid until revoke completes).
    •	Add diagnostic logging around VmbFrameAnnounce_t / VmbFrameRevokeAll_t to detect any SDK-queued references at runtime.
    •	Optionally extend locking coverage (or use a more fine-grained reader/writer strategy) if callbacks still race with capture under heavy load.
    •	Run the exposure-change + SnapImage stress test to validate stability.

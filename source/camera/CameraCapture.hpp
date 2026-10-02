#pragma once

#include <SDL.h>
#include <string>

namespace Camera {

    enum class Status {
        Closed,
        Starting,
        Running,
        NoCamera,
        Error
    };

    constexpr int WIDTH      = 640;
    constexpr int HEIGHT     = 480;
    constexpr int FRAME_RATE = 30;

    bool Open(int instance = 0, bool highFrameRate = true);
    void Close();

    bool   IsOpen();
    Status GetStatus();

    const char* GetStatusMessage();

    SDL_Surface* AcquireFrame(bool mirror, bool force = false);

    bool HasNewFrame();

    // Diagnostics for the on-screen HUD.
    uint64_t GetFrameCount();
    float    GetMeasuredFps();

    void ResetStats();

// Diagnostics

struct Diagnostics {
    bool     valid = false;

    // Pipeline accounting
    uint64_t framesProduced = 0;
    uint64_t framesConsumed = 0;
    uint64_t framesDropped  = 0;
    uint64_t decodeFailed   = 0;
    uint64_t resyncFailures = 0;

    uint64_t jobsStarted   = 0;
    uint64_t jobsCompleted = 0;
    uint64_t jobsRejected  = 0;

    uint64_t handoffChecks   = 0;
    uint64_t handoffMismatch = 0;

    uint64_t cacheChecks = 0;
    uint64_t cacheStale  = 0;
    float    cacheStaleLinesPct = 0.f;

    uint64_t settleChecks   = 0;
    uint64_t settleChanged  = 0;

    float yMean = 0.f, yStd = 0.f;
    float uMeanPitchStride = 0.f, vMeanPitchStride = 0.f, vSpreadPitchStride = 0.f;
    float uMeanWidthStride = 0.f, vMeanWidthStride = 0.f, vSpreadWidthStride = 0.f;
    float uMeanUnpadded    = 0.f, vMeanUnpadded    = 0.f, vSpreadUnpadded    = 0.f;

    float convertMsAvg = 0.f;
    float convertMsMax = 0.f;

    float producedFps = 0.f;
    float consumedFps = 0.f;

    uint64_t drcDetach = 0;
    uint32_t lastDetachAt = 0;

    uint64_t tearFrames        = 0;
    uint64_t tearRows          = 0;
    uint32_t tearLastFrameAt   = 0;
    int      tearWorstRow      = 0;
    float    tearWorstDelta    = 0.f;
float tearRowsThreshold = 14.f;

    float outRMean = 0.f, outGMean = 0.f, outBMean = 0.f;
    bool  outValid = false;

    int   scratchPitch = 0;
    int   scratchPixelsAligned = 0;

    int  textureScaleMode = -1;
    const char* rendererName = "";
    const char* lastSdlError = "";
};

Diagnostics GetDiagnostics();

void LogDiagnostics(const char* tag);

void ResetDiagnostics();

void SetDumpPath(const std::string& path);

}
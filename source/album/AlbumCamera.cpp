#include "Album.hpp"
#include "../camera/CameraCapture.hpp"
#include "../ui/Glyphs.hpp"
#include "../ui/Log.hpp"

#include <SDL_image.h>
#include <coreinit/debug.h>
#include <whb/log.h>

#include <sys/stat.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

static const SDL_Color CAM_REC_RED = {0xe5, 0x33, 0x2a, 0xff};
static const SDL_Color CAM_OK      = {0x2e, 0x9a, 0x2e, 0xff};

static void MaskRoundedCorners(int x, int y, int w, int h, int radius, SDL_Color color) {
    if (radius <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    for (int i = 0; i < radius; i++) {
        float dy = (float)(radius - i) - 0.5f;
        float halfw = std::sqrt((float)(radius * radius) - dy * dy);
        int outside = radius - (int)(halfw + 0.5f) + 1;
        if (outside <= 0) continue;
        if (outside > radius) outside = radius;

        Gfx::DrawRectFilled(x,                 y + i,             outside,           1, color); // TL
        Gfx::DrawRectFilled(x + w - outside,   y + i,             outside,           1, color); // TR
        Gfx::DrawRectFilled(x,                 y + h - 1 - i,     outside,           1, color); // BL
        Gfx::DrawRectFilled(x + w - outside,   y + h - 1 - i,     outside,           1, color); // BR
    }
}

// Box-filters an RGBA32 surface down to a thumbnail-sized one.
static SDL_Surface* MakeThumbnail(SDL_Surface* src, int tw, int th) {
    if (!src) return nullptr;
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32,
                                                      SDL_PIXELFORMAT_RGBA32);
    if (!dst) return nullptr;

    const uint8_t* s = (const uint8_t*)src->pixels;
    uint8_t*       d = (uint8_t*)dst->pixels;

    for (int ty = 0; ty < th; ty++) {
        int y0 = ty * src->h / th;
        int y1 = (ty + 1) * src->h / th;
        if (y1 <= y0) y1 = y0 + 1;
        for (int tx = 0; tx < tw; tx++) {
            int x0 = tx * src->w / tw;
            int x1 = (tx + 1) * src->w / tw;
            if (x1 <= x0) x1 = x0 + 1;

            int acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t* row = s + (size_t)y * src->pitch;
                for (int x = x0; x < x1; x++) {
                    acc[0] += row[x * 4 + 0];
                    acc[1] += row[x * 4 + 1];
                    acc[2] += row[x * 4 + 2];
                    acc[3] += row[x * 4 + 3];
                    n++;
                }
            }
            if (!n) n = 1;

            uint8_t* p = d + (size_t)ty * dst->pitch + (size_t)tx * 4;
            p[0] = (uint8_t)(acc[0] / n);
            p[1] = (uint8_t)(acc[1] / n);
            p[2] = (uint8_t)(acc[2] / n);
            p[3] = 0xff;
        }
    }
    return dst;
}

static std::string FormatSessionTime(Uint32 elapsedMs) {
    char buf[16];
    int total = (int)(elapsedMs / 1000);
    snprintf(buf, sizeof(buf), "%02d:%02d", total / 60, total % 60);
    return buf;
}

bool Album::CameraRunning() const {
    return Camera::GetStatus() == Camera::Status::Running ||
           Camera::GetStatus() == Camera::Status::Starting;
}

void Album::ApplyCameraSettings() {
    mCameraMirror   = mSettingsCamMirror;
    mCameraGrid     = mSettingsCamGrid;
    mCameraFps      = mSettingsCamFps;
    mCameraInstance = mSettingsCamSource;
}

void Album::EnterCameraMode() {
    if (mCameraActive) return;

    if (mMultiSelect) ExitMultiSelect(true);
    if (mTransferMultiSelect) {
        mTransferMultiSelect = false;
        mTransferSelected.clear();
        mTransferSelectCount = 0;
    }
    mTransferMode      = TransferMode::None;
    mViewerSidePanel   = false;
    mViewerConfirmDelete = false;
    mSaveNotifEndTime  = 0;

    if (!mMicReady) {
        mMicReady = mMic.Open(0);
        if (!mMicReady) mMicError = mMic.GetError();
        else            mMicError.clear();
    }

    mCameraActive = true;
    ApplyCameraSettings();

    mCameraHasFrame   = false;
    mCamSessionStart  = SDL_GetTicks();
    mCamFlashStart    = 0;
    mCamFlashEnd      = 0;
    mCamNotifEnd      = 0;
    mCamShotsTaken    = 0;
    mCamDiagLastTime  = SDL_GetTicks();
    ClearCameraThumbs();

    {
        std::string dir = ComputeSdRoot() + "/wiiu/apps/WiiUAlbum";
        mkdir(dir.c_str(), 0777);
        Camera::SetDumpPath(dir + "/cam_raw.nv12");
    }

    mOverlay     = Overlay::None;
    mSidebarFocus = false;

    bool opened = Camera::Open(mCameraInstance, mCameraFps >= 30);
    if (opened) {
        ALBUM_LOG("Camera: mode entered");
    } else {
        ALBUM_LOG("Camera: mode entered but the camera failed: %s",
                     Camera::GetStatusMessage());
    }
}

void Album::ExitCameraMode() {
    WriteCameraDiagnostics("final");
    if (mCamRecording) StopCameraRecording(false);
    Camera::Close();

    if (mCameraTexture) { Gfx::DestroyTexture(mCameraTexture); mCameraTexture = nullptr; }
    ClearCameraThumbs();

    mMic.Close();
    mMicReady = false;

    mCameraHasFrame = false;
    mCameraActive   = false;
    mCamNotifEnd    = 0;

    // Newly captured photos belong in the grid.
    if (mCamShotsTaken > 0) {
        mPendingRefresh = true;
        Refresh();
    }

    mSidebarFocus = true;
    mSidebarSel   = 4;
    ALBUM_LOG("Camera: mode exited after %d shot(s)", mCamShotsTaken);
    if (mCamRecording) ALBUM_ERROR("Camera: recording was still active on exit");
}

void Album::ClearCameraThumbs() {
    for (auto* tex : mCamThumbs) Gfx::DestroyTexture(tex);
    mCamThumbs.clear();
    mCamThumbNames.clear();
}

std::string Album::CameraNextFileName() const {
    char stamp[32];
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof(stamp), "Camera_%Y-%m-%d_%H-%M-%S", &tmv);

    std::string outPath = mPathScreenshots + "/" + stamp + ".png";
    struct stat st;
    int counter = 1;
    while (stat(outPath.c_str(), &st) == 0) {
        outPath = mPathScreenshots + "/" + stamp + "_" +
                  std::to_string(counter++) + ".png";
    }
    return outPath;
}

void Album::CaptureCameraPhoto() {
    if (!CameraRunning()) return;

    SDL_Surface* frame = Camera::AcquireFrame(mCameraMirror, true);
    if (!frame) {
        mCamNotifText = "No frame available yet";
        mCamNotifError = true;
        mCamNotifEnd   = SDL_GetTicks() + 2200;
        return;
    }

    std::string outPath = CameraNextFileName();
    bool ok = (IMG_SavePNG(frame, outPath.c_str()) == 0);
    if (!ok) {
        ALBUM_ERROR("Camera: photo save failed: %s", outPath.c_str());
        mCamNotifText  = "Could not write the photo to the SD card";
        mCamNotifError = true;
        mCamNotifEnd   = SDL_GetTicks() + 2500;
        return;
    }

    // Remember a thumbnail for the recent shots grid.
    SDL_Surface* thumbSurf = MakeThumbnail(frame, CAM_THUMB_W, CAM_THUMB_H);
    if (thumbSurf) {
        SDL_Texture* tex = SDL_CreateTextureFromSurface(Gfx::GetRenderer(), thumbSurf);
        SDL_FreeSurface(thumbSurf);
        if (tex) {
            if ((int)mCamThumbs.size() >= CAM_MAX_THUMBS) {
                if (mCamThumbs.front()) Gfx::DestroyTexture(mCamThumbs.front());
                mCamThumbs.erase(mCamThumbs.begin());
                mCamThumbNames.erase(mCamThumbNames.begin());
            }
            mCamThumbs.push_back(tex);
            mCamThumbNames.push_back(outPath);
        }
    }

    mCamShotsTaken++;
    mCamNotifText  = "Photo saved";
    mCamNotifError = false;
    mCamNotifEnd   = SDL_GetTicks() + 1800;

    // Shutter flash.
    Uint32 now = SDL_GetTicks();
    mCamFlashStart = now;
    mCamFlashEnd   = now + 320;

    OSReport("[ALBUM] Camera photo saved: %s", outPath.c_str());
}

void Album::ToggleCameraRecording() {
    if (mCamRecording) {
        StopCameraRecording(true);
        return;
    }
    if (!mCameraHasFrame) {
        mCamNotifText  = "No camera frame to record yet";
        mCamNotifError = true;
        mCamNotifEnd   = SDL_GetTicks() + 2200;
        return;
    }

    if (!mMicReady && !mMicError.empty()) {
        mCamNotifText  = "Recording without audio: " + mMicError;
        mCamNotifError = false;
        mCamNotifEnd   = SDL_GetTicks() + 3200;
    }

    char stamp[32];
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof(stamp), "Video_%Y-%m-%d_%H-%M-%S", &tmv);

    mCamRecordPath = mPathVideos + "/" + stamp + ".avi";

    mRecorder.SetMic(mMicReady ? &mMic : nullptr);
    if (!mRecorder.Start(mCamRecordPath, 640, 480, mCameraFps)) {
        mCamNotifText  = "Could not start recording: " + mRecorder.GetError();
        mCamNotifError = true;
        mCamNotifEnd   = SDL_GetTicks() + 3000;
        return;
    }

    mCamRecording   = true;
    mCamRecordStart = SDL_GetTicks();
    ALBUM_LOG("Camera: recording started");
}

void Album::StopCameraRecording(bool notify) {
    if (!mCamRecording) return;

    uint32_t frames = mRecorder.GetFrameCount();
    double   secs   = mRecorder.GetDuration();
    uint64_t bytes  = mRecorder.GetBytesWritten();
    std::string path = mRecorder.GetPath();
    std::string err  = mRecorder.GetError();

    ALBUM_LOG("Camera: record start -> %s (%dx%d @%d, audio=%s)",
              mCamRecordPath.c_str(), 640, 480, mCameraFps,
              mMicReady ? "on" : "off");

    mCamRecording = false;
    mRecorder.Stop();
    mRecorder.ValidateFile(path);

    if (notify) {
        if (!err.empty()) {
            mCamRecordNotifErr = true;
            mCamRecordNotifText = "Recording error: " + err;
        } else if (frames == 0) {
            mCamRecordNotifErr = true;
            mCamRecordNotifText = "No frames were recorded";
        } else {
            mCamRecordNotifErr = false;
            char buf[128];
            snprintf(buf, sizeof(buf), "Saved %.0fs (%u frames, %llu KB)",
                     secs, frames, (unsigned long long)(bytes / 1024));
            mCamRecordNotifText = buf;
            mPendingRefresh = true;
        }
        mCamRecordNotifEnd = SDL_GetTicks() + 3200;
    }

    if (!err.empty()) {
        ALBUM_ERROR("Camera: recording FAILED (%s)", err.c_str());
    } else if (frames == 0) {
        ALBUM_ERROR("Camera: recording produced no frames");
    } else {
        ALBUM_LOG("Camera: recording saved %s (%u frames, %.0fs, %u KB, %s audio)",
                  path.c_str(), frames, secs,
                  (unsigned)(bytes / 1024), mMicReady ? "with" : "no");
    }
}

void Album::WriteCameraDiagnostics(const char* tag) {
    Camera::Diagnostics d = Camera::GetDiagnostics();

    SDL_RendererInfo info;
    memset(&info, 0, sizeof(info));
    SDL_GetRendererInfo(Gfx::GetRenderer(), &info);

    SDL_ScaleMode mode = SDL_ScaleModeNearest;
    if (mCameraTexture) SDL_GetTextureScaleMode(mCameraTexture, &mode);

    const char* modeName = "nearest";
    switch (mode) {
        case SDL_ScaleModeNearest: modeName = "nearest"; break;
        case SDL_ScaleModeLinear:  modeName = "linear";  break;
        case SDL_ScaleModeBest:    modeName = "best";    break;
        default: break;
    }

    char renderer[128] = {0};
    snprintf(renderer, sizeof(renderer), "%s", info.name ? info.name : "?");

    const char* sdlError = mCamUploadLastError.empty() ? "none" : mCamUploadLastError.c_str();

    std::string report;
    char line[3072];

    snprintf(line, sizeof(line),
             "\n===== camera diagnostics: %s =====\n"
             "status              : %d (%s)\n"
             "message             : %s\n"
             "renderer            : %s\n"
             "preview scale mode  : %s%s\n"
             "sdl error after up  : %s\n"
             "frames produced     : %llu\n"
             "frames consumed     : %llu\n"
             "frames dropped      : %llu\n"
             "decode failures     : %llu\n"
             "resync failures     : %llu\n"
             "handoff checks     : %llu  mismatches: %llu\n"
             "cache checks        : %llu  stale: %llu  (%.1f%% of 32B lines)\n"
             "dma settle checks   : %llu  still changing: %llu\n"
             "luma mean / std     : %.1f / %.1f\n"
             "chroma pitch=768    : u=%6.1f v=%6.1f vspread=%6.1f\n"
             "chroma pitch=640    : u=%6.1f v=%6.1f vspread=%6.1f\n"
             "chroma unpadded     : u=%6.1f v=%6.1f vspread=%6.1f\n"
             "convert avg / max   : %.2f ms / %.2f ms\n"
             "phase update avg   : %.2f ms (max %.2f)\n"
             "phase draw   avg   : %.2f ms\n"
             "phase present avg  : %.2f ms (max %.2f)\n"
             "jobs started/done  : %llu / %llu  (rejected %llu)\n"
             "frame total avg/max : %.2f ms / %.2f ms   (60Hz budget is 16.67)\n"
             "draw avg / max      : %.2f ms / %.2f ms\n"
             "texture upload fails: %u (%s)\n"
             "OUTPUT r/g/b mean   : %.1f / %.1f / %.1f   (luma mean was %.1f)\n"
             "scratch pitch       : %d   pixels 4-byte aligned: %s\n"
             "consumed fps        : %.1f\n"
             "camera fps setting  : %d\n"
             "mirror / grid       : %s / %s\n"
             "photos this session : %d\n",
             tag ? tag : "",
             (int)Camera::GetStatus(), Camera::IsOpen() ? "open" : "closed",
             Camera::GetStatusMessage(),
             renderer,
             modeName,
             (mode == SDL_ScaleModeLinear) ? "  (expected)" : "  (WARNING: not set!)",
             sdlError,
             (unsigned long long)d.framesProduced, (unsigned long long)d.framesConsumed,
             (unsigned long long)d.framesDropped,  (unsigned long long)d.decodeFailed,
             (unsigned long long)d.resyncFailures,
             (unsigned long long)d.handoffChecks,   (unsigned long long)d.handoffMismatch,
             (unsigned long long)d.cacheChecks,     (unsigned long long)d.cacheStale,
             d.cacheStaleLinesPct,
             (unsigned long long)d.settleChecks,    (unsigned long long)d.settleChanged,
             d.yMean, d.yStd,
             d.uMeanPitchStride, d.vMeanPitchStride, d.vSpreadPitchStride,
             d.uMeanWidthStride,  d.vMeanWidthStride,  d.vSpreadWidthStride,
             d.uMeanUnpadded,     d.vMeanUnpadded,     d.vSpreadUnpadded,
             d.convertMsAvg, d.convertMsMax,
             (double)mCamUpdateMsAvg, (double)mCamUpdateMsMax,
             (double)mCamDrawMsAvg,
             (double)mCamRenderMsAvg, (double)mCamRenderMsMax,
             (unsigned long long)d.jobsStarted, (unsigned long long)d.jobsCompleted,
             (unsigned long long)d.jobsRejected,
             (double)mCamFrameMsAvg, (double)mCamFrameMsMax,
             (double)mCamDrawMsAvg,  (double)mCamDrawMsMax,
             (unsigned)mCamUploadErrors, sdlError,
             d.outRMean, d.outGMean, d.outBMean, d.yMean,
             d.scratchPitch, d.scratchPixelsAligned ? "yes" : "NO",
             d.consumedFps,
             mCameraFps,
             mCameraMirror ? "on" : "off", mCameraGrid ? "on" : "off",
             mCamShotsTaken);
    report += line;

    ALBUM_LOG_QUIET("[CAM][diag] --- %s ---", tag ? tag : "");
    ALBUM_LOG_QUIET("%s", report.c_str());

    std::string dir = ComputeSdRoot() + "/wiiu/apps/WiiUAlbum";
    mkdir(dir.c_str(), 0777);
    std::string path = dir + "/camdiag.txt";

    FILE* f = fopen(path.c_str(), "a");
    if (f) {
        fwrite(report.c_str(), 1, report.size(), f);
        fclose(f);
    } else {
        ALBUM_ERROR("Camera: could not append diagnostics to %s", path.c_str());
    }
}

void Album::RecordCameraFrameTiming(Uint32 frameMs, Uint32 drawMs) {
    float n = (float)(++mCamFrameSamples);
    mCamFrameMsAvg += ((float)frameMs - mCamFrameMsAvg) / n;
    if (frameMs > mCamFrameMsMax) mCamFrameMsMax = frameMs;

    float m = (float)(++mCamDrawSamples);
    mCamDrawMsAvg += ((float)drawMs - mCamDrawMsAvg) / m;
    if (drawMs > mCamDrawMsMax) mCamDrawMsMax = drawMs;
}

void Album::RecordCameraPhases(Uint32 updateMs, Uint32 drawMs, Uint32 renderMs) {
    if (!mCameraActive) return;

    float n = (float)(++mCamPhaseSamples);
    mCamUpdateMsAvg += ((float)updateMs - mCamUpdateMsAvg) / n;
    mCamRenderMsAvg += ((float)renderMs - mCamRenderMsAvg) / n;
    if (updateMs > mCamUpdateMsMax) mCamUpdateMsMax = updateMs;
    if (renderMs > mCamRenderMsMax) mCamRenderMsMax = renderMs;
}

static std::string fmtMs(float ms) {
    int whole = (int)ms;
    int tenth = (int)((ms - (float)whole) * 10.f + 0.5f);
    if (tenth > 9) { whole++; tenth = 0; }
    return std::to_string(whole) + "." + std::to_string(tenth);
}

void Album::DrawCameraDiagnostics() {
    const auto& th = Gfx::Theme();
    Camera::Diagnostics d = Camera::GetDiagnostics();

    SDL_ScaleMode mode = SDL_ScaleModeNearest;
    if (mCameraTexture) SDL_GetTextureScaleMode(mCameraTexture, &mode);
    const int PW = 980;
    const int PH = PW * Camera::HEIGHT / Camera::WIDTH;
    const int PY = CAM_HEADER_H + 24;
    const int PX = (Gfx::SCREEN_WIDTH - PW) / 2;

    Gfx::DrawRectFilled(PX, PY, PW, PH, {0x00, 0x00, 0x00, 0xc0});
    Gfx::DrawRectRounded(PX, PY, PW, PH, 14, th.cardBg);

    int y = PY + 22;
    Gfx::Print(PX + 28, y, 30, th.text, "Camera diagnostics", Gfx::ALIGN_LEFT);
    Gfx::Print(PX + PW - 28, y, 24, th.textDim, "L+R+X+A+B to hide",
               Gfx::ALIGN_RIGHT);
    y += 48;

    struct Row { const char* label; std::string value; bool warn; };
    std::vector<Row> rows = {
        {"produced / consumed",
         std::to_string(d.framesProduced) + " / " + std::to_string(d.framesConsumed),
         d.framesConsumed == 0},
        {"dropped by render thread",
         std::to_string(d.framesDropped), d.framesDropped > 0},
        {"decode failures / resync failures",
         std::to_string(d.decodeFailed) + " / " + std::to_string(d.resyncFailures),
         (d.decodeFailed + d.resyncFailures) > 0},
        {"GamePad detaches",
         std::to_string(d.drcDetach), d.drcDetach > 0},
        {"handoff mismatches",
         std::to_string(d.handoffMismatch) + " of " + std::to_string(d.handoffChecks),
         d.handoffMismatch > 0},
        {"stale cache lines detected",
         std::to_string(d.cacheStale) + " lines" +
             "  (" + std::to_string((int)d.cacheStaleLinesPct) + "%)",
         d.cacheStale > 0},
        {"DMA still moving",
         std::to_string(d.settleChanged) + " of " + std::to_string(d.settleChecks),
         d.settleChanged > 0},
        {"TORN FRAMES / suspect rows",
         std::to_string(d.tearFrames) + " frames, " + std::to_string(d.tearRows) +
             " rows  (" + std::to_string(d.tearWorstRow) + " rows, delta " +
             fmtMs(d.tearWorstDelta) + ")",
         d.tearFrames > 0},
        {"conversion cost",
         std::to_string((int)d.convertMsAvg) + " ms avg, " +
             std::to_string((int)d.convertMsMax) + " ms max",
         d.convertMsAvg > 8.f},
        {"frame phase: update",
         fmtMs(mCamUpdateMsAvg) + " ms avg, " + std::to_string((int)mCamUpdateMsMax) + " max",
         mCamUpdateMsAvg > 16.7f},
        {"frame phase: render",
         fmtMs(mCamRenderMsAvg) + " ms avg, " + std::to_string((int)mCamRenderMsMax) + " max",
         mCamRenderMsAvg > 16.7f},
        {"convert jobs started",
         std::to_string(d.jobsStarted) + " started, " +
             std::to_string(d.jobsCompleted) + " done",
         false},
        {"convert jobs skipped",
         std::to_string(d.jobsRejected) + " skipped", false},
        {"texture upload errors",
         std::to_string(mCamUploadErrors) +
             (mCamUploadErrors ? "  " + mCamUploadLastError : ""),
         mCamUploadErrors > 0},
        {"preview filter",
         mode == SDL_ScaleModeLinear ? "linear" : "nearest",
         mode != SDL_ScaleModeLinear},
    };

    for (const auto& r : rows) {
        Gfx::Print(PX + 28, y, 22, th.textDim, r.label,
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(PX + PW - 28, y, 22, r.warn ? Gfx::COLOR_BTN_Y : th.text, r.value,
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
        y += 30;
    }

    Gfx::DrawRectFilled(PX + 28, y, PW - 56, 1, th.separator);
    y += 18;

    Gfx::Print(PX + 28, y, 22, th.textDim, "luma mean / std",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    Gfx::Print(PX + PW - 28, y, 22, th.text,
               std::to_string((int)d.yMean) + " / " + std::to_string((int)d.yStd),
               Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
    y += 30;

    struct ChromaRow { const char* label; float u, v, spread; };
    const ChromaRow chroma[] = {
        {"chroma stride 768", d.uMeanPitchStride, d.vMeanPitchStride, d.vSpreadPitchStride},
        {"chroma width 640", d.uMeanWidthStride, d.vMeanWidthStride, d.vSpreadWidthStride},
        {"chroma unpadded",  d.uMeanUnpadded,    d.vMeanUnpadded,    d.vSpreadUnpadded},
    };
    for (const auto& c : chroma) {
        Gfx::Print(PX + 28, y, 22, th.textDim, c.label,
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(PX + PW - 28, y, 22, th.text,
                   "u " + std::to_string((int)c.u) +
                   "   v " + std::to_string((int)c.v) +
                   "   spread " + std::to_string((int)c.spread),
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
        y += 30;
    }

    if (d.drcDetach > 0) {
        Gfx::Print(PX + 28, PH + PY - 62, 21, Gfx::COLOR_BTN_Y,
                   "GamePad detached " +
                   std::to_string((int)((SDL_GetTicks() - d.lastDetachAt) / 1000)) +
                   "s ago - try keeping the GamePad screen awake",
                   Gfx::ALIGN_LEFT);
    }
}

void Album::UploadCameraFrame(SDL_Surface* frame) {
    if (!frame) return;

    if (!mCameraTexture) {
        mCameraTexture = SDL_CreateTexture(Gfx::GetRenderer(),
                                           SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           frame->w, frame->h);
        if (!mCameraTexture) {
            static unsigned fails = 0;
            if (fails == 0 || fails % 120 == 0)
                ALBUM_ERROR("Camera: could not create the preview texture (%ux%u): %s",
                            frame->w, frame->h, SDL_GetError());
            fails++;
        } else {
            SDL_SetTextureScaleMode(mCameraTexture, SDL_ScaleModeLinear);
        }
    }
    if (!mCameraTexture) return;

    if (SDL_UpdateTexture(mCameraTexture, nullptr, frame->pixels, frame->pitch) == -1) {
        mCamUploadErrors++;
        mCamUploadLastError = SDL_GetError();
        if (mCamUploadErrors == 1 || mCamUploadErrors % 60 == 0) {
            ALBUM_ERROR("Camera: texture upload failed (%d): %s",
                        (int)mCamUploadErrors, mCamUploadLastError.c_str());
        }
    } else {
        mCamUploadErrors = 0;
    }

    mCamFrameStart = SDL_GetTicks();
}

void Album::UpdateCamera(const Input& input) {
    // Pull the newest frame into the preview texture.
    SDL_Surface* frame = CameraRunning()
        ? Camera::AcquireFrame(mCameraMirror, true) : nullptr;
    if (frame) {
        mCameraHasFrame = true;
        UploadCameraFrame(frame);
    }

    // A finished clip only appears in the album once the toast has been read.
    Uint32 now = SDL_GetTicks();
    if (mCamRecordNotifEnd && now > mCamRecordNotifEnd) {
        mCamRecordNotifEnd = 0;
    }
    if (mCamNotifEnd && now > mCamNotifEnd) {
        mCamNotifEnd = 0;
    }

    // Expire the shutter flash.
    if (mCamFlashEnd && now > mCamFlashEnd) {
        mCamFlashEnd = 0;
    }

    if (CameraRunning() && now - mCamDiagLastTime >= 3000) {
        WriteCameraDiagnostics("periodic");
        mCamDiagLastTime = now;
    }

    const bool diagChord =
        input.IsHeld(Input::BUTTON_L) && input.IsHeld(Input::BUTTON_R) &&
        input.IsHeld(Input::BUTTON_X) && input.IsHeld(Input::BUTTON_A) &&
        input.IsHeld(Input::BUTTON_B);
    static bool diagChordWas = false;

    if (diagChord && !diagChordWas) {
        TriggerCameraHint(CAM_HINT_DIAG);
    } else if (!diagChord) {
        if (input.IsPressed(Input::BUTTON_B)) {
            ExitCameraMode();
        } else if (input.IsPressed(Input::BUTTON_R)) {
            if (!mPointerConsumedClick) TriggerCameraHint(CAM_HINT_SHUTTER);
        } else if (input.IsPressed(Input::BUTTON_X)) {
            if (CameraRunning()) TriggerCameraHint(CAM_HINT_FLIP);
            else                 TriggerCameraHint(CAM_HINT_FPS); // doubles as "Retry"
        } else if (input.IsPressed(Input::BUTTON_Y)) {
            TriggerCameraHint(CAM_HINT_GRID);
        } else if (input.IsPressed(Input::BUTTON_ZR) || input.IsPressed(Input::BUTTON_ZL)) {
            TriggerCameraHint(CAM_HINT_FPS);
        } else if (input.IsPressed(Input::BUTTON_L)) {
            ToggleCameraRecording();
        }
    }
    diagChordWas = diagChord;

    if (mCamRecording && frame)
        mRecorder.PushFrame((const uint8_t*)frame->pixels, frame->pitch);
}

void Album::TriggerCameraHint(int index) {
    switch (index) {
        case CAM_HINT_SHUTTER:
            CaptureCameraPhoto();
            break;

        case CAM_HINT_FLIP:
            if (!CameraRunning()) break;
            mCameraMirror       = !mCameraMirror;
            mSettingsCamMirror  = mCameraMirror;
            SaveConfig();
            if (mCameraHasFrame)
                UploadCameraFrame(Camera::AcquireFrame(mCameraMirror, true));
            break;

        case CAM_HINT_GRID:
            if (!CameraRunning()) break;
            mCameraGrid      = !mCameraGrid;
            mSettingsCamGrid = mCameraGrid;
            SaveConfig();
            break;

        case CAM_HINT_FPS:
            if (!CameraRunning()) {
                mCameraHasFrame = false;
                Camera::Close();
                ClearCameraThumbs();
                Camera::Open(mCameraInstance, mCameraFps >= 30);
                break;
            }
            mSettingsCamFps = (mSettingsCamFps >= 30) ? 15 : 30;
            mCameraFps     = mSettingsCamFps;
            SaveConfig();
            Camera::Close();
            mCameraHasFrame = false;
            Camera::Open(mCameraInstance, mCameraFps >= 30);
            mCamSessionStart = SDL_GetTicks();
            break;

        case CAM_HINT_BACK:
            ExitCameraMode();
            break;

        case CAM_HINT_DIAG:
            mCamDiagOverlay = !mCamDiagOverlay;
            break;

        case CAM_HINT_REC:
            if (CameraRunning()) ToggleCameraRecording();
            break;

        default: break;
    }
}

bool Album::HandleCameraTouch(int px, int py) {
    for (int i = 0; i < CAM_HINT_COUNT; i++) {
        const int* r = mCamHintRect[i];
        if (r[2] <= 0 || r[3] <= 0) continue;
        if (TouchHitRect(px, py, r[0], r[1], r[2], r[3])) {
            TriggerCameraHint(i);
            return true;
        }
    }
    return false;
}

void Album::DrawCameraSpinner(int cx, int cy, int radius, SDL_Color color, Uint32 time) {
    constexpr int DOTS = 12;
    int active = (int)((time / 80) % DOTS);
    for (int i = 0; i < DOTS; i++) {
        float angle = (float)i * 2.0f * 3.14159265f / DOTS;
        int dx = cx + (int)(std::cos(angle) * radius);
        int dy = cy + (int)(std::sin(angle) * radius);
        Uint8 alpha = (i == active) ? 0xff : 0x40;
        Gfx::DrawCircleFilled(dx, dy, (i == active) ? 7 : 5, {color.r, color.g, color.b, alpha});
    }
}

void Album::DrawCameraPreview(int px, int py, int pw, int ph) {
    const auto& th = Gfx::Theme();
    constexpr int RADIUS = 20;

    if (mCameraTexture) {
        Gfx::DrawTexture(mCameraTexture, px, py, pw, ph);

        // Rule-of-thirds guides.
        if (mCameraGrid) {
            for (int i = 1; i <= 2; i++) {
                int gx = px + pw * i / 3;
                int gy = py + ph * i / 3;
                Gfx::DrawRectFilled(gx - 1, py + 4, 2, ph - 8, {0xff, 0xff, 0xff, 0x3c});
                Gfx::DrawRectFilled(px + 4, gy - 1, pw - 8, 2, {0xff, 0xff, 0xff, 0x3c});
            }
        }

        MaskRoundedCorners(px, py, pw, ph, RADIUS, th.bg);

        Gfx::DrawRectRoundedOutline(px, py, pw, ph, RADIUS, th.separator, 2);
    } else {
        Gfx::DrawRectRounded(px, py, pw, ph, RADIUS, th.thumbPlaceholder);
        Gfx::DrawRectRoundedOutline(px, py, pw, ph, RADIUS, th.separator, 2);

        int cx = px + pw / 2;
        int cy = py + ph / 2;
        if (CameraRunning()) {
            DrawCameraSpinner(cx, cy - 40, 26, Gfx::COLOR_ACCENT, SDL_GetTicks());
            Gfx::Print(cx, cy + 30, 30, th.text,
                       "Waking up the camera...", Gfx::ALIGN_CENTER);
            Gfx::Print(cx, cy + 74, 24, th.textDim,
                       "The first frame can take a moment to arrive",
                       Gfx::ALIGN_CENTER);
        } else {
            int iw = 84, ih = 60;
            int lensY = cy - 50;
            int ix = cx - iw / 2, iy = lensY - ih / 2;
            Gfx::DrawRectRoundedOutline(ix, iy, iw, ih, 12, th.textDim, 4);
            Gfx::DrawRectFilled(ix + 14, iy - 10, 30, 10, th.textDim);
            Gfx::DrawCircleOutline(cx, lensY, 22, th.textDim, 4);

            const char* title = "No camera detected";
            Gfx::Print(cx, cy + 40, 34, th.text, title, Gfx::ALIGN_CENTER);

            std::string reason = Camera::GetStatusMessage();
            if (!reason.empty())
                Gfx::Print(cx, cy + 86, 24, th.textDim, reason, Gfx::ALIGN_CENTER);

            Gfx::Print(cx, cy + 140, 24, th.textDim,
                       "Using the " + std::string(mCameraInstance ? "USB camera" : "GamePad camera") + ". Close other apps",
                       Gfx::ALIGN_CENTER);
            Gfx::Print(cx, cy + 174, 24, th.textDim,
                       "that are using it, or pick a different source in",
                       Gfx::ALIGN_CENTER);
            Gfx::Print(cx, cy + 208, 24, th.textDim,
                       "Settings, then press X to try again.",
                       Gfx::ALIGN_CENTER);
        }
    }
}

void Album::DrawCameraSidePanel(int px, int py, int pw, int ph) {
    const auto& th = Gfx::Theme();

    Gfx::DrawRectRounded(px, py, pw, ph, 14, th.cardBg);
    Gfx::DrawRectRoundedOutline(px, py, pw, ph, 14, th.separator, 2);

    const int PAD   = 24;
    const int ix    = px + PAD;
    const int iw    = pw - PAD * 2;
    int       iy    = py + PAD;

    struct Stat { const char* label; std::string value; };
    float measured = Camera::GetMeasuredFps();
    Stat stats[5] = {
        {"Resolution", std::to_string(Camera::WIDTH) + " x " + std::to_string(Camera::HEIGHT)},
        {"Frame rate",  std::to_string(mCameraFps) + " fps" +
                        (measured > 1.0f ? "  (" + std::to_string((int)(measured + 0.5f)) + " seen)" : "")},
        {"Source",      mCameraInstance ? "USB / DLC camera" : "GamePad camera"},
        {"Photos",      std::to_string(mCamShotsTaken)},
        {"View",        mCameraMirror ? "Mirrored" : "Standard"},
    };

    constexpr int ROW_H = 38;
    for (int i = 0; i < 5; i++) {
        int ry = iy + i * ROW_H;
        if (i) Gfx::DrawRectFilled(ix, ry - 4, iw, 1, th.separator);
        Gfx::Print(ix, ry + ROW_H / 2, 22, th.textDim, stats[i].label,
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(ix + iw, ry + ROW_H / 2, 22,
                   ((i == 4 && mCameraMirror) || i == 2) ? Gfx::COLOR_ACCENT : th.text,
                   stats[i].value, Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
    }

    iy += ROW_H * 5 + 16;

    Gfx::Print(ix, iy, 24, th.text, "Recent shots", Gfx::ALIGN_LEFT);
    std::string countStr = std::to_string((int)mCamThumbs.size()) + "/" +
                           std::to_string(CAM_MAX_THUMBS);
    Gfx::Print(ix + iw, iy, 22, th.textDim, countStr, Gfx::ALIGN_RIGHT);
    iy += 36;

    if (!mCamThumbNames.empty()) {
        std::string latest = mCamThumbNames.back();
        size_t slash = latest.rfind('/');
        if (slash != std::string::npos) latest = latest.substr(slash + 1);
        Gfx::Print(ix, iy, 20, th.textDim, "Last: " + latest, Gfx::ALIGN_LEFT);
        iy += 30;
    }

    const int cellW = CAM_THUMB_W;
    const int cellH = CAM_THUMB_H;
    const int gridH = ph - (iy - py) - PAD;
    int rows = gridH / (cellH + CAM_THUMB_GAP);
    if (rows < 0) rows = 0;

    if (mCamThumbs.empty()) {
        int boxH = rows > 0 ? rows * (cellH + CAM_THUMB_GAP) - CAM_THUMB_GAP : cellH;
        Gfx::DrawRectRoundedOutline(ix, iy, iw, boxH, 12, th.separator, 2);
        Gfx::Print(ix + iw / 2, iy + boxH / 2 - 12, 24, th.textDim,
                   "Shots you take show up here", Gfx::ALIGN_CENTER);
        Gfx::Print(ix + iw / 2, iy + boxH / 2 + 20, 22, th.textDim,
                   "and are saved to the album", Gfx::ALIGN_CENTER);
    } else {
        int shown = 0;
        for (size_t i = 0; i < mCamThumbs.size() && shown < rows * CAM_THUMB_COLS; i++) {
            int col = shown % CAM_THUMB_COLS;
            int row = shown / CAM_THUMB_COLS;
            int tx  = ix + col * (cellW + CAM_THUMB_GAP);
            int ty  = iy + row * (cellH + CAM_THUMB_GAP);

            if (i + 1 == mCamThumbs.size()) {
                Gfx::DrawRectRoundedOutline(tx - 3, ty - 3, cellW + 6, cellH + 6, 10,
                                            Gfx::COLOR_ACCENT, 3);
            }
            Gfx::DrawRectFilled(tx, ty, cellW, cellH, th.thumbPlaceholder);
            Gfx::DrawTexture(mCamThumbs[i], tx, ty, cellW, cellH);
            MaskRoundedCorners(tx, ty, cellW, cellH, 8, th.cardBg);
            shown++;
        }

        if (rows * CAM_THUMB_COLS < (int)mCamThumbs.size()) {
            Gfx::Print(ix + iw, iy + rows * (cellH + CAM_THUMB_GAP) - CAM_THUMB_GAP,
                       20, th.textDim, "+ older shots",
                       Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
        }
    }
}

void Album::DrawCameraFooter() {
    const auto& th = Gfx::Theme();
    int fy = Gfx::SCREEN_HEIGHT - CAM_FOOTER_H;

    Gfx::DrawRectFilled(0, fy, Gfx::SCREEN_WIDTH, 1, th.separator);
    Gfx::DrawRectFilled(0, fy + 1, Gfx::SCREEN_WIDTH, CAM_FOOTER_H - 1, th.footerBg);

    for (int i = 0; i < CAM_HINT_COUNT; i++) {
        mCamHintRect[i][0] = 0;
        mCamHintRect[i][1] = 0;
        mCamHintRect[i][2] = 0;
        mCamHintRect[i][3] = 0;
    }

    constexpr int ICON_SZ = 28;
    constexpr int LBL_SZ  = 24;
    constexpr int GAP     = 8;
    constexpr int STEP    = 36;
    constexpr int PAD_Y   = 12;

    int cy = fy + CAM_FOOTER_H / 2;
    int cx = 30;

    struct Hint { int index; const char* glyph; SDL_Color color; const char* label; };
    std::string fpsLabel = std::to_string(mCameraFps) + " fps";

    std::vector<Hint> hints;
    if (CameraRunning()) {
        hints = {
            {CAM_HINT_SHUTTER, Glyphs::R.c_str(), Gfx::COLOR_ACCENT, "Shutter"},
            {CAM_HINT_FLIP,    Glyphs::X.c_str(), Gfx::COLOR_BTN_X, mCameraMirror ? "Unflip" : "Flip"},
            {CAM_HINT_GRID,    Glyphs::Y.c_str(), Gfx::COLOR_BTN_Y, mCameraGrid ? "Hide grid" : "Grid"},
            {CAM_HINT_FPS,     Glyphs::ZR.c_str(), Gfx::COLOR_ACCENT, fpsLabel.c_str()},
        };
    } else {
        hints = {
            {CAM_HINT_FPS, Glyphs::X.c_str(), Gfx::COLOR_BTN_X, "Retry"},
        };
    }
    hints.push_back({CAM_HINT_BACK, Glyphs::B.c_str(), Gfx::COLOR_BTN_B, "Back"});
    if (CameraRunning()) {
        hints.push_back({CAM_HINT_REC, Glyphs::L.c_str(),
                         mCamRecording ? Gfx::COLOR_BTN_B : Gfx::COLOR_ACCENT,
                         mCamRecording ? "Stop" : "Record"});
    }

    for (const auto& h : hints) {
        std::string label = h.label;
        Gfx::PrintIcon(cx, cy, ICON_SZ, h.color, h.glyph, Gfx::ALIGN_CENTER);

        int textX = cx + ICON_SZ / 2 + GAP;
        Gfx::Print(textX, cy, LBL_SZ, th.text, label, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

        int endX = textX + Gfx::GetTextWidth(LBL_SZ, label);
        mCamHintRect[h.index][0] = cx - 12;
        mCamHintRect[h.index][1] = fy + PAD_Y;
        mCamHintRect[h.index][2] = (endX - cx) + 24;
        mCamHintRect[h.index][3] = CAM_FOOTER_H - PAD_Y * 2;

        cx = endX + STEP;
    }

    int btnR = 34;
    int bx = Gfx::SCREEN_WIDTH - CAM_MARGIN - btnR;
    int by = fy + CAM_FOOTER_H / 2;

    mCamHintRect[CAM_HINT_SHUTTER][0] = bx - btnR - 10;
    mCamHintRect[CAM_HINT_SHUTTER][1] = by - btnR - 10;
    mCamHintRect[CAM_HINT_SHUTTER][2] = (btnR + 10) * 2;
    mCamHintRect[CAM_HINT_SHUTTER][3] = (btnR + 10) * 2;

    Gfx::DrawCircleFilled(bx, by, btnR + 8, th.sidebarSel);
    if (CameraRunning()) {
        Gfx::DrawCircleFilled(bx, by, btnR, Gfx::COLOR_WHITE);
        Gfx::DrawCircleOutline(bx, by, btnR, Gfx::COLOR_ACCENT, 4);
        Gfx::DrawCircleOutline(bx, by, btnR - 12, th.separator, 2);
    } else {
        Gfx::DrawCircleOutline(bx, by, btnR, th.separator, 3);
        Gfx::DrawCircleOutline(bx, by, btnR - 12, th.separator, 2);
    }
}

void Album::DrawCamera() {
    const auto& th = Gfx::Theme();
    Uint32 drawStart = SDL_GetTicks();

    Gfx::Clear(th.bg);
    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, CAM_HEADER_H, th.headerBg);
    Gfx::DrawRectFilled(0, CAM_HEADER_H - 1, Gfx::SCREEN_WIDTH, 1, th.separator);

    int tx = 30, ty = CAM_HEADER_H / 2;
    int iw = 32, ih = 24;
    int ix = tx, iy = ty - ih / 2;
    Gfx::DrawRectRounded(ix, iy, iw, ih, 6, th.text);
    Gfx::DrawRectFilled(ix + 9, iy - 5, 14, 5, th.text);
    Gfx::DrawCircleFilled(ix + iw / 2, iy + ih / 2, 7, th.headerBg);

    Gfx::Print(ix + iw + 16, ty, 30, th.text, "Camera",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    if (mCamShotsTaken > 0) {
        Gfx::Print(Gfx::SCREEN_WIDTH - 30, ty, 24, th.textDim,
                   std::to_string(mCamShotsTaken) + " photo" +
                   (mCamShotsTaken == 1 ? "" : "s") + " saved this session",
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
    }
    {
        std::string micText;
        SDL_Color micColor = th.textDim;
        if (mCamRecording) {
            micText  = mMicReady ? "Mic: recording" : "Mic: unavailable";
            micColor = mMicReady ? CAM_REC_RED : Gfx::COLOR_BTN_Y;
        } else {
        }
        int titleX = ix + iw + 16;
        Gfx::Print(titleX + Gfx::GetTextWidth(30, "Camera") + 36, ty, 22,
                   micColor, micText, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }

    const int panelW = 620;
    const int mainTop = CAM_HEADER_H + 14;
    const int mainH   = Gfx::SCREEN_HEIGHT - CAM_FOOTER_H - mainTop - 14;
    const int previewMaxW = Gfx::SCREEN_WIDTH - panelW - CAM_MARGIN * 3;

    int pw = previewMaxW;
    int ph = pw * Camera::HEIGHT / Camera::WIDTH;
    if (ph > mainH) {
        ph = mainH;
        pw = ph * Camera::WIDTH / Camera::HEIGHT;
    }
    int px = CAM_MARGIN + (previewMaxW - pw) / 2;
    int py = mainTop + (mainH - ph) / 2;

    DrawCameraPreview(px, py, pw, ph);

    int panelX = px + pw + 28;
    DrawCameraSidePanel(panelX, mainTop, panelW, mainH);

    DrawCameraFooter();

    if (mCamDiagOverlay) DrawCameraDiagnostics();

    if (mCamFlashEnd) {
        Uint32 now = SDL_GetTicks();
        Uint32 dur = mCamFlashEnd - mCamFlashStart;
        if (dur > 0 && now >= mCamFlashStart && now < mCamFlashEnd) {
            float t = (float)(now - mCamFlashStart) / (float)dur;
            Uint8 alpha = (Uint8)(255.0f * (1.0f - t) * (1.0f - t));
            Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT,
                                {0xff, 0xff, 0xff, alpha});
        }
    }

    bool        showRecToast = mCamRecordNotifEnd && SDL_GetTicks() < mCamRecordNotifEnd;
    const char* toastText    = showRecToast ? mCamRecordNotifText.c_str() : mCamNotifText.c_str();
    bool        toastError   = showRecToast ? mCamRecordNotifErr : mCamNotifError;

    if ((showRecToast || (mCamNotifEnd && SDL_GetTicks() < mCamNotifEnd)) &&
        toastText && toastText[0]) {
        int bw = 620, bh = 76;
        int bx = (Gfx::SCREEN_WIDTH - bw) / 2;
        int by = Gfx::SCREEN_HEIGHT - CAM_FOOTER_H - bh - 28;

        Gfx::DrawRectFilled(bx + 4, by + 5, bw, bh, {0, 0, 0, 0x50});
        Gfx::DrawRectRounded(bx, by, bw, bh, 14, th.cardBg);
        Gfx::DrawRectRoundedOutline(bx, by, bw, bh, 14,
                                    toastError ? Gfx::COLOR_DELETE : CAM_OK, 3);

        int iconX = bx + 40, iconY = by + bh / 2;
        if (toastError) {
            Gfx::DrawCircleOutline(iconX, iconY, 14, Gfx::COLOR_DELETE, 4);
            Gfx::DrawRectFilled(iconX - 2, iconY - 8, 4, 10, Gfx::COLOR_DELETE);
            Gfx::DrawRectFilled(iconX - 2, iconY + 5, 4, 4, Gfx::COLOR_DELETE);
        } else {
            Gfx::DrawCircleOutline(iconX, iconY, 15, CAM_OK, 4);
            Gfx::DrawLine(iconX - 7, iconY, iconX - 2, iconY + 6, CAM_OK);
            Gfx::DrawLine(iconX - 2, iconY + 6, iconX + 8, iconY - 7, CAM_OK);
        }

        Gfx::Print(bx + 72, by + bh / 2, 24, th.text, toastText,
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }

    if (mCamRecording) {
        int now = (int)SDL_GetTicks();
        std::string recTime = FormatSessionTime((Uint32)(now - (int)mCamRecordStart));

        int bw = 470, bh = 62;
        int bx = px + pw / 2 - bw / 2;
        int by = py + 22;

        Gfx::DrawRectFilled(bx + 3, by + 4, bw, bh, {0, 0, 0, 0x60});
        Gfx::DrawRectRounded(bx, by, bw, bh, 31, {0x00, 0x00, 0x00, 0xC0});
        Gfx::DrawRectRoundedOutline(bx, by, bw, bh, 31, CAM_REC_RED, 3);

        bool on = ((now / 250) % 2) == 0;
        if (on) Gfx::DrawCircleFilled(bx + 34, by + bh / 2, 11, CAM_REC_RED);
        Gfx::Print(bx + 58, by + bh / 2, 26, Gfx::COLOR_WHITE, "REC",
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(bx + 118, by + bh / 2, 26, Gfx::COLOR_WHITE, recTime,
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

        std::string stats = std::to_string(mRecorder.GetFrameCount()) + "f  " +
                            std::to_string((unsigned)(mRecorder.GetBytesWritten() / 1024)) + "KB";
        Gfx::Print(bx + bw - 22, by + bh / 2, 22, {0xdd, 0xdd, 0xdd, 0xff}, stats,
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
    }

    if (mPointerDraw) {
        Gfx::DrawCircleFilled(mPointerScreenX, mPointerScreenY, 10, Gfx::COLOR_WHITE);
        Gfx::DrawCircleOutline(mPointerScreenX, mPointerScreenY, 12, Gfx::COLOR_BLACK, 2);
    }

    Uint32 drawMs = SDL_GetTicks() - drawStart;
    Uint32 frameMs = mCamFrameStart ? (drawStart - mCamFrameStart) + drawMs : 0;
    RecordCameraFrameTiming(frameMs, drawMs);
}

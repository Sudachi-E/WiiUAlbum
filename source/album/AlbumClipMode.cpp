#include "Album.hpp"
#include "../ui/Glyphs.hpp"
#include <sys/stat.h>
#include <cstdio>
#include <whb/log.h>
#include <coreinit/debug.h>

void Album::EnterClipMode() {
    if (mViewerItem < 0 || mViewerItem >= (int)mFiltered.size()) return;
    int idx = mFiltered[mViewerItem];
    if (mAllItems[idx].type != MediaType::Video) return;

    double dur = mVideoDecoder.GetDuration();
    if (dur <= 0) return;

    mClipStartTime = 0.0;
    mClipEndTime   = dur;
    mClipMode      = true;
    mClipPreviewPlaying = false;
    mClipSaving    = false;
    mClipSaveNotifEndTime = 0;
    mClipProgress  = 0.0;

    if (mVideoPlaying) {
        mVideoPaused = true;
        mVideoDecoder.PauseAudio(true);
    }

    mVideoDecoder.Seek(0.0);
    for (int attempts = 0; attempts < 50; attempts++) {
        if (mVideoDecoder.GetVideoQueueSize() > 0) {
            mVideoDecoder.ReadFrame(mVideoTexture);
            if (mVideoDecoder.GetCurrentTime() > 0.0) break;
        }
        SDL_Delay(10);
    }

    WHBLogPrintf("[ALBUM] EnterClipMode: duration=%.1f start=%.1f end=%.1f", dur, mClipStartTime, mClipEndTime);
}

void Album::ExitClipMode() {
    mClipMode = false;
    mClipPreviewPlaying = false;
    mClipSaving = false;
    mClipSaveNotifEndTime = 0;

    if (mViewerState == ViewerState::Open && mVideoDecoder.HasVideo()) {
        mVideoPlaying = true;
        mVideoPaused = false;
        mWallClockStartTime = SDL_GetTicks();
        mWallClockStartPTS = mVideoDecoder.GetCurrentTime();
        mVideoDecoder.PauseAudio(false);
    }

    WHBLogPrintf("[ALBUM] ExitClipMode");
}

void Album::UpdateClipMode(const Input& input) {
    if (mClipSaveNotifEndTime > 0 && SDL_GetTicks() >= mClipSaveNotifEndTime) {
        mClipSaveNotifEndTime = 0;
        ExitClipMode();
        CloseViewer();
        return;
    }

    if (mClipSaving) {
        if (SDL_AtomicGet(&mClipEncodingDone)) {
            mClipSaving = false;
            mClipProgress = 1.0f;
            if (mClipEncodeSuccess) {
                mClipSaveNotifEndTime = SDL_GetTicks() + 3000;
                mPendingRefresh = true;
            } else {
                mClipSaveNotifEndTime = SDL_GetTicks() + 4000;
            }
        }
        return;
    }

    double dur = mVideoDecoder.GetDuration();
    if (dur <= 0) return;

    // Preview playback
    if (mClipPreviewPlaying) {
        if (input.IsPressed(Input::BUTTON_A)) {
            mClipPreviewPlaying = false;
            mVideoDecoder.PauseAudio(true);
            mVideoPaused = true;
            mVideoPlaying = false;
            return;
        }
        if (mVideoTexture) {
            double videoPTS = mVideoDecoder.GetCurrentTime();
            if (videoPTS >= mClipEndTime - 0.05) {
                mClipPreviewPlaying = false;
                mVideoDecoder.PauseAudio(true);
                mVideoPaused = true;
                mVideoPlaying = false;
                return;
            }
            Uint32 now = SDL_GetTicks();
            double elapsed = (now - mWallClockStartTime) / 1000.0;
            double expectedPTS = mWallClockStartPTS + elapsed;
            double drift = videoPTS - expectedPTS;
            if (drift < -0.1) {
                int framesToSkip = (drift < -0.3) ? 3 : 1;
                for (int i = 0; i < framesToSkip; i++)
                    mVideoDecoder.ReadFrame(nullptr);
                mVideoDecoder.ReadFrame(mVideoTexture);
            } else if (drift < mFrameDelay / 1000.0) {
                mVideoDecoder.ReadFrame(mVideoTexture);
            }
        }
        return;
    }

    // D-pad L/R — fine adjust active marker
    if (input.IsPressed(Input::BUTTON_LEFT) || input.IsPressed(Input::BUTTON_RIGHT)) {
        double step = input.IsPressed(Input::BUTTON_LEFT) ? (-1.0 / 30.0) : (1.0 / 30.0);
        if (mClipMarkerEndActive) {
            double newEnd = mClipEndTime + step;
            if (newEnd > dur) newEnd = dur;
            if (newEnd <= mClipStartTime + 0.5) newEnd = mClipStartTime + 0.5;
            mClipEndTime = newEnd;
            mVideoDecoder.Seek(mClipEndTime);
        } else {
            double newStart = mClipStartTime + step;
            if (newStart < 0) newStart = 0;
            if (newStart >= mClipEndTime - 0.5) newStart = mClipEndTime - 0.5;
            mClipStartTime = newStart;
            mVideoDecoder.Seek(mClipStartTime);
        }
        for (int i = 0; i < 30; i++) {
            if (mVideoDecoder.GetVideoQueueSize() > 0) {
                mVideoDecoder.ReadFrame(mVideoTexture);
                break;
            }
            SDL_Delay(5);
        }
        mVideoDecoder.PauseAudio(true);
    }

    // Left stick — adjust start time
    float lx = input.GetLeftStickX();
    if (lx > 0.3f || lx < -0.3f) {
        mClipMarkerEndActive = false;
        double step = lx * lx * lx * 2.0;
        double newStart = mClipStartTime + step;
        if (newStart < 0) newStart = 0;
        if (newStart >= mClipEndTime - 0.5) newStart = mClipEndTime - 0.5;
        if (newStart != mClipStartTime) {
            mClipStartTime = newStart;
            mVideoDecoder.Seek(mClipStartTime);
            for (int i = 0; i < 30; i++) {
                if (mVideoDecoder.GetVideoQueueSize() > 0) {
                    mVideoDecoder.ReadFrame(mVideoTexture);
                    break;
                }
                SDL_Delay(5);
            }
            mVideoDecoder.PauseAudio(true);
        }
    }

    // Right stick — adjust end time
    float rx = input.GetRightStickX();
    if (rx > 0.3f || rx < -0.3f) {
        mClipMarkerEndActive = true;
        double step = rx * rx * rx * 2.0;
        double newEnd = mClipEndTime + step;
        if (newEnd > dur) newEnd = dur;
        if (newEnd <= mClipStartTime + 0.5) newEnd = mClipStartTime + 0.5;
        if (newEnd != mClipEndTime) {
            mClipEndTime = newEnd;
            mVideoDecoder.Seek(mClipEndTime);
            for (int i = 0; i < 30; i++) {
                if (mVideoDecoder.GetVideoQueueSize() > 0) {
                    mVideoDecoder.ReadFrame(mVideoTexture);
                    break;
                }
                SDL_Delay(5);
            }
            mVideoDecoder.PauseAudio(true);
        }
    }

    if (input.IsPressed(Input::BUTTON_A)) {
        mVideoDecoder.Seek(mClipStartTime);
        mClipPreviewPlaying = true;
        mVideoPlaying = true;
        mVideoPaused = false;
        mWallClockStartTime = SDL_GetTicks();
        mWallClockStartPTS = mClipStartTime;
        mVideoDecoder.PauseAudio(false);
    }

    if (input.IsPressed(Input::BUTTON_Y)) {
        SaveClip();
    }

    if (input.IsPressed(Input::BUTTON_B)) {
        ExitClipMode();
    }
}

void Album::DrawClipMode() {
    if (mViewerState == ViewerState::None) return;

    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT,
                        {0x10, 0x10, 0x10, 0xff});

    double dur = mVideoDecoder.GetDuration();
    if (dur <= 0) dur = 1.0;

    int videoAreaH = (int)(Gfx::SCREEN_HEIGHT * 0.55f);
    if (mVideoTexture) {
        int texW = mVideoDecoder.GetWidth();
        int texH = mVideoDecoder.GetHeight();
        if (texW > 0 && texH > 0) {
            float scaleX = (float)Gfx::SCREEN_WIDTH / texW;
            float scaleY = (float)videoAreaH / texH;
            float scale = (scaleX < scaleY) ? scaleX : scaleY;
            int drawW = (int)(texW * scale);
            int drawH = (int)(texH * scale);
            int drawX = (Gfx::SCREEN_WIDTH - drawW) / 2;
            int drawY = (videoAreaH - drawH) / 2;
            SDL_Rect src = {0, 0, texW, texH};
            SDL_Rect dst = {drawX, drawY, drawW, drawH};
            SDL_RenderCopy(Gfx::GetRenderer(), mVideoTexture, &src, &dst);
        }
    }

    int barY = videoAreaH + 50;
    int barX = 100;
    int barW = Gfx::SCREEN_WIDTH - 200;
    int barH = 16;

    Gfx::DrawRectRounded(barX, barY, barW, barH, 8, {0x3c, 0x3c, 0x3c, 0xff});

    if (mClipEndTime > mClipStartTime) {
        int hlX = barX + (int)(barW * (mClipStartTime / dur));
        int hlW = barX + (int)(barW * (mClipEndTime / dur)) - hlX;
        if (hlW > 0) {
            Gfx::DrawRectRounded(hlX, barY, hlW, barH, 8, {0x00, 0x9a, 0xc7, 0xff});
        }
    }

    double curTime = mVideoDecoder.GetCurrentTime();
    if (curTime >= 0) {
        int posX = barX + (int)(barW * (curTime / dur));
        Gfx::DrawRectFilled(posX - 2, barY - 4, 4, barH + 8, {0xff, 0xff, 0xff, 0xff});
    }

    int startMX = barX + (int)(barW * (mClipStartTime / dur));
    Gfx::DrawRectFilled(startMX - 3, barY - 8, 6, barH + 16, Gfx::COLOR_BTN_Y);

    int endMX = barX + (int)(barW * (mClipEndTime / dur));
    Gfx::DrawRectFilled(endMX - 3, barY - 8, 6, barH + 16, Gfx::COLOR_BTN_X);

    char startStr[32], endStr[32];
    int sM = (int)mClipStartTime / 60, sS = (int)mClipStartTime % 60;
    int eM = (int)mClipEndTime / 60,   eS = (int)mClipEndTime % 60;
    snprintf(startStr, sizeof(startStr), "%d:%02d", sM, sS);
    snprintf(endStr,   sizeof(endStr),   "%d:%02d", eM, eS);

    Gfx::Print(startMX, barY + barH + 12, 22, Gfx::COLOR_BTN_Y, startStr, Gfx::ALIGN_HORIZONTAL | Gfx::ALIGN_TOP);
    Gfx::Print(endMX, barY + barH + 12, 22, Gfx::COLOR_BTN_X, endStr, Gfx::ALIGN_HORIZONTAL | Gfx::ALIGN_TOP);


    char clipStr[64];
    int cM = (int)(mClipEndTime - mClipStartTime) / 60;
    int cS = (int)(mClipEndTime - mClipStartTime) % 60;
    snprintf(clipStr, sizeof(clipStr), "Clip: %d:%02d", cM, cS);
    Gfx::Print(Gfx::SCREEN_WIDTH / 2, barY + barH + 44, 24, Gfx::COLOR_WHITE,
               clipStr, Gfx::ALIGN_CENTER);

    if (mClipSaving) {
        Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT,
                            {0, 0, 0, 160});
        Gfx::Print(Gfx::SCREEN_WIDTH / 2, Gfx::SCREEN_HEIGHT / 2 - 20, 32,
                   Gfx::COLOR_WHITE, "Saving clip...", Gfx::ALIGN_CENTER);
        int pBarX = Gfx::SCREEN_WIDTH / 2 - 150;
        int pBarY = Gfx::SCREEN_HEIGHT / 2 + 20;
        int pBarW = 300, pBarH = 20;
        Gfx::DrawRectRounded(pBarX, pBarY, pBarW, pBarH, 10, {0x3c, 0x3c, 0x3c, 0xff});
        Gfx::DrawRectRounded(pBarX, pBarY,
                             (int)(pBarW * mClipProgress), pBarH,
                             10, Gfx::COLOR_ACCENT);
        return;
    }

    if (mClipSaveNotifEndTime > 0 && SDL_GetTicks() < mClipSaveNotifEndTime) {
        bool isError = !mClipEncodeSuccess;
        int nw = 400, nh = 100;
        int nx = (Gfx::SCREEN_WIDTH - nw) / 2;
        int ny = (Gfx::SCREEN_HEIGHT - nh) / 2;
        Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT,
                            {0, 0, 0, 100});
        Gfx::DrawRectRounded(nx, ny, nw, nh, 12, {0xff, 0xff, 0xff, 0xff});
        Gfx::DrawRectRoundedOutline(nx, ny, nw, nh, 12,
                                    isError ? Gfx::COLOR_DELETE : Gfx::COLOR_ACCENT, 2);
        Gfx::Print(nx + nw / 2, ny + nh / 2, 30,
                   isError ? Gfx::COLOR_DELETE : Gfx::COLOR_TEXT,
                   isError ? "Clip failed!" : "Clip saved!", Gfx::ALIGN_CENTER);
        return;
    }

    int hintY = Gfx::SCREEN_HEIGHT - 50;
    constexpr int IZ = 28;
    constexpr int LZ = 24;
    constexpr int IG = 8;
    Gfx::PrintIcon(60, hintY, IZ, Gfx::COLOR_WHITE, Glyphs::STICK_L, Gfx::ALIGN_CENTER);
    Gfx::Print(60 + IZ / 2 + IG, hintY, LZ, Gfx::COLOR_WHITE,
               "Start", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    Gfx::PrintIcon(420, hintY, IZ, Gfx::COLOR_WHITE, Glyphs::STICK_R, Gfx::ALIGN_CENTER);
    Gfx::Print(420 + IZ / 2 + IG, hintY, LZ, Gfx::COLOR_WHITE,
               "End", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    int bx = Gfx::SCREEN_WIDTH - 390;
    Gfx::PrintIcon(bx, hintY, IZ, Gfx::COLOR_BTN_A, Glyphs::A, Gfx::ALIGN_CENTER);
    Gfx::Print(bx + IZ / 2 + IG, hintY, LZ, Gfx::COLOR_WHITE, "Preview",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    bx = Gfx::SCREEN_WIDTH - 250;
    Gfx::PrintIcon(bx, hintY, IZ, Gfx::COLOR_BTN_Y, Glyphs::Y, Gfx::ALIGN_CENTER);
    Gfx::Print(bx + IZ / 2 + IG, hintY, LZ, Gfx::COLOR_WHITE, "Save",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    bx = Gfx::SCREEN_WIDTH - 130;
    Gfx::PrintIcon(bx, hintY, IZ, Gfx::COLOR_BTN_B, Glyphs::B, Gfx::ALIGN_CENTER);
    Gfx::Print(bx + IZ / 2 + IG, hintY, LZ, Gfx::COLOR_WHITE, "Back",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    if (mPointerDraw) {
        Gfx::DrawCircleFilled(mPointerScreenX, mPointerScreenY, 10, Gfx::COLOR_WHITE);
        Gfx::DrawCircleOutline(mPointerScreenX, mPointerScreenY, 12, Gfx::COLOR_BLACK, 2);
    }
}

void Album::SaveClip() {
    if (mViewerItem < 0 || mViewerItem >= (int)mFiltered.size()) return;
    int idx = mFiltered[mViewerItem];
    if (mAllItems[idx].type != MediaType::Video) return;

    double dur = mVideoDecoder.GetDuration();
    if (dur <= 0 || mClipEndTime <= mClipStartTime) return;

    std::string baseName = mAllItems[idx].filename;
    size_t dot = baseName.rfind('.');
    if (dot != std::string::npos) baseName = baseName.substr(0, dot);

    mClipOutputPath = mPathVideos + "/" + baseName + "_clip.mov";
    struct stat st;
    int counter = 1;
    while (stat(mClipOutputPath.c_str(), &st) == 0) {
        mClipOutputPath = mPathVideos + "/" + baseName + "_clip_" + std::to_string(counter) + ".mov";
        counter++;
    }

    mClipSaving = true;
    mClipProgress = 0.0f;
    mClipPreviewPlaying = false;
    mClipEncodeSuccess = false;
    mClipErrorMsg.clear();
    SDL_AtomicSet(&mClipEncodingDone, 0);

    std::string inputPath = mAllItems[idx].path;
    double start = mClipStartTime;
    double end   = mClipEndTime;
    std::string outPath = mClipOutputPath;

    if (mClipThread.joinable()) mClipThread.join();
    mClipThread = std::thread([this, inputPath, outPath, start, end]() {
        bool ok = false;
        if (mClipEncoder.Open(inputPath, outPath, start, end)) {
            ok = mClipEncoder.Process();
            mClipEncoder.Close();
        } else {
            mClipErrorMsg = "Failed to open output file: " + outPath;
            OSReport("[ALBUM] %s\n", mClipErrorMsg.c_str());
        }
        if (!ok && mClipErrorMsg.empty()) {
            mClipErrorMsg = "Encoding failed";
            OSReport("[ALBUM] Clip encoding failed\n");
        }
        mClipOutputPath = mClipEncoder.GetOutputPath();
        mClipProgress = 1.0f;
        if (ok) mClipEncodeSuccess = true;
        SDL_AtomicSet(&mClipEncodingDone, 1);
    });

    WHBLogPrintf("[ALBUM] SaveClip: %s (%.1f -> %.1f)", mClipOutputPath.c_str(), start, end);
}

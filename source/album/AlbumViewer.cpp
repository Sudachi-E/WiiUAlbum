#include "Album.hpp"
#include "../ui/Glyphs.hpp"
#include <sys/stat.h>
#include <cstdio>
#include <ctime>
#include <SDL_image.h>
#include <whb/log.h>
#include <coreinit/debug.h>

void Album::OpenViewer(int filteredIdx) {
    if (filteredIdx < 0 || filteredIdx >= (int)mFiltered.size()) return;
    if (mMultiSelect) ExitMultiSelect(true);
    mViewerItem  = filteredIdx;
    mViewerState = ViewerState::Open;
    mViewerShowUI = true;
    mViewZoom    = 1.0f;
    mViewPanX    = 0.0f;
    mViewPanY    = 0.0f;

    int idx = mFiltered[filteredIdx];
    if (mAllItems[idx].type == MediaType::Screenshot) {
        if (mViewerTex) { Gfx::DestroyTexture(mViewerTex); mViewerTex = nullptr; }
        mViewerTex = Gfx::LoadTexture(mAllItems[idx].path);
    } else if (mAllItems[idx].type == MediaType::Video) {
        OpenVideoItem(filteredIdx);
    }
}

void Album::CloseViewer() {
    if (mViewerTex) { Gfx::DestroyTexture(mViewerTex); mViewerTex = nullptr; }
    if (mVideoTexture) { SDL_DestroyTexture(mVideoTexture); mVideoTexture = nullptr; }
    mVideoDecoder.Close();
    mVideoPlaying = false;
    mVideoPaused = false;
    mViewerState = ViewerState::None;

    if (mViewerItem >= 0 && mViewerItem < (int)mFiltered.size()) {
        mGridCursor = mViewerItem;
        int curRow = mGridCursor / COLS;
        if (curRow < mScrollRow) mScrollRow = curRow;
        if (curRow >= mScrollRow + ROWS_VIS) mScrollRow = curRow - ROWS_VIS + 1;
    }
    mViewerItem = -1;

    if (mPendingRefresh) {
        mPendingRefresh = false;
        Refresh();
    }
}

// Single-item delete
void Album::ExecuteDelete() {
    if (mViewerItem < 0 || mViewerItem >= (int)mFiltered.size()) return;
    int idx = mFiltered[mViewerItem];
    auto& item = mAllItems[idx];

    remove(item.path.c_str());
    OSReport("[ALBUM] Deleted: %s", item.path.c_str());

    StopThumbWorkers();

    if (item.thumbnail)      { Gfx::DestroyTexture(item.thumbnail); }
    if (item.pendingSurface) { SDL_FreeSurface(item.pendingSurface); }
    if (mViewerTex)          { Gfx::DestroyTexture(mViewerTex); mViewerTex = nullptr; }
    if (mVideoTexture)       { SDL_DestroyTexture(mVideoTexture); mVideoTexture = nullptr; }
    mVideoDecoder.Close();
    mVideoPlaying = false;
    mVideoPaused = false;

    mAllItems.erase(mAllItems.begin() + idx);

    for (auto& mi : mAllItems)
        if (mi.thumbRequested && !mi.thumbnail) mi.thumbRequested = false;

    StartThumbWorkers();

    ApplyFilterSort();
    int total = (int)mFiltered.size();
    if (total == 0) {
        CloseViewer();
    } else {
        if (mViewerItem >= total) mViewerItem = total - 1;
        NavigateToItem(mViewerItem, true);
    }
}

// Save video frame as screenshot
void Album::SaveScreenshot() {
    if (mViewerItem < 0 || mViewerItem >= (int)mFiltered.size()) return;
    int idx = mFiltered[mViewerItem];
    if (mAllItems[idx].type != MediaType::Video) return;

    SDL_Surface* frame = mVideoDecoder.GetCurrentFrameAsSurface();
    if (!frame) return;

    std::string baseName = mAllItems[idx].filename;
    size_t dot = baseName.rfind('.');
    if (dot != std::string::npos) baseName = baseName.substr(0, dot);

    std::string outPath = mPathScreenshots + "/" + baseName + "_frame.png";

    struct stat st;
    int counter = 1;
    while (stat(outPath.c_str(), &st) == 0) {
        outPath = mPathScreenshots + "/" + baseName + "_frame_" + std::to_string(counter) + ".png";
        counter++;
    }

    bool ok = (IMG_SavePNG(frame, outPath.c_str()) == 0);
    SDL_FreeSurface(frame);

    if (ok) {
        OSReport("[ALBUM] Screenshot saved: %s", outPath.c_str());
        mSaveNotifEndTime = SDL_GetTicks() + 2000;
        mPendingRefresh = true;
    } else {
        OSReport("[ALBUM] Failed to save screenshot: %s", outPath.c_str());
    }
}

void Album::UpdateViewer(const Input& input) {
    int idx = (mViewerItem >= 0 && mViewerItem < (int)mFiltered.size()) ? mFiltered[mViewerItem] : -1;
    bool isVideo = (idx >= 0 && mAllItems[idx].type == MediaType::Video);

    if (mViewerConfirmDelete) {
        if (input.IsPressed(Input::BUTTON_LEFT) || input.IsPressed(Input::BUTTON_RIGHT)) {
            mOverlaySel = 1 - mOverlaySel;
        }
        if (input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) {
            mViewerConfirmDelete = false;
            if (mOverlaySel == 0) {
                ExecuteDelete();
            } else {
                if (isVideo && mVideoPlaying && !mClipMode) {
                    mVideoDecoder.PauseAudio(false);
                    mWallClockStartTime = SDL_GetTicks();
                    mWallClockStartPTS = mVideoDecoder.GetCurrentTime();
                }
            }
        }
        if (input.IsPressed(Input::BUTTON_B)) {
            mViewerConfirmDelete = false;
            if (isVideo && mVideoPlaying && !mClipMode) {
                mVideoDecoder.PauseAudio(false);
                mWallClockStartTime = SDL_GetTicks();
                mWallClockStartPTS = mVideoDecoder.GetCurrentTime();
            }
        }
        return;
    }

    if (mSaveNotifEndTime > 0) {
        if (SDL_GetTicks() >= mSaveNotifEndTime)
            mSaveNotifEndTime = 0;
        else if ((input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) || input.IsPressed(Input::BUTTON_B) ||
                 input.IsPressed(Input::BUTTON_Y))
            mSaveNotifEndTime = 0;
    }

    if (input.IsPressed(Input::BUTTON_PLUS)) {
        mViewerShowUI = !mViewerShowUI;
    }

    // Side panel
    if (mViewerSidePanel) {
        int numOpts = isVideo ? 4 : 3;
        if (input.IsPressed(Input::BUTTON_DOWN)) mViewerSidePanelSel = (mViewerSidePanelSel + 1) % numOpts;
        if (input.IsPressed(Input::BUTTON_UP))   mViewerSidePanelSel = (mViewerSidePanelSel + numOpts - 1) % numOpts;
        if (input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) {
            if (isVideo && mViewerSidePanelSel == 0) {
                EnterClipMode();
                mViewerSidePanel = false;
            } else if (isVideo && mViewerSidePanelSel == 1) {
                SaveScreenshot();
            } else if ((isVideo && mViewerSidePanelSel == 2) || (!isVideo && mViewerSidePanelSel == 1)) {
                mPendingTransferIdx = mViewerItem;
                CloseViewer();
                mOverlaySel = 0;
                mOverlay = Overlay::TransferMode;
                mViewerSidePanel = false;
            } else if (!isVideo && mViewerSidePanelSel == 0) {
                mViewerSidePanel = false;
                EnterTextOverlay();
            } else {
                mViewerConfirmDelete = true;
                mOverlaySel = 1;
                mViewerSidePanel = false;
            }
        }
        if (input.IsPressed(Input::BUTTON_B) || input.IsPressed(Input::BUTTON_Y)) {
            mViewerSidePanel = false;
        }
        if (!mViewerSidePanel && isVideo && mVideoPlaying && !mClipMode) {
            mVideoDecoder.PauseAudio(false);
            mWallClockStartTime = SDL_GetTicks();
            mWallClockStartPTS = mVideoDecoder.GetCurrentTime();
        }
        return;
    }

    if (input.IsPressed(Input::BUTTON_Y)) {
        mViewerSidePanel = true;
        mViewerSidePanelSel = 0;
        if (isVideo && mVideoPlaying) {
            mVideoDecoder.PauseAudio(true);
        }
    }

    if (isVideo) {
        // Video playback controls
        if (mVideoPlaying && !mVideoPaused) {
            float rx = input.GetRightStickX();
            if (rx > 0.3f || rx < -0.3f) {
                mSeekAccum += rx * rx * rx * 2.0;
            } else {
                mSeekAccum = 0.0;
            }
            if (fabs(mSeekAccum) >= 1.0) {
                double newTime = mVideoDecoder.GetCurrentTime() + mSeekAccum;
                if (newTime < 0) newTime = 0;
                double dur = mVideoDecoder.GetDuration();
                if (dur > 0 && newTime > dur) newTime = dur;
                mVideoDecoder.Seek(newTime);
                mWallClockStartTime = 0;
                mSeekAccum = 0.0;
            }

            int cur   = mViewerItem;
            int total = (int)mFiltered.size();
            int next  = -1;
            if (input.IsPressed(Input::BUTTON_RIGHT) || input.IsPressed(Input::BUTTON_R)) next = (cur + 1) % total;
            if (input.IsPressed(Input::BUTTON_LEFT)  || input.IsPressed(Input::BUTTON_L))  next = (cur - 1 + total) % total;
            if (next >= 0 && next != cur) {
                NavigateToItem(next, false);
                return;
            }
            if (input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) {
                mVideoPaused = true;
                mVideoDecoder.PauseAudio(true);
            }
            if (input.IsPressed(Input::BUTTON_B)) {
                CloseViewer();
            }
            UpdateVideoPlayback();
        } else {
            int cur   = mViewerItem;
            int total = (int)mFiltered.size();
            int next  = -1;
            if (input.IsPressed(Input::BUTTON_RIGHT) || input.IsPressed(Input::BUTTON_R)) next = (cur + 1) % total;
            if (input.IsPressed(Input::BUTTON_LEFT)  || input.IsPressed(Input::BUTTON_L))  next = (cur - 1 + total) % total;
            if (next >= 0 && next != cur) {
                NavigateToItem(next, false);
                return;
            }
            if (input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) {
                mVideoPlaying = true;
                mVideoPaused = false;
                mVideoDecoder.PauseAudio(false);
                mWallClockStartTime = SDL_GetTicks();
                mWallClockStartPTS = mVideoDecoder.GetCurrentTime();
            }
            if (input.IsPressed(Input::BUTTON_B)) {
                CloseViewer();
            }
        }
        if (input.IsPressed(Input::BUTTON_X)) {
            mViewerConfirmDelete = true;
            mOverlaySel = 1;
            if (mVideoPlaying) {
                mVideoDecoder.PauseAudio(true);
            }
        }
    } else {
        // Image viewer controls

        if (mViewZoom <= ZOOM_MIN + 0.01f) {
            int cur   = mViewerItem;
            int total = (int)mFiltered.size();
            int next  = -1;
            if (input.IsPressed(Input::BUTTON_RIGHT) || input.IsPressed(Input::BUTTON_R)) next = (cur + 1) % total;
            if (input.IsPressed(Input::BUTTON_LEFT)  || input.IsPressed(Input::BUTTON_L))  next = (cur - 1 + total) % total;
            if (next >= 0) {
                NavigateToItem(next, true);
                return;
            }
        }

        float ry = input.GetRightStickY();
        if (ry != 0.f) {
            mViewZoom += ry * ZOOM_STEP;
            if (mViewZoom < ZOOM_MIN) mViewZoom = ZOOM_MIN;
            if (mViewZoom > ZOOM_MAX) mViewZoom = ZOOM_MAX;
        }

        if (mViewZoom > ZOOM_MIN + 0.01f) {
            float lx = input.GetLeftStickX();
            float ly = input.GetLeftStickY();
            mViewPanX -= lx * PAN_SPEED * mViewZoom;
            mViewPanY += ly * PAN_SPEED * mViewZoom;

            if (mViewerTex) {
                int texW = 0, texH = 0;
                SDL_QueryTexture(mViewerTex, nullptr, nullptr, &texW, &texH);

                float scaleX = (float)Gfx::SCREEN_WIDTH  / texW;
                float scaleY = (float)Gfx::SCREEN_HEIGHT / texH;
                float scale  = (scaleX < scaleY ? scaleX : scaleY) * mViewZoom;
                float drawW  = texW * scale;
                float drawH  = texH * scale;

                float maxPanX = (drawW  > Gfx::SCREEN_WIDTH)  ? (drawW  - Gfx::SCREEN_WIDTH)  * 0.5f : 0.f;
                float maxPanY = (drawH > Gfx::SCREEN_HEIGHT) ? (drawH - Gfx::SCREEN_HEIGHT) * 0.5f : 0.f;
                if (mViewPanX >  maxPanX) mViewPanX =  maxPanX;
                if (mViewPanX < -maxPanX) mViewPanX = -maxPanX;
                if (mViewPanY >  maxPanY) mViewPanY =  maxPanY;
                if (mViewPanY < -maxPanY) mViewPanY = -maxPanY;
            }
        } else {
            mViewPanX = 0.f;
            mViewPanY = 0.f;
        }

        if (input.IsPressed(Input::BUTTON_X)) {
            mViewerConfirmDelete = true;
            mOverlaySel = 1;
        }

        if (input.IsPressed(Input::BUTTON_B)) {
            if (mViewZoom > ZOOM_MIN + 0.01f) {
                mViewZoom = ZOOM_MIN;
                mViewPanX = 0.f;
                mViewPanY = 0.f;
            } else {
                CloseViewer();
            }
        }
    }
}

void Album::UpdateVideoPlayback() {
    if (!mVideoPlaying || mVideoPaused || !mVideoTexture) return;

    double videoPTS = mVideoDecoder.GetCurrentTime();
    Uint32 currentTime = SDL_GetTicks();

    if (mWallClockStartTime == 0 ||
        (currentTime - mWallClockStartTime) / 1000.0 - (videoPTS - mWallClockStartPTS) > 2.0) {
        if (mWallClockStartTime != 0) {
            WHBLogPrintf("[ALBUM] UpdateVideoPlayback: large time gap detected, re-syncing wall clock");
        }
        mWallClockStartTime = currentTime;
        mWallClockStartPTS = videoPTS;
    }

    if (!mVideoDecoder.HasAudio()) {
        double elapsedWallTime = (currentTime - mWallClockStartTime) / 1000.0;
        double expectedVideoPTS = mWallClockStartPTS + elapsedWallTime;
        double avDrift = videoPTS - expectedVideoPTS;

        static Uint32 lastLog = 0;
        if (currentTime - lastLog > 5000) {
            WHBLogPrintf("[ALBUM] UpdateVideoPlayback: wall-clock vPTS=%.2f exp=%.2f drift=%.2f", videoPTS, expectedVideoPTS, avDrift);
            lastLog = currentTime;
        }

        if (avDrift < -0.1) {
            int framesToSkip = (avDrift < -0.3) ? 3 : 1;
            for (int i = 0; i < framesToSkip; i++) {
                if (!mVideoDecoder.ReadFrame(nullptr)) {
                    mVideoPlaying = false;
                    mVideoDecoder.StopAudio();
                    return;
                }
            }
            mVideoDecoder.ReadFrame(mVideoTexture);
        } else {
            mVideoDecoder.ReadFrame(mVideoTexture);
        }
    } else {
        double audioPTS = mVideoDecoder.GetAudioTime();
        double avDrift = videoPTS - audioPTS;

        static double prevSyncAudioPTS = 0.0;
        static Uint32 audioStallStart = 0;
        bool audioStalled = false;

        if (audioPTS == prevSyncAudioPTS) {
            if (audioStallStart == 0) audioStallStart = currentTime;
            if (currentTime - audioStallStart > 2000 && mVideoDecoder.GetAudioQueueSize() == 0)
                audioStalled = true;
        } else {
            audioStallStart = 0;
        }
        prevSyncAudioPTS = audioPTS;

        if (audioStalled) {
            double elapsedWallTime = (currentTime - mWallClockStartTime) / 1000.0;
            double expectedVideoPTS = mWallClockStartPTS + elapsedWallTime;
            avDrift = videoPTS - expectedVideoPTS;
        } else {
            static Uint32 lastLog = 0;
            if (currentTime - lastLog > 5000) {
                WHBLogPrintf("[ALBUM] UpdateVideoPlayback: A-V sync vPTS=%.2f aPTS=%.2f drift=%.2f isAudioPlaying=%d",
                     videoPTS, audioPTS, avDrift, mVideoDecoder.IsAudioPlaying());
                lastLog = currentTime;
            }
        }

        if (avDrift < -0.1) {
            if (avDrift < -2.0) {
                WHBLogPrintf("[ALBUM] UpdateVideoPlayback: large A-V gap (%.1fs), re-syncing via seek", -avDrift);
                mVideoDecoder.Seek(audioPTS);
                mWallClockStartTime = currentTime;
                mWallClockStartPTS = audioPTS;
                return;
            }
            int framesToSkip = (avDrift < -0.3) ? 3 : 1;
            for (int i = 0; i < framesToSkip; i++) {
                if (!mVideoDecoder.ReadFrame(nullptr)) {
                    mVideoPlaying = false;
                    mVideoDecoder.StopAudio();
                    return;
                }
            }
            mVideoDecoder.ReadFrame(mVideoTexture);
        } else if (avDrift < mFrameDelay / 1000.0) {
            mVideoDecoder.ReadFrame(mVideoTexture);
        }
    }

    if (mVideoDecoder.GetCurrentTime() >= mVideoDecoder.GetDuration() - 0.1) {
        if (mVideoDecoder.GetDuration() > 0) {
            WHBLogPrintf("[ALBUM] UpdateVideoPlayback: reached end of video, looping");
            mVideoDecoder.Seek(0.0);
            mWallClockStartTime = SDL_GetTicks();
            mWallClockStartPTS = mVideoDecoder.GetCurrentTime();
        }
    }
}

void Album::DrawViewer() {
    if (mViewerState == ViewerState::None) return;

    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT,
                        {0x10, 0x10, 0x10, 0xff});

    if (mViewerItem >= 0 && mViewerItem < (int)mFiltered.size()) {
        int idx = mFiltered[mViewerItem];
        auto& item = mAllItems[idx];

        if (item.type == MediaType::Screenshot && mViewerTex) {
            int texW = 0, texH = 0;
            SDL_QueryTexture(mViewerTex, nullptr, nullptr, &texW, &texH);

            float scaleX  = (float)Gfx::SCREEN_WIDTH  / texW;
            float scaleY  = (float)Gfx::SCREEN_HEIGHT / texH;
            float fitScale = (scaleX < scaleY ? scaleX : scaleY);
            float scale   = fitScale * mViewZoom;

            int drawW = (int)(texW * scale);
            int drawH = (int)(texH * scale);
            int drawX = (Gfx::SCREEN_WIDTH  - drawW) / 2 + (int)mViewPanX;
            int drawY = (Gfx::SCREEN_HEIGHT - drawH) / 2 + (int)mViewPanY;

            SDL_Rect clip = {0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT};
            SDL_RenderSetClipRect(Gfx::GetRenderer(), &clip);

            SDL_Rect src = {0, 0, texW, texH};
            SDL_Rect dst = {drawX, drawY, drawW, drawH};
            SDL_RenderCopy(Gfx::GetRenderer(), mViewerTex, &src, &dst);

            SDL_RenderSetClipRect(Gfx::GetRenderer(), nullptr);

        } else if (item.type == MediaType::Video && mVideoTexture) {
            int texW = mVideoDecoder.GetWidth();
            int texH = mVideoDecoder.GetHeight();

            float scaleX  = (float)Gfx::SCREEN_WIDTH  / texW;
            float scaleY  = (float)Gfx::SCREEN_HEIGHT / texH;
            float fitScale = (scaleX < scaleY ? scaleX : scaleY);

            int drawW = (int)(texW * fitScale);
            int drawH = (int)(texH * fitScale);
            int drawX = (Gfx::SCREEN_WIDTH  - drawW) / 2;
            int drawY = (Gfx::SCREEN_HEIGHT - drawH) / 2;

            SDL_Rect src = {0, 0, texW, texH};
            SDL_Rect dst = {drawX, drawY, drawW, drawH};
            SDL_RenderCopy(Gfx::GetRenderer(), mVideoTexture, &src, &dst);
        }
    }

    if (mViewerShowUI) {
        bool isVideo = (mViewerItem >= 0 && mViewerItem < (int)mFiltered.size() &&
                        mAllItems[mFiltered[mViewerItem]].type == MediaType::Video);
        const int VFOOTER_H = isVideo ? 110 : 80;
        int fy = Gfx::SCREEN_HEIGHT - VFOOTER_H;
        Gfx::DrawRectFilled(0, fy, Gfx::SCREEN_WIDTH, VFOOTER_H, {0x00, 0x00, 0x00, 0xaa});

        auto& vitem = mAllItems[mFiltered[mViewerItem]];
        if (vitem.type == MediaType::Video && mVideoPlaying) {
            double dur = mVideoDecoder.GetDuration();
            double cur = mVideoDecoder.GetCurrentTime();
            if (dur > 0) {
                int barX = 40;
                int barY = fy + 8;
                int barW = Gfx::SCREEN_WIDTH - 80;
                int barH = 8;
                Gfx::DrawRectFilled(barX, barY, barW, barH, {0x3c, 0x3c, 0x3c, 0xaa});
                double showTime = mVideoBarSeeking ? mVideoBarSeekTarget : cur;
                int progW = (int)(barW * (showTime / dur));
                Gfx::DrawRectFilled(barX, barY, progW, barH, {0x00, 0x9a, 0xc7, 0xff});
                int cursorX = barX + progW;
                int cursorY = barY + barH / 2;
                Gfx::DrawCircleFilled(cursorX, cursorY, 7, Gfx::COLOR_WHITE);
                Gfx::DrawCircleOutline(cursorX, cursorY, 9, {0x00, 0x9a, 0xc7, 0xff}, 2);
            }
        }
        int cy2 = fy + VFOOTER_H - 30;
        constexpr int IZ = 28;
        constexpr int LZ = 24;
        constexpr int IG = 8;

        if (isVideo) {
            int bx = Gfx::SCREEN_WIDTH - 130;
            Gfx::PrintIcon(bx, cy2, IZ, Gfx::COLOR_BTN_B, Glyphs::B, Gfx::ALIGN_CENTER);
            Gfx::Print(bx + IZ / 2 + IG, cy2, LZ, Gfx::COLOR_WHITE, "Back",
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

            bx = Gfx::SCREEN_WIDTH - 250;
            Gfx::PrintIcon(bx, cy2, IZ, Gfx::COLOR_BTN_A, Glyphs::A, Gfx::ALIGN_CENTER);
            const char* aLabel = mVideoPlaying && !mVideoPaused ? "Pause" : "Play";
            Gfx::Print(bx + IZ / 2 + IG, cy2, LZ, Gfx::COLOR_WHITE, aLabel,
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

            bx = Gfx::SCREEN_WIDTH - 370;
            Gfx::PrintIcon(bx, cy2, IZ, Gfx::COLOR_DELETE, Glyphs::X, Gfx::ALIGN_CENTER);
            Gfx::Print(bx + IZ / 2 + IG, cy2, LZ, Gfx::COLOR_WHITE, "Delete",
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

            bx = Gfx::SCREEN_WIDTH - 490;
            Gfx::PrintIcon(bx, cy2, IZ, Gfx::COLOR_BTN_Y, Glyphs::Y, Gfx::ALIGN_CENTER);
            Gfx::Print(bx + IZ / 2 + IG, cy2, LZ, Gfx::COLOR_WHITE, "Menu",
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

            bx = Gfx::SCREEN_WIDTH - 610;
            Gfx::PrintIcon(bx, cy2, IZ, Gfx::COLOR_TEXT_DIM, Glyphs::PLUS, Gfx::ALIGN_CENTER);
            Gfx::Print(bx + IZ / 2 + IG, cy2, LZ, Gfx::COLOR_WHITE, "Hide",
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

            Gfx::Print(30, cy2, 28, Gfx::COLOR_WHITE,
                       Glyphs::STICK_R + " Seek ",
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        } else {
            int bx = Gfx::SCREEN_WIDTH - 130;
            Gfx::PrintIcon(bx, cy2, IZ, Gfx::COLOR_BTN_B, Glyphs::B, Gfx::ALIGN_CENTER);
            Gfx::Print(bx + IZ / 2 + IG, cy2, LZ, Gfx::COLOR_WHITE, "Back",
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

            bx = Gfx::SCREEN_WIDTH - 250;
            Gfx::PrintIcon(bx, cy2, IZ, Gfx::COLOR_DELETE, Glyphs::X, Gfx::ALIGN_CENTER);
            Gfx::Print(bx + IZ / 2 + IG, cy2, LZ, Gfx::COLOR_WHITE, "Delete",
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

            bx = Gfx::SCREEN_WIDTH - 370;
            Gfx::PrintIcon(bx, cy2, IZ, Gfx::COLOR_BTN_Y, Glyphs::Y, Gfx::ALIGN_CENTER);
            Gfx::Print(bx + IZ / 2 + IG, cy2, LZ, Gfx::COLOR_WHITE, "Menu",
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

            bx = Gfx::SCREEN_WIDTH - 490;
            Gfx::PrintIcon(bx, cy2, IZ, Gfx::COLOR_TEXT_DIM, Glyphs::PLUS, Gfx::ALIGN_CENTER);
            Gfx::Print(bx + IZ / 2 + IG, cy2, LZ, Gfx::COLOR_WHITE, "Hide",
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

            Gfx::Print(30, cy2, 29, Gfx::COLOR_WHITE,
                       mViewZoom > ZOOM_MIN + 0.01f
                       ? Glyphs::STICK_L + " pan " + Glyphs::STICK_R + " zoom "
                       : Glyphs::STICK_R + " zoom ",
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        }
    }

    // Side panel overlay
    if (mViewerSidePanel) {
        const int PW = 420;
        int px = Gfx::SCREEN_WIDTH - PW;
        int py = 0;

        bool isVideo = mViewerItem >= 0 && mViewerItem < (int)mFiltered.size() &&
                       mAllItems[mFiltered[mViewerItem]].type == MediaType::Video;
        const int VFOOTER_H = isVideo ? 110 : 80;
        int panelH = Gfx::SCREEN_HEIGHT - VFOOTER_H;

        Gfx::DrawRectFilled(px, py, PW, panelH, {0x1e, 0x1e, 0x1e, 0xf0});
        Gfx::DrawRectFilled(px, py, 2, panelH, Gfx::COLOR_ACCENT);

        {
            auto& panelItem = mAllItems[mFiltered[mViewerItem]];
            const char* appLabel = panelItem.appName.empty()
                ? (panelItem.type == MediaType::Video ? "Video" : "Screenshot")
                : panelItem.appName.c_str();
            Gfx::Print(px + PW / 2, 36, 28, Gfx::COLOR_WHITE, appLabel, Gfx::ALIGN_CENTER);
            char dtbuf[64];
            time_t mt = (time_t)panelItem.modTime;
            struct tm* tminfo = localtime(&mt);
            if (tminfo) {
                strftime(dtbuf, sizeof(dtbuf), "%Y-%m-%d  %H:%M:%S", tminfo);
                Gfx::Print(px + PW / 2, 66, 22, Gfx::COLOR_TEXT_DIM, dtbuf, Gfx::ALIGN_CENTER);
            }
            Gfx::Print(px + PW / 2, 96, 18, Gfx::COLOR_TEXT_DIM, panelItem.filename.c_str(), Gfx::ALIGN_CENTER);
        }
        Gfx::DrawRectFilled(px + 16, 118, PW - 32, 2, {0x66, 0x66, 0x66, 0xff});

        const char* opts[4];
        int numOpts = 0;
        if (isVideo) { opts[0] = "Clip Video"; opts[1] = "Save Frame as PNG"; opts[2] = "Transfer to Device"; opts[3] = "Delete"; numOpts = 4; }
        else         { opts[0] = "Enter Text"; opts[1] = "Transfer to Device"; opts[2] = "Delete"; numOpts = 3; }
        int itemH = 64;
        int startY = 134;
        for (int i = 0; i < numOpts; i++) {
            int oy = startY + i * itemH;
            bool sel = (i == mViewerSidePanelSel);
            if (sel) {
                Gfx::DrawRectFilled(px + 4, oy, PW - 8, itemH - 4, {0x00, 0x9a, 0xc7, 0x30});
                Gfx::DrawRectFilled(px, oy, 4, itemH - 4, Gfx::COLOR_ACCENT);
            }
            SDL_Color tc = sel ? Gfx::COLOR_ACCENT : Gfx::COLOR_WHITE;
            Gfx::Print(px + 20, oy + itemH / 2, 24, tc, opts[i], Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        }
    }

    if (mSaveNotifEndTime > 0 && SDL_GetTicks() < mSaveNotifEndTime) {
        const auto& th = Gfx::Theme();
        int nw = 400, nh = 100;
        int nx = (Gfx::SCREEN_WIDTH - nw) / 2;
        int ny = (Gfx::SCREEN_HEIGHT - nh) / 2;
        Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT, {0, 0, 0, 100});
        Gfx::DrawRectRounded(nx, ny, nw, nh, 12, th.cardBg);
        Gfx::DrawRectRoundedOutline(nx, ny, nw, nh, 12, Gfx::COLOR_ACCENT, 2);
        Gfx::Print(nx + nw / 2, ny + nh / 2, 30, th.text, "Screenshot saved!", Gfx::ALIGN_CENTER);
    }

    // Delete confirmation dialog
    if (mViewerConfirmDelete) {
        DrawDeleteConfirmDialog("Delete this item?");
    }

    // Draw Wiimote pointer cursor
    if (mPointerDraw) {
        Gfx::DrawCircleFilled(mPointerScreenX, mPointerScreenY, 10, Gfx::COLOR_WHITE);
        Gfx::DrawCircleOutline(mPointerScreenX, mPointerScreenY, 12, Gfx::COLOR_BLACK, 2);
    }
}

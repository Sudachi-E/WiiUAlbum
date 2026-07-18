#include "Album.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <set>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <SDL_image.h>
#include <whb/log.h>
#include <coreinit/debug.h>

static bool EndsWith(const std::string& s, const char* suffix) {
    size_t sl = strlen(suffix);
    if (s.size() < sl) return false;
    return strcasecmp(s.c_str() + s.size() - sl, suffix) == 0;
}

static bool IsImage(const std::string& name) {
    return EndsWith(name, ".png") || EndsWith(name, ".jpg") || EndsWith(name, ".jpeg");
}

static bool IsVideo(const std::string& name) {
    return EndsWith(name, ".mp4") || EndsWith(name, ".avi") || EndsWith(name, ".mkv") || EndsWith(name, ".mov");
}

static std::string ExtractAppName(const std::string& folderName) {
    auto paren = folderName.find(" (");
    if (paren != std::string::npos) {
        auto close = folderName.find(")", paren);
        if (close != std::string::npos) {
            std::string name = folderName.substr(paren + 2, close - paren - 2);
            if (!name.empty()) return name;
        }
    }
    return folderName;
}

static int ScanDir(const std::string& dirPath, MediaType type,
                   std::vector<MediaItem>& out, std::string& diagOut,
                   const std::string& appName = "", int depth = 0) {
    if (depth > 8) return 0;

    DIR* dir = opendir(dirPath.c_str());
    if (!dir) {
        int err = errno;
        WHBLogPrintf("[ALBUM] ScanDir: opendir('%s') failed errno=%d (%s)", dirPath.c_str(), err, strerror(err));
        if (depth == 0) {
            diagOut += "opendir('" + dirPath + "') failed: errno " +
                       std::to_string(err) + " (" + strerror(err) + ")\n";
        }
        return 0;
    }

    int found = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        const std::string fullPath = dirPath + "/" + name;

        bool isDir = false, isReg = false;
        if (entry->d_type == DT_DIR) {
            isDir = true;
        } else if (entry->d_type == DT_REG) {
            isReg = true;
        } else {
            struct stat st;
            if (stat(fullPath.c_str(), &st) == 0) {
                isDir = S_ISDIR(st.st_mode);
                isReg = S_ISREG(st.st_mode);
            }
        }

        if (isDir) {
            std::string childApp = (depth == 0) ? ExtractAppName(name) : appName;
            found += ScanDir(fullPath, type, out, diagOut, childApp, depth + 1);
            continue;
        }

        if (!isReg) continue;

        if (type == MediaType::Screenshot && !IsImage(name)) continue;
        if (type == MediaType::Video      && !IsVideo(name)) continue;

        MediaItem item;
        item.path     = fullPath;
        item.filename = name;
        item.type     = type;
        item.appName  = appName;

        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0)
            item.modTime = (uint32_t)st.st_mtime;

        out.push_back(item);
        found++;
    }
    closedir(dir);

    if (depth == 0)
        WHBLogPrintf("[ALBUM] ScanDir: %d items total under '%s'", found, dirPath.c_str());
    return found;
}

Album::Album(const char* sdRoot) {
    SDL_AtomicSet(&mClipEncodingDone, 1);
    mPathScreenshots = std::string(sdRoot) + "/wiiu/screenshots";
    mPathVideos      = std::string(sdRoot) + "/wiiu/screencaptures";

    WHBLogPrintf("[ALBUM] Screenshot path: %s", mPathScreenshots.c_str());
    WHBLogPrintf("[ALBUM] Video path:      %s", mPathVideos.c_str());

    LoadConfig();
    ScanMedia();
    ApplyFilterSort();

    WHBLogPrintf("[ALBUM] Scan complete: %d items found", (int)mAllItems.size());

    StartThumbWorkers();
}

Album::~Album() {
    StopQRTransfer();
    if (mClipThread.joinable()) mClipThread.join();
    StopThumbWorkers();
    for (auto& item : mAllItems) {
        if (item.thumbnail)      Gfx::DestroyTexture(item.thumbnail);
        if (item.pendingSurface) SDL_FreeSurface(item.pendingSurface);
    }
    if (mViewerTex) Gfx::DestroyTexture(mViewerTex);
}

void Album::StopThumbWorkers() {
    mThumbRunning = false;
    for (int i = 0; i < NUM_THUMB_THREADS; i++)
        if (mThumbThreads[i].joinable()) mThumbThreads[i].join();
}

void Album::StartThumbWorkers() {
    mThumbRunning = true;
    for (int i = 0; i < NUM_THUMB_THREADS; i++)
        mThumbThreads[i] = std::thread(&Album::ThumbWorker, this);
}

void Album::ScanMedia() {
    mAllItems.clear();
    mScanDiagnostics.clear();

    int screenshots = ScanDir(mPathScreenshots, MediaType::Screenshot,
                               mAllItems, mScanDiagnostics);
    int videos      = ScanDir(mPathVideos,      MediaType::Video,
                               mAllItems, mScanDiagnostics);

    std::set<std::string> uniqueApps;
    for (auto& item : mAllItems) {
        if (!item.appName.empty()) uniqueApps.insert(item.appName);
    }
    mAppNames.assign(uniqueApps.begin(), uniqueApps.end());

    if (screenshots + videos == 0 && mScanDiagnostics.empty()) {
        mScanDiagnostics = "Directories exist but contain no recognised files.\n"
                           "Screenshots: .png/.jpg  Videos: .mp4/.avi/.mkv";
    }
}

void Album::Refresh() {
    if (mViewerState != ViewerState::None) CloseViewer();

    StopThumbWorkers();

    for (auto& item : mAllItems) {
        if (item.thumbnail)      Gfx::DestroyTexture(item.thumbnail);
        if (item.pendingSurface) SDL_FreeSurface(item.pendingSurface);
    }
    mAllItems.clear();
    mFiltered.clear();

    ScanMedia();
    ApplyFilterSort();

    mGridCursor    = 0;
    mScrollRow     = 0;
    mSidebarFocus  = false;
    mSidebarSel    = 0;

    StartThumbWorkers();

    WHBLogPrintf("[ALBUM] Album refreshed — %d items found", (int)mAllItems.size());
}

void Album::ApplyFilterSort() {
    if (mMultiSelect) ExitMultiSelect(true);
    mFiltered.clear();
    for (int i = 0; i < (int)mAllItems.size(); i++) {
        const auto& item = mAllItems[i];
        if (mFilter == FilterMode::Screenshots && item.type != MediaType::Screenshot) continue;
        if (mFilter == FilterMode::Videos      && item.type != MediaType::Video)      continue;
        if (!mFilterApp.empty() && item.appName != mFilterApp) continue;
        mFiltered.push_back(i);
    }

    if (mSort == SortOrder::NewestFirst) {
        std::sort(mFiltered.begin(), mFiltered.end(), [&](int a, int b){
            return mAllItems[a].modTime > mAllItems[b].modTime;
        });
    } else {
        std::sort(mFiltered.begin(), mFiltered.end(), [&](int a, int b){
            return mAllItems[a].modTime < mAllItems[b].modTime;
        });
    }

    if (mGridCursor >= (int)mFiltered.size())
        mGridCursor = (int)mFiltered.size() - 1;
    if (mGridCursor < 0) mGridCursor = 0;
    mScrollRow = 0;
}

void Album::ThumbWorker() {
    while (mThumbRunning) {
        int toLoad = -1;
        {
            std::lock_guard<std::mutex> lock(mThumbMutex);

            int visStart = mScrollRow * COLS;
            int visEnd   = std::min(visStart + ROWS_VIS * COLS, (int)mFiltered.size());

            for (int fi = visStart; fi < visEnd && toLoad < 0; fi++) {
                int idx = mFiltered[fi];
                auto& item = mAllItems[idx];
                if (!item.thumbRequested && !item.thumbnail && !item.pendingSurface) {
                    item.thumbRequested = true;
                    toLoad = idx;
                }
            }
            for (int fi = 0; fi < (int)mFiltered.size() && toLoad < 0; fi++) {
                int idx = mFiltered[fi];
                auto& item = mAllItems[idx];
                if (!item.thumbRequested && !item.thumbnail && !item.pendingSurface) {
                    item.thumbRequested = true;
                    toLoad = idx;
                }
            }
        }

        if (toLoad < 0) {
            SDL_Delay(16);
            continue;
        }

        SDL_Surface* full = nullptr;
        if (mAllItems[toLoad].type == MediaType::Video) {
            uint32_t dur = 0;
            full = VideoDecoder::ExtractThumbnail(mAllItems[toLoad].path, &dur);
            if (dur > 0) mAllItems[toLoad].durationSec = dur;
        } else {
            full = IMG_Load(mAllItems[toLoad].path.c_str());
        }
        if (!full) {
            std::lock_guard<std::mutex> lock(mThumbMutex);
            mAllItems[toLoad].pendingSurface = nullptr;
            continue;
        }

        SDL_Surface* thumb = SDL_CreateRGBSurfaceWithFormat(
            0, THUMB_W, THUMB_H, 32, SDL_PIXELFORMAT_RGBA32);

        if (thumb) {
            int sw = full->w;
            int sh = full->h;

            float scaleX = (float)THUMB_W / sw;
            float scaleY = (float)THUMB_H / sh;
            float scale  = (scaleX > scaleY) ? scaleX : scaleY;

            SDL_Rect srcRect;
            srcRect.w = (int)(THUMB_W / scale);
            srcRect.h = (int)(THUMB_H / scale);
            if (srcRect.w > sw) srcRect.w = sw;
            if (srcRect.h > sh) srcRect.h = sh;
            srcRect.x = (sw - srcRect.w) / 2;
            srcRect.y = (sh - srcRect.h) / 2;

            SDL_Rect dstRect = {0, 0, THUMB_W, THUMB_H};
            SDL_BlitScaled(full, &srcRect, thumb, &dstRect);
        }

        SDL_FreeSurface(full);

        {
            std::lock_guard<std::mutex> lock(mThumbMutex);
            mAllItems[toLoad].pendingSurface = thumb;
        }
    }
}

void Album::FlushPendingSurfaces() {
    int uploaded = 0;
    std::lock_guard<std::mutex> lock(mThumbMutex);
    for (auto& item : mAllItems) {
        if (item.pendingSurface && !item.thumbnail) {
            item.thumbnail = SDL_CreateTextureFromSurface(Gfx::GetRenderer(), item.pendingSurface);
            SDL_FreeSurface(item.pendingSurface);
            item.pendingSurface = nullptr;
            if (++uploaded >= 16) break;
        }
    }
}

std::string Album::ComputeSdRoot() const {
    std::string sdRoot = mPathScreenshots;
    auto pos = sdRoot.rfind("/wiiu/screenshots");
    if (pos != std::string::npos) sdRoot = sdRoot.substr(0, pos);
    return sdRoot;
}

void Album::SaveConfig() const {
    std::string sdRoot = ComputeSdRoot();
    std::string dir  = sdRoot + "/wiiu/apps/WiiUAlbum";
    std::string file = dir + "/config.txt";

    mkdir(dir.c_str(), 0777);

    FILE* f = fopen(file.c_str(), "w");
    if (!f) return;
    fprintf(f, "filter=%d\nsort=%d\nfilter_app=%s\n", (int)mFilter, (int)mSort, mFilterApp.c_str());
    fclose(f);
    WHBLogPrintf("[ALBUM] Config saved to %s", file.c_str());
}

void Album::LoadConfig() {
    std::string file = ComputeSdRoot() + "/wiiu/apps/WiiUAlbum/config.txt";

    FILE* f = fopen(file.c_str(), "r");
    if (!f) {
        WHBLogPrintf("[ALBUM] No config file found, using defaults");
        return;
    }

    int filterVal = 0, sortVal = 0;
    char appBuf[256] = {0};
    fscanf(f, "filter=%d\nsort=%d\n", &filterVal, &sortVal);
    char line[512];
    if (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "filter_app=%255[^\n]", appBuf) == 1) {
            size_t len = strlen(appBuf);
            if (len > 0 && appBuf[len-1] == '\r') appBuf[len-1] = '\0';
        }
    }
    fclose(f);

    if (filterVal >= 0 && filterVal <= 2) mFilter = (FilterMode)filterVal;
    if (sortVal   >= 0 && sortVal   <= 1) mSort   = (SortOrder)sortVal;
    if (appBuf[0]) mFilterApp = appBuf;

    WHBLogPrintf("[ALBUM] Config loaded: filter=%d sort=%d", filterVal, sortVal);
}

std::string Album::FormatDuration(uint32_t sec) {
    char buf[16];
    if (sec < 60) {
        snprintf(buf, sizeof(buf), "%us", sec);
    } else {
        uint32_t m = sec / 60, s = sec % 60;
        snprintf(buf, sizeof(buf), "%u:%02u", m, s);
    }
    return buf;
}

std::string Album::GetCountStr() const {
    return std::to_string(mFiltered.size());
}

std::string Album::GetSortStr() const {
    return (mSort == SortOrder::NewestFirst) ? "Newest First" : "Oldest First";
}

std::string Album::GetFilterStr() const {
    if (!mFilterApp.empty()) return mFilterApp;
    switch (mFilter) {
        case FilterMode::Screenshots: return "Screenshots";
        case FilterMode::Videos:      return "Videos";
        default:                      return "All";
    }
}

void Album::OpenOverlay(Overlay o) {
    mOverlay = o;
    if (o == Overlay::Filter) {
        if (!mFilterApp.empty()) {
            auto it = std::find(mAppNames.begin(), mAppNames.end(), mFilterApp);
            mOverlaySel = (it != mAppNames.end()) ? (4 + (int)(it - mAppNames.begin())) : 0;
        } else {
            mOverlaySel = (int)mFilter;
        }
    } else if (o == Overlay::Sort) mOverlaySel = (int)mSort;
    else {
        mOverlaySel = 0;
    }
}

void Album::CloseOverlay() {
    mOverlay = Overlay::None;
}

bool Album::OpenVideoItem(int filteredIdx, bool startAudio) {
    int idx = mFiltered[filteredIdx];
    if (mAllItems[idx].type != MediaType::Video) return false;

    if (mViewerTex) { Gfx::DestroyTexture(mViewerTex); mViewerTex = nullptr; }
    if (mVideoTexture) { SDL_DestroyTexture(mVideoTexture); mVideoTexture = nullptr; }

    WHBLogPrintf("[ALBUM] OpenVideoItem: %s", mAllItems[idx].path.c_str());
    if (!mVideoDecoder.Open(mAllItems[idx].path)) return false;

    int vw = mVideoDecoder.GetWidth();
    int vh = mVideoDecoder.GetHeight();
    WHBLogPrintf("[ALBUM] OpenVideoItem: video %dx%d fps=%.1f hasAudio=%d duration=%.1f",
         vw, vh, mVideoDecoder.GetFrameRate(),
         mVideoDecoder.HasAudio(), mVideoDecoder.GetDuration());

    if (vw <= 0 || vh <= 0) return false;

    mVideoTexture = SDL_CreateTexture(Gfx::GetRenderer(),
                                      SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      vw, vh);
    double fps = mVideoDecoder.GetFrameRate();
    mFrameDelay = 1000.0 / fps;

    for (int attempts = 0; attempts < 50; attempts++) {
        if (mVideoDecoder.GetVideoQueueSize() > 0) {
            mVideoDecoder.ReadFrame(mVideoTexture);
            if (mVideoDecoder.GetCurrentTime() > 0.0) break;
        }
        SDL_Delay(10);
    }

    mVideoPlaying = true;
    mVideoPaused = false;
    mWallClockStartTime = SDL_GetTicks();
    mWallClockStartPTS = mVideoDecoder.GetCurrentTime();

    if (startAudio) {
        WHBLogPrintf("[ALBUM] OpenVideoItem: starting audio playback (hasAudio=%d)", mVideoDecoder.HasAudio());
        mVideoDecoder.StartAudio();
    }
    return true;
}

void Album::NavigateToItem(int nextFilteredIdx, bool resetZoom) {
    if (mViewerTex) { Gfx::DestroyTexture(mViewerTex); mViewerTex = nullptr; }
    if (mVideoTexture) { SDL_DestroyTexture(mVideoTexture); mVideoTexture = nullptr; }
    mVideoDecoder.Close();
    mVideoPlaying = false;
    mVideoPaused = false;
    mViewerItem = nextFilteredIdx;

    if (resetZoom) {
        mViewZoom = 1.0f;
        mViewPanX = 0.0f;
        mViewPanY = 0.0f;
    }

    int nidx = mFiltered[nextFilteredIdx];
    if (mAllItems[nidx].type == MediaType::Screenshot) {
        mViewerTex = Gfx::LoadTexture(mAllItems[nidx].path);
    } else {
        OpenVideoItem(nextFilteredIdx);
    }
}

void Album::Update(const Input& input) {
    FlushPendingSurfaces();

    mPointerConsumedClick = false;

    HandleTouch(input);
    UpdatePointerPosition(input);
    HandlePointer(input);

    if (mQRState != QRState::Inactive) {
        UpdateQRTransfer(input);
        return;
    }

    if (mClipMode) {
        UpdateClipMode(input);
        return;
    }

    if (mTextOverlayActive) {
        UpdateTextOverlay(input);
        return;
    }

    if (mViewerState == ViewerState::Open) {
        UpdateViewer(input);
        return;
    }

    if (mOverlay != Overlay::None) {
        UpdateOverlay(input);
        return;
    }

    CheckAutoRefresh();

    if (mSidebarFocus) {
        int numItems = 4;
        if (input.IsPressed(Input::BUTTON_DOWN))  mSidebarSel = (mSidebarSel + 1) % numItems;
        if (input.IsPressed(Input::BUTTON_UP))    mSidebarSel = (mSidebarSel + numItems - 1) % numItems;
        if (input.IsPressed(Input::BUTTON_RIGHT)) {
            if (!mFiltered.empty()) mSidebarFocus = false;
        }
        if (input.IsPressed(Input::BUTTON_A)) {
            switch (mSidebarSel) {
                case 0: OpenOverlay(Overlay::QuickAccess); break;
                case 1: OpenOverlay(Overlay::Filter);   break;
                case 2: OpenOverlay(Overlay::Sort);     break;
                case 3: OpenOverlay(Overlay::Settings); break;
            }
        }
        if (input.IsPressed(Input::BUTTON_B)) {
        }
    }
    else {
        int total = (int)mFiltered.size();
        if (total == 0) {
            if (input.IsPressed(Input::BUTTON_LEFT) || input.IsPressed(Input::BUTTON_B)) {
                if (mMultiSelect) ExitMultiSelect();
                mSidebarFocus = true;
            }
            return;
        }

        if (input.IsPressed(Input::BUTTON_LEFT)) {
            if (mGridCursor % COLS == 0) {
                if (mMultiSelect) { ExitMultiSelect(); mSidebarFocus = true; }
                else if (mTransferMultiSelect) { mTransferMultiSelect = false; mTransferSelected.clear(); mTransferSelectCount = 0; mSidebarFocus = true; }
                else mSidebarFocus = true;
            } else {
                mGridCursor--;
            }
        }
        if (input.IsPressed(Input::BUTTON_RIGHT)) {
            if (mGridCursor % COLS < COLS - 1 && mGridCursor + 1 < total)
                mGridCursor++;
        }
        if (input.IsPressed(Input::BUTTON_UP)) {
            if (mGridCursor - COLS >= 0) {
                mGridCursor -= COLS;
            } else {
                int col       = mGridCursor % COLS;
                int lastRow   = (total - 1) / COLS;
                int candidate = lastRow * COLS + col;
                if (candidate >= total) candidate -= COLS;
                mGridCursor = candidate;
            }
        }
        if (input.IsPressed(Input::BUTTON_DOWN)) {
            if (mGridCursor + COLS < total) {
                mGridCursor += COLS;
            } else {
                mGridCursor = mGridCursor % COLS;
            }
        }
        int curRow = mGridCursor / COLS;
        if (curRow < mScrollRow) mScrollRow = curRow;
        if (curRow >= mScrollRow + ROWS_VIS) mScrollRow = curRow - ROWS_VIS + 1;

        if (mTransferMultiSelect) {
            if (input.IsPressed(Input::BUTTON_Y)) {
                if (mTransferSelectCount > 0) {
                    std::vector<int> indices;
                    for (size_t i = 0; i < mTransferSelected.size(); i++)
                        if (mTransferSelected[i]) indices.push_back((int)i);
                    mTransferMultiSelect = false;
                    mTransferMode = TransferMode::None;
                    StartQRMultiTransfer(indices);
                }
                return;
            }
            if (input.IsPressed(Input::BUTTON_A)) {
                bool wasSel = mTransferSelected[mGridCursor];
                if (!wasSel && mTransferSelectCount < 5) {
                    mTransferSelected[mGridCursor] = true;
                    mTransferSelectCount++;
                } else if (wasSel) {
                    mTransferSelected[mGridCursor] = false;
                    mTransferSelectCount--;
                }
                return;
            }
            if (input.IsPressed(Input::BUTTON_B)) {
                mTransferMultiSelect = false;
                mTransferSelected.clear();
                mTransferSelectCount = 0;
                mTransferFilePaths.clear();
                return;
            }
        } else if (mTransferMode == TransferMode::Single) {
            if (input.IsPressed(Input::BUTTON_A) && !mFiltered.empty()) {
                mTransferMode = TransferMode::None;
                StartQRTransfer(mGridCursor);
            }
            if (input.IsPressed(Input::BUTTON_B)) {
                mTransferMode = TransferMode::None;
            }
            return;
        } else {
            if (input.IsPressed(Input::BUTTON_X)) {
                if (mMultiSelect) {
                    bool any = false;
                    for (bool s : mSelected) { if (s) { any = true; break; } }
                    if (any) {
                        OpenOverlay(Overlay::DeleteConfirm);
                    }
                }
                return;
            }

            if (input.IsPressed(Input::BUTTON_A)) {
                if (mMultiSelect) {
                    mSelected[mGridCursor] = !mSelected[mGridCursor];
                } else {
                    OpenViewer(mGridCursor);
                }
            }
            if (input.IsPressed(Input::BUTTON_B)) {
                if (mMultiSelect) {
                    ExitMultiSelect(true);
                } else {
                    mSidebarFocus = true;
                }
            }
        }
    }
}

void Album::Draw() {
    if (mQRState != QRState::Inactive) {
        DrawQRTransfer();
        return;
    }

    if (mClipMode) {
        DrawClipMode();
        return;
    }

    if (mTextOverlayActive) {
        DrawTextOverlay();
        return;
    }

    if (mViewerState == ViewerState::Open) {
        DrawViewer();
        return;
    }

    Gfx::Clear(Gfx::COLOR_BG);

    DrawGrid();
    DrawSidebar();
    DrawHeader();
    DrawFooter();

    if (mOverlay != Overlay::None) DrawOverlay();

    // Draw Wiimote pointer cursor
    if (mPointerDraw) {
        // White circle with dark outline
        Gfx::DrawCircleFilled(mPointerScreenX, mPointerScreenY, 10, Gfx::COLOR_WHITE);
        Gfx::DrawCircleOutline(mPointerScreenX, mPointerScreenY, 12, Gfx::COLOR_BLACK, 2);
    }
}

void Album::CheckAutoRefresh() {
    Uint32 now = SDL_GetTicks();
    if (now - mLastAutoRefreshTime < 3000) return;
    mLastAutoRefreshTime = now;

    struct stat st1, st2;
    time_t s1 = 0, s2 = 0;
    if (stat(mPathScreenshots.c_str(), &st1) == 0) s1 = st1.st_mtime;
    if (stat(mPathVideos.c_str(),      &st2) == 0) s2 = st2.st_mtime;

    if ((mLastDirMtimeScreenshots != 0 || mLastDirMtimeVideos != 0) &&
        (s1 != mLastDirMtimeScreenshots || s2 != mLastDirMtimeVideos)) {
        Refresh();
    }
    mLastDirMtimeScreenshots = s1;
    mLastDirMtimeVideos      = s2;
}

// GamePad touch

bool Album::TouchHitRect(int tx, int ty, int rx, int ry, int rw, int rh) const {
    return tx >= rx && tx < rx + rw && ty >= ry && ty < ry + rh;
}

int Album::TouchHitTestSidebar(int tx, int ty) const {
    int iconSize = 44;
    int spacing  = 80;
    int startY   = HEADER_H + 40;
    int ix       = (SIDEBAR_W - iconSize) / 2;
    for (int i = 0; i < 4; i++) {
        int iy = startY + i * spacing;
        if (TouchHitRect(tx, ty, ix - 10, iy - 10, iconSize + 20, iconSize + 20))
            return i;
    }
    return -1;
}

int Album::TouchHitTestGrid(int tx, int ty) const {
    if (mFiltered.empty()) return -1;

    int cellW = THUMB_W + THUMB_PAD;
    int cellH = THUMB_H + THUMB_PAD;
    int gridContentW = COLS * cellW;
    int gridOffsetX  = GRID_X + (GRID_W - gridContentW) / 2;

    int totalRows = ((int)mFiltered.size() + COLS - 1) / COLS;
    int endRow    = std::min(mScrollRow + ROWS_VIS + 1, totalRows);

    for (int row = mScrollRow; row < endRow; row++) {
        for (int col = 0; col < COLS; col++) {
            int fi = row * COLS + col;
            if (fi >= (int)mFiltered.size()) break;

            int x = gridOffsetX + col * cellW;
            int y = GRID_Y + (row - mScrollRow) * cellH;

            if (TouchHitRect(tx, ty, x, y, cellW, cellH))
                return fi;
        }
    }
    return -1;
}

// Shared action handlers (used by both touch and pointer input)

void Album::ResumeVideoAudio() {
    mVideoDecoder.PauseAudio(false);
    mWallClockStartTime = SDL_GetTicks();
    mWallClockStartPTS = mVideoDecoder.GetCurrentTime();
}

void Album::OpenSidebarOverlay(int idx) {
    switch (idx) {
        case 0: OpenOverlay(Overlay::QuickAccess); break;
        case 1: OpenOverlay(Overlay::Filter);     break;
        case 2: OpenOverlay(Overlay::Sort);       break;
        case 3: OpenOverlay(Overlay::Settings);   break;
    }
}

bool Album::HandleQRTransferClick(int px, int py) {
    int hintY = Gfx::SCREEN_HEIGHT - 40;
    constexpr int IZ = 28;

    bool isMulti = (mTransferMode == TransferMode::Multi);
    bool showA = (mQRState == QRState::ShowQR && !isMulti &&
                  !mFileServer.GetClientIP().empty() &&
                  !mFileServer.IsTransferConfirmed());

    if (showA) {
        if (TouchHitRect(px, py, 350 - IZ / 2 - 10, hintY - IZ / 2 - 10, 100, IZ + 20)) {
            mFileServer.ConfirmTransfer();
            return true;
        }
        if (TouchHitRect(px, py, 472 - IZ / 2 - 10, hintY - IZ / 2 - 10, 100, IZ + 20)) {
            StopQRTransfer();
            mSidebarFocus = true;
            return true;
        }
    } else {
        if (TouchHitRect(px, py, 180 - IZ / 2 - 10, hintY - IZ / 2 - 10, 100, IZ + 20)) {
            StopQRTransfer();
            mSidebarFocus = true;
            return true;
        }
    }
    return false;
}

bool Album::HandleDeleteConfirmClick(int px, int py) {
    int bw = 480, bh = 200;
    int bx = (Gfx::SCREEN_WIDTH - bw) / 2;
    int by = (Gfx::SCREEN_HEIGHT - bh) / 2;
    int btnY = by + 110;
    int btnW = 160, btnH = 52;
    int gap  = 40;

    int delX = bx + (bw - btnW * 2 - gap) / 2;
    if (TouchHitRect(px, py, delX, btnY, btnW, btnH)) {
        if (mOverlaySel == 0) {
            CloseOverlay();
            if (mMultiSelect) ExecuteMultiDelete();
            else ExecuteDelete();
        } else {
            CloseOverlay();
        }
        return true;
    }
    int canX = delX + btnW + gap;
    if (TouchHitRect(px, py, canX, btnY, btnW, btnH)) {
        mViewerConfirmDelete = false;
        int idx = (mViewerItem >= 0 && mViewerItem < (int)mFiltered.size()) ? mFiltered[mViewerItem] : -1;
        bool isVideo = (idx >= 0 && mAllItems[idx].type == MediaType::Video);
        if (isVideo && mVideoPlaying && !mClipMode) ResumeVideoAudio();
        return true;
    }
    return false;
}

bool Album::HandleViewerSidePanelClick(int px, int py) {
    if (!mViewerSidePanel) return false;

    int idx = (mViewerItem >= 0 && mViewerItem < (int)mFiltered.size()) ? mFiltered[mViewerItem] : -1;
    bool isVideo = (idx >= 0 && mAllItems[idx].type == MediaType::Video);
    const int PW = 420;
    int panelX = Gfx::SCREEN_WIDTH - PW;
    int VFOOTER_H = isVideo ? 110 : 80;
    int panelH = Gfx::SCREEN_HEIGHT - VFOOTER_H;

    if (!TouchHitRect(px, py, panelX, 0, PW, panelH)) {
        mViewerSidePanel = false;
        if (isVideo && mVideoPlaying && !mClipMode) ResumeVideoAudio();
        return true;
    }

    int numOpts = isVideo ? 4 : 3;
    int itemH = 64;
    int startY = 110;
    for (int i = 0; i < numOpts; i++) {
        int oy = startY + i * itemH;
        if (TouchHitRect(px, py, panelX, oy, PW, itemH - 4)) {
            mViewerSidePanelSel = i;
            if (isVideo && i == 0) {
                EnterClipMode();
                mViewerSidePanel = false;
            } else if (isVideo && i == 1) {
                SaveScreenshot();
            } else if ((isVideo && i == 2) || (!isVideo && i == 1)) {
                mPendingTransferIdx = mViewerItem;
                CloseViewer();
                mOverlaySel = 0;
                mOverlay = Overlay::TransferMode;
                mViewerSidePanel = false;
            } else if (!isVideo && i == 0) {
                mViewerSidePanel = false;
                EnterTextOverlay();
            } else {
                mViewerConfirmDelete = true;
                mOverlaySel = 1;
                mViewerSidePanel = false;
            }
            if (!mViewerSidePanel && isVideo && mVideoPlaying && !mClipMode) ResumeVideoAudio();
            return true;
        }
    }
    return false;
}

bool Album::HandleViewerFooterClick(int px, int py) {
    int idx = (mViewerItem >= 0 && mViewerItem < (int)mFiltered.size()) ? mFiltered[mViewerItem] : -1;
    bool isVideo = (idx >= 0 && mAllItems[idx].type == MediaType::Video);
    const int VFOOTER_H = isVideo ? 110 : 80;
    int fy = Gfx::SCREEN_HEIGHT - VFOOTER_H;
    int cy2 = fy + VFOOTER_H - 30;
    constexpr int IZ = 28;

    if (isVideo) {
        if (TouchHitRect(px, py, Gfx::SCREEN_WIDTH - 154, cy2 - IZ / 2, 100, IZ + 8)) {
            CloseViewer();
            return true;
        }
        if (TouchHitRect(px, py, Gfx::SCREEN_WIDTH - 274, cy2 - IZ / 2, 120, IZ + 8)) {
            if (mVideoPlaying && !mVideoPaused) {
                mVideoPaused = true;
                mVideoDecoder.PauseAudio(true);
            } else {
                mVideoPlaying = true;
                mVideoPaused = false;
                ResumeVideoAudio();
            }
            return true;
        }
        if (TouchHitRect(px, py, Gfx::SCREEN_WIDTH - 394, cy2 - IZ / 2, 100, IZ + 8)) {
            mViewerConfirmDelete = true; mOverlaySel = 1;
            if (mVideoPlaying) mVideoDecoder.PauseAudio(true);
            return true;
        }
        if (TouchHitRect(px, py, Gfx::SCREEN_WIDTH - 514, cy2 - IZ / 2, 100, IZ + 8)) {
            mViewerSidePanel = true; mViewerSidePanelSel = 0;
            if (mVideoPlaying) mVideoDecoder.PauseAudio(true);
            return true;
        }
        if (TouchHitRect(px, py, Gfx::SCREEN_WIDTH - 634, cy2 - IZ / 2, 100, IZ + 8)) {
            mViewerShowUI = !mViewerShowUI;
            return true;
        }
    } else {
        if (TouchHitRect(px, py, Gfx::SCREEN_WIDTH - 154, cy2 - IZ / 2, 100, IZ + 8)) {
            if (mViewZoom > ZOOM_MIN + 0.01f) {
                mViewZoom = ZOOM_MIN; mViewPanX = 0.f; mViewPanY = 0.f;
            } else {
                CloseViewer();
            }
            return true;
        }
        if (TouchHitRect(px, py, Gfx::SCREEN_WIDTH - 274, cy2 - IZ / 2, 100, IZ + 8)) {
            mViewerConfirmDelete = true; mOverlaySel = 1;
            return true;
        }
        if (TouchHitRect(px, py, Gfx::SCREEN_WIDTH - 394, cy2 - IZ / 2, 100, IZ + 8)) {
            mViewerSidePanel = true; mViewerSidePanelSel = 0;
            return true;
        }
        if (TouchHitRect(px, py, Gfx::SCREEN_WIDTH - 514, cy2 - IZ / 2, 100, IZ + 8)) {
            mViewerShowUI = !mViewerShowUI;
            return true;
        }
    }
    return false;
}

bool Album::HandleOverlayClick(int px, int py) {
    if (mOverlay == Overlay::DeleteConfirm) {
        int bw = 480, bh = 200;
        int bx = (Gfx::SCREEN_WIDTH - bw) / 2;
        int by = (Gfx::SCREEN_HEIGHT - bh) / 2;
        int btnY = by + 110;
        int btnW = 160, btnH = 52;
        int gap  = 40;

        int delX = bx + (bw - btnW * 2 - gap) / 2;
        if (TouchHitRect(px, py, delX, btnY, btnW, btnH)) {
            if (mOverlaySel == 0) {
                CloseOverlay();
                if (mMultiSelect) ExecuteMultiDelete();
                else ExecuteDelete();
            } else {
                CloseOverlay();
            }
            return true;
        }
        int canX = delX + btnW + gap;
        if (TouchHitRect(px, py, canX, btnY, btnW, btnH)) {
            CloseOverlay();
            return true;
        }
        return false;
    }

    if (mOverlay == Overlay::TransferMode) {
        int bw = 400;
        int itemH = 56;
        int bh = 60 + 2 * itemH + 16;
        int bx = (Gfx::SCREEN_WIDTH - bw) / 2;
        int by = (Gfx::SCREEN_HEIGHT - bh) / 2;

        if (!TouchHitRect(px, py, bx, by, bw, bh)) {
            CloseOverlay();
            return true;
        }
        for (int i = 0; i < 2; i++) {
            int oy = by + 60 + i * itemH;
            if (TouchHitRect(px, py, bx + 8, oy + 4, bw - 16, itemH - 8)) {
                Uint32 now = SDL_GetTicks();
                bool doubleTap = (i == mTouchLastOverlaySel &&
                                  (now - mTouchLastOverlayTapTime) < DOUBLE_TAP_MS);
                mOverlaySel = i;
                mTouchLastOverlaySel = i;
                mTouchLastOverlayTapTime = now;

                if (doubleTap) {
                    if (i == 0) {
                        mTransferMode = TransferMode::Single;
                        CloseOverlay();
                        if (mPendingTransferIdx >= 0) StartQRTransfer(mPendingTransferIdx);
                        else mSidebarFocus = false;
                    } else {
                        mTransferMode = TransferMode::Multi;
                        mTransferMultiSelect = true;
                        mTransferSelectCount = 0;
                        mTransferSelected.assign(mFiltered.size(), false);
                        mTransferFilePaths.clear();
                        CloseOverlay();
                        mSidebarFocus = false;
                    }
                }
                return true;
            }
        }
        return false;
    }

    if (mOverlay == Overlay::Settings) {
        int bw = 400;
        int itemH = 56;
        int bh = 60 + 1 * itemH + 16;
        int bx = (Gfx::SCREEN_WIDTH - bw) / 2;
        int by = (Gfx::SCREEN_HEIGHT - bh) / 2;

        if (!TouchHitRect(px, py, bx, by, bw, bh)) {
            CloseOverlay();
            return true;
        }
        return false;
    }

    // Sidebar-style overlays: Filter, Sort, QuickAccess
    const int PW = 280;
    int startY = HEADER_H + 40;
    int spacing = 80;
    int slot, numItems, itemH;
    switch (mOverlay) {
        case Overlay::QuickAccess: slot = 0; numItems = 3; itemH = 42; break;
        case Overlay::Filter:      slot = 1; numItems = 3 + 1 + (int)mAppNames.size(); itemH = 42; break;
        case Overlay::Sort:        slot = 2; numItems = 2; itemH = 42; break;
        default: return false;
    }

    int panelY = startY + slot * spacing;
    int ph = 20 + numItems * itemH + 16;
    if (panelY + ph > Gfx::SCREEN_HEIGHT - 10) panelY = Gfx::SCREEN_HEIGHT - 10 - ph;
    if (panelY < 10) panelY = 10;
    int panelX = SIDEBAR_W + 4;

    if (!TouchHitRect(px, py, panelX, panelY, PW, ph)) {
        CloseOverlay();
        return true;
    }

    int yo = panelY + 10;
    for (int i = 0; i < numItems; i++) {
        if (i == 3) { yo += 16; continue; }
        if (TouchHitRect(px, py, panelX + 6, yo, PW - 12, itemH - 4)) {
            mOverlaySel = i;
            Uint32 now = SDL_GetTicks();
            bool doubleTap = (i == mTouchLastOverlaySel &&
                              (now - mTouchLastOverlayTapTime) < DOUBLE_TAP_MS);
            mTouchLastOverlaySel = i;
            mTouchLastOverlayTapTime = now;

            if (mOverlay == Overlay::Filter) {
                if (i < 3) {
                    mFilter = (FilterMode)i;
                    mFilterApp.clear();
                } else {
                    mFilter = FilterMode::All;
                    mFilterApp = (i >= 4) ? mAppNames[i - 4] : "";
                }
                ApplyFilterSort();
                SaveConfig();
            } else if (mOverlay == Overlay::Sort) {
                if (doubleTap) {
                    mSort = (SortOrder)i;
                    ApplyFilterSort();
                    SaveConfig();
                    CloseOverlay();
                }
            } else if (mOverlay == Overlay::QuickAccess) {
                if (doubleTap) {
                    if (i == 0) {
                        CloseOverlay();
                        Refresh();
                    } else if (i == 1) {
                        mPendingTransferIdx = -1;
                        CloseOverlay();
                        mOverlaySel = 0;
                        mOverlay = Overlay::TransferMode;
                    } else if (i == 2) {
                        CloseOverlay();
                        if (!mFiltered.empty()) EnterMultiSelect();
                    }
                }
            }
            return true;
        }
        yo += itemH;
    }
    return false;
}

void Album::HandleGridItemClick(int filteredIdx) {
    if (filteredIdx < 0 || filteredIdx >= (int)mFiltered.size()) return;
    if (mMultiSelect) {
        mSelected[filteredIdx] = !mSelected[filteredIdx];
    } else if (mTransferMultiSelect) {
        bool wasSel = mTransferSelected[filteredIdx];
        if (!wasSel && mTransferSelectCount < 5) {
            mTransferSelected[filteredIdx] = true;
            mTransferSelectCount++;
        } else if (wasSel) {
            mTransferSelected[filteredIdx] = false;
            mTransferSelectCount--;
        }
    } else if (mTransferMode == TransferMode::Single) {
        mTransferMode = TransferMode::None;
        StartQRTransfer(filteredIdx);
    } else {
        OpenViewer(filteredIdx);
    }
}

bool Album::HandleFooterBackClick(int px, int py) {
    int ftrY = Gfx::SCREEN_HEIGHT - FOOTER_H;
    int cx = Gfx::SCREEN_WIDTH - 30;
    constexpr int BTN_W = 120;
    constexpr int BTN_H = FOOTER_H;

    if (mMultiSelect || mTransferMultiSelect || mTransferMode == TransferMode::Single) {
        int backBx = mMultiSelect ? cx - 180 : cx - 80;
        if (TouchHitRect(px, py, backBx - 40, ftrY, BTN_W, BTN_H)) {
            if (mMultiSelect) ExitMultiSelect(true);
            else if (mTransferMultiSelect) {
                mTransferMultiSelect = false;
                mTransferSelected.clear();
                mTransferSelectCount = 0;
                mTransferFilePaths.clear();
            } else if (mTransferMode == TransferMode::Single) {
                mTransferMode = TransferMode::None;
            }
            return true;
        }

        if (mMultiSelect) {
            int okBx = cx - 80;
            if (TouchHitRect(px, py, okBx - 40, ftrY, BTN_W, BTN_H)) {
                bool any = false;
                for (bool s : mSelected) { if (s) { any = true; break; } }
                if (any) OpenOverlay(Overlay::DeleteConfirm);
                return true;
            }
        }
    }
    return false;
}

// Input touch

void Album::HandleTouch(const Input& input) {
    if (!input.IsTouchJustPressed() && !input.IsTouchPressed() && !input.IsTouchJustReleased()) return;

    int tx = input.GetTouchX();
    int ty = input.GetTouchY();

    if (mQRState != QRState::Inactive) {
        if (input.IsTouchJustPressed()) HandleQRTransferClick(tx, ty);
        return;
    }
    if (mClipMode) return;
    if (mTextOverlayActive) return;

    if (mViewerState == ViewerState::Open && mViewerConfirmDelete) {
        if (input.IsTouchJustPressed()) HandleDeleteConfirmClick(tx, ty);
        return;
    }
    if (mViewerState == ViewerState::Open && mSaveNotifEndTime > 0) {
        if (input.IsTouchJustPressed()) mSaveNotifEndTime = 0;
        return;
    }

    if (mViewerState == ViewerState::Open) {
        if (mViewerSidePanel) {
            if (input.IsTouchJustPressed()) HandleViewerSidePanelClick(tx, ty);
            return;
        }

        // Swipe detection on release
        if (input.IsTouchJustReleased() && !mTouchViewerSwipeHandled) {
            int dx = input.GetTouchX() - input.GetTouchStartX();
            int dy = input.GetTouchY() - input.GetTouchStartY();
            int vidx = (mViewerItem >= 0 && mViewerItem < (int)mFiltered.size()) ? mFiltered[mViewerItem] : -1;
            bool isVideo = (vidx >= 0 && mAllItems[vidx].type == MediaType::Video);

            if (abs(dx) > 200 && abs(dx) > abs(dy) * 2) {
                int total = (int)mFiltered.size();
                int next = -1;
                if (dx < 0) next = (mViewerItem + 1) % total;
                if (dx > 0) next = (mViewerItem - 1 + total) % total;
                if (next >= 0 && next != mViewerItem) {
                    NavigateToItem(next, isVideo ? false : true);
                    mTouchViewerSwipeHandled = true;
                    return;
                }
            }
        }
        if (input.IsTouchJustPressed()) mTouchViewerSwipeHandled = false;

        // Footer buttons
        if (input.IsTouchJustPressed()) {
            if (!HandleViewerFooterClick(tx, ty)) {
                int vidx = (mViewerItem >= 0 && mViewerItem < (int)mFiltered.size()) ? mFiltered[mViewerItem] : -1;
                bool isVideo = (vidx >= 0 && mAllItems[vidx].type == MediaType::Video);
                const int VFOOTER_H = isVideo ? 110 : 80;
                int fy = Gfx::SCREEN_HEIGHT - VFOOTER_H;
                if (!TouchHitRect(tx, ty, 0, fy, Gfx::SCREEN_WIDTH, VFOOTER_H)) {
                    mViewerShowUI = !mViewerShowUI;
                }
            }
        }
        return;
    }

    if (mOverlay != Overlay::None) {
        if (input.IsTouchJustPressed()) HandleOverlayClick(tx, ty);
        return;
    }

    // Main grid view
    if (input.IsTouchJustPressed()) {
        int sbIdx = TouchHitTestSidebar(tx, ty);
        if (sbIdx >= 0) {
            mSidebarFocus = true;
            mSidebarSel = sbIdx;
            OpenSidebarOverlay(sbIdx);
            return;
        }

        int ftrY = Gfx::SCREEN_HEIGHT - FOOTER_H;
        if (ty >= ftrY) {
            HandleFooterBackClick(tx, ty);
            return;
        }

        int fi = TouchHitTestGrid(tx, ty);
        if (fi >= 0) {
            mSidebarFocus = false;
            mGridCursor = fi;
            mTouchGridPressIdx = fi;
            return;
        }
    }

    if (input.IsTouchJustReleased() && mTouchGridPressIdx >= 0) {
        int dx = input.GetTouchX() - input.GetTouchStartX();
        int dy = input.GetTouchY() - input.GetTouchStartY();
        bool wasSwipe = (abs(dx) > 40 || abs(dy) > 40);

        if (!wasSwipe) HandleGridItemClick(mTouchGridPressIdx);
        mTouchGridPressIdx = -1;
    }

    // Grid scroll via touch drag anywhere on the grid
    if (input.IsTouchPressed() && !mSidebarFocus && !mFiltered.empty()) {
        int ty2 = input.GetTouchY();
        if (ty2 >= GRID_Y && ty2 < GRID_Y + GRID_H) {
            int dragY = input.GetTouchDragY();
            if (dragY != 0) {
                int totalRows = ((int)mFiltered.size() + COLS - 1) / COLS;
                mTouchGridScrollAccum += -dragY / 2.f;
                int rowDelta = (int)mTouchGridScrollAccum;
                if (rowDelta != 0) {
                    mScrollRow += rowDelta;
                    mTouchGridScrollAccum -= rowDelta;
                    if (mScrollRow < 0) mScrollRow = 0;
                    if (mScrollRow > totalRows - ROWS_VIS) mScrollRow = totalRows - ROWS_VIS;
                    if (mScrollRow < 0) mScrollRow = 0;
                }
            }
        } else {
            mTouchGridScrollAccum = 0.f;
        }
    } else {
        mTouchGridScrollAccum = 0.f;
    }
}

// Wiimote pointer

void Album::UpdatePointerPosition(const Input& input) {
    bool pointerActive = input.IsPointerActive();

    if (!pointerActive) {
        mPointerDraw = false;
        return;
    }

    mPointerScreenX = input.GetPointerX();
    mPointerScreenY = input.GetPointerY();
    mPointerDraw = true;
}

void Album::HandlePointer(const Input& input) {
    bool pointerActive = input.IsPointerActive();
    bool aDown   = pointerActive && input.IsPressed(Input::BUTTON_A);
    bool aClick  = aDown && !mPointerAClickWasDown;
    mPointerAClickWasDown = aDown;

    if (!pointerActive) return;

    int px = mPointerScreenX;
    int py = mPointerScreenY;

    if (mQRState != QRState::Inactive) {
        mPointerConsumedClick = true;
        if (aClick) HandleQRTransferClick(px, py);
        return;
    }
    if (mClipMode) return;
    if (mTextOverlayActive) return;

    if (mViewerState == ViewerState::Open && mViewerConfirmDelete) {
        mPointerConsumedClick = true;
        if (aClick) HandleDeleteConfirmClick(px, py);
        return;
    }
    if (mViewerState == ViewerState::Open && mSaveNotifEndTime > 0) {
        mPointerConsumedClick = true;
        if (aClick) mSaveNotifEndTime = 0;
        return;
    }

    if (mViewerState == ViewerState::Open) {
        mPointerConsumedClick = true;

        if (HandleViewerSidePanelClick(px, py)) return;

        // Duration bar seeking (hold A on the bar to seek with Wiimote)
        int vidx = (mViewerItem >= 0 && mViewerItem < (int)mFiltered.size()) ? mFiltered[mViewerItem] : -1;
        bool isVideo = (vidx >= 0 && mAllItems[vidx].type == MediaType::Video);
        if (isVideo && mVideoPlaying && !mViewerSidePanel) {
            const int VFOOTER_H = 110;
            int fy = Gfx::SCREEN_HEIGHT - VFOOTER_H;
            int barX = 40;
            int barY = fy + 8;
            int barW = Gfx::SCREEN_WIDTH - 80;
            int barH = 8;

            if (aClick && TouchHitRect(px, py, barX, barY - 12, barW, barH + 24)) {
                mVideoBarSeeking = true;
            }
            if (mVideoBarSeeking && aDown) {
                double dur = mVideoDecoder.GetDuration();
                if (dur > 0) {
                    float t = (float)(px - barX) / barW;
                    if (t < 0.f) t = 0.f;
                    if (t > 1.f) t = 1.f;
                    mVideoBarSeekTarget = dur * t;
                }
            }
            if (mVideoBarSeeking && !aDown) {
                mVideoDecoder.Seek(mVideoBarSeekTarget);
                mWallClockStartTime = 0;
                mVideoBarSeeking = false;
            }
        }

        // Footer buttons
        if (aClick) HandleViewerFooterClick(px, py);
        return;
    }

    if (mOverlay != Overlay::None) {
        mPointerConsumedClick = true;
        if (aClick) HandleOverlayClick(px, py);
        return;
    }

    // Main grid view
    int sbIdx = TouchHitTestSidebar(px, py);
    if (sbIdx >= 0) {
        mSidebarFocus = true;
        mSidebarSel = sbIdx;
    }

    if (!mSidebarFocus && !mFiltered.empty() && py >= GRID_Y && py < GRID_Y + GRID_H) {
        int fi = TouchHitTestGrid(px, py);
        if (fi >= 0) mGridCursor = fi;
    }

    if (aClick) {
        if (sbIdx >= 0) {
            mSidebarFocus = true;
            mSidebarSel = sbIdx;
            OpenSidebarOverlay(sbIdx);
            mPointerConsumedClick = true;
            return;
        }

        int ftrY = Gfx::SCREEN_HEIGHT - FOOTER_H;
        if (py >= ftrY) {
            if (HandleFooterBackClick(px, py)) mPointerConsumedClick = true;
            return;
        }

        int fi = TouchHitTestGrid(px, py);
        if (fi >= 0) {
            mSidebarFocus = false;
            mGridCursor = fi;
            HandleGridItemClick(fi);
            mPointerConsumedClick = true;
        }
    }
}

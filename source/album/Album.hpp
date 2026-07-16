#pragma once

#include "../ui/Gfx.hpp"
#include "../ui/Input.hpp"
#include "../video/VideoDecoder.hpp"
#include "../video/ClipEncoder.hpp"
#include "../qr/QRCode.hpp"
#include "../network/FileServer.hpp"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

enum class MediaType { Screenshot, Video };

struct MediaItem {
    std::string  path;
    std::string  filename;
    MediaType    type;
    uint32_t     modTime;
    uint32_t     durationSec;
    std::string  appName;

    SDL_Texture* thumbnail      = nullptr;
    SDL_Surface* pendingSurface = nullptr;
    bool         thumbRequested = false;
};

enum class FilterMode { All, Screenshots, Videos };
enum class SortOrder  { NewestFirst, OldestFirst };

enum class Overlay { None, Filter, Sort, Settings, DeleteConfirm, TransferMode, QuickAccess };

enum class ViewerState { None, Open };

enum class QRState { Inactive, ShowQR, Transferring, Done, Error };
enum class TransferMode { None, Single, Multi };

class Album {
public:
    Album(const char* sdRoot);
    ~Album();

    void Update(const Input& input);
    void Draw();
    void StopThumbWorkers();

private:
    std::vector<MediaItem> mAllItems;
    std::vector<int>       mFiltered;

    FilterMode  mFilter   = FilterMode::All;
    SortOrder   mSort     = SortOrder::NewestFirst;
    std::string mFilterApp;
    std::vector<std::string> mAppNames;

    void ScanMedia();
    void ApplyFilterSort();
    void Refresh();

    int      mSidebarSel  = 0;
    bool     mSidebarFocus = false;
    Overlay  mOverlay     = Overlay::None;

    int      mGridCursor  = 0;
    int      mScrollRow   = 0;

    static constexpr int COLS     = 4;
    static constexpr int THUMB_W  = 400;
    static constexpr int THUMB_H  = 225;
    static constexpr int THUMB_PAD = 12;

    static constexpr int HEADER_H  = 72;
    static constexpr int FOOTER_H  = 60;
    static constexpr int SIDEBAR_W = 100;
    static constexpr int GRID_X    = SIDEBAR_W + 20;
    static constexpr int GRID_Y    = HEADER_H;
    static constexpr int GRID_W    = Gfx::SCREEN_WIDTH - GRID_X - 20;
    static constexpr int GRID_H    = Gfx::SCREEN_HEIGHT - GRID_Y - FOOTER_H;
    static constexpr int ROWS_VIS  = GRID_H / (THUMB_H + THUMB_PAD);

    int  mOverlaySel = 0;
    void OpenOverlay(Overlay o);
    void CloseOverlay();
    void UpdateOverlay(const Input& input);
    void UpdateFilterOverlay(const Input& input);
    void UpdateSortOverlay(const Input& input);
    void UpdateDeleteConfirmOverlay(const Input& input);
    void UpdateTransferModeOverlay(const Input& input);
    void UpdateQuickAccessOverlay(const Input& input);
    void DrawOverlay();

    bool             mMultiSelect   = false;
    std::vector<bool> mSelected;
    void EnterMultiSelect();
    void ExitMultiSelect(bool keepCursor = false);
    void ExecuteMultiDelete();

    ViewerState  mViewerState = ViewerState::None;
    int          mViewerItem  = -1;
    SDL_Texture* mViewerTex   = nullptr;
    bool         mViewerConfirmDelete = false;
    bool         mViewerShowUI  = true;
    void         ExecuteDelete();
    float        mViewZoom    = 1.0f;
    float        mViewPanX    = 0.0f;
    float        mViewPanY    = 0.0f;
    static constexpr float ZOOM_MIN  = 1.0f;
    static constexpr float ZOOM_MAX  = 8.0f;
    static constexpr float ZOOM_STEP = 0.05f;
    static constexpr float PAN_SPEED = 8.0f;
    void OpenViewer(int filteredIdx);
    void CloseViewer();
    void DrawViewer();
    void UpdateViewer(const Input& input);
    void UpdateVideoPlayback();
    bool OpenVideoItem(int filteredIdx, bool startAudio = true);
    void NavigateToItem(int nextFilteredIdx, bool resetZoom = false);

    VideoDecoder   mVideoDecoder;
    SDL_Texture*   mVideoTexture      = nullptr;
    bool           mVideoPlaying      = false;
    bool           mVideoPaused       = false;
    double         mFrameDelay        = 33.0;
    Uint32         mWallClockStartTime = 0;
    double         mWallClockStartPTS  = 0.0;
    double         mSeekAccum          = 0.0;

    bool           mClipMode              = false;
    double         mClipStartTime         = 0.0;
    double         mClipEndTime           = 0.0;
    bool           mClipMarkerEndActive   = false;
    bool           mClipPreviewPlaying    = false;
    Uint32         mClipPreviewStartTime  = 0;
    double         mClipPreviewStartPTS   = 0.0;
    Uint32         mClipSaveNotifEndTime  = 0;
    bool           mClipSaving            = false;
    ClipEncoder    mClipEncoder;
    float          mClipProgress          = 0.0f;
    SDL_atomic_t   mClipEncodingDone;
    bool           mClipEncodeSuccess     = false;
    std::thread    mClipThread;
    std::string    mClipOutputPath;
    std::string    mClipErrorMsg;
    void           EnterClipMode();
    void           ExitClipMode();
    void           UpdateClipMode(const Input& input);
    void           DrawClipMode();
    void           SaveClip();

    bool           mViewerSidePanel    = false;
    int            mViewerSidePanelSel = 0;
    Uint32         mSaveNotifEndTime   = 0;
    bool           mPendingRefresh     = false;
    void           SaveScreenshot();

    static constexpr int       NUM_THUMB_THREADS = 2;
    std::thread        mThumbThreads[NUM_THUMB_THREADS];
    std::atomic<bool>  mThumbRunning{false};
    std::mutex         mThumbMutex;
    void               ThumbWorker();
    void               FlushPendingSurfaces();
    void               StartThumbWorkers();

    void SaveConfig() const;
    void LoadConfig();
    std::string ComputeSdRoot() const;

    void DrawHeader();
    void DrawSidebar();
    void DrawGrid();
    void DrawFooter();
    void DrawFilterPanel();
    void DrawSortPanel();
    void DrawQuickAccessPanel();
    void DrawDeleteConfirmDialog(const std::string& title, int bw = 480, int bh = 200);

    void DrawSidebarItem(int idx, int x, int y, int size,
                         const std::string& icon, const std::string& label,
                         bool selected, bool hasCircle);

    static std::string FormatDuration(uint32_t sec);

    std::string GetCountStr() const;
    std::string GetSortStr()  const;
    std::string GetFilterStr() const;

    bool mSettingsDarkMode = false;

    std::string mScanDiagnostics;
    std::string mPathScreenshots;
    std::string mPathVideos;

    QRState         mQRState        = QRState::Inactive;
    int             mQRItemIdx      = -1;
    FileServer      mFileServer;
    QRCode          mQRCode;
    SDL_Texture*    mQRCodeTexture  = nullptr;
    std::string     mQRUrl;
    std::string     mQRFileName;
    void            StartQRTransfer(int filteredIdx);
    void            StartQRMultiTransfer(const std::vector<int>& filteredIndices);
    void            StopQRTransfer();
    void            UpdateQRTransfer(const Input& input);
    void            DrawQRTransfer();

    int             mPendingTransferIdx   = -1;
    TransferMode    mTransferMode         = TransferMode::None;
    bool            mTransferMultiSelect  = false;
    std::vector<bool> mTransferSelected;
    int             mTransferSelectCount  = 0;
    std::vector<std::string> mTransferFilePaths;

    bool           mTextOverlayActive     = false;
    std::string    mTextOverlayText;
    float          mTextOverlaySize       = 48.f;
    float          mTextOverlayPosX       = 0.f;
    float          mTextOverlayPosY       = 0.f;
    float          mTextOverlayAngle      = 0.f;
    int            mTextOverlayColorSel   = 0;
    bool           mTextOverlayAdvancedColor = false;
    SDL_Color      mTextOverlayColor      = {0xff, 0xff, 0xff, 0xff};

    int            mTextOverlayFocus      = 0;

    bool           mTextOverlayPlacing    = false;

    // Color wheel state
    float          mTextOverlayColorHue   = 0.f;
    float          mTextOverlayColorSat   = 1.f;
    float          mTextOverlayColorVal   = 1.f;
    float          mTextOverlayCursorX    = 1.f;
    float          mTextOverlayCursorY    = 0.f;

    SDL_Surface*   mTextOverlayOrigSurface  = nullptr;
    SDL_Surface*   mTextOverlayPreviewSurf   = nullptr;
    SDL_Texture*   mTextOverlayPreviewTex   = nullptr;
    float          mTextOverlayDownscale     = 1.0f;
    int            mTextOverlayUpdateThrottle = 0;

    void EnterTextOverlay();
    void ExitTextOverlay();
    void UpdateTextOverlay(const Input& input);
    void DrawTextOverlay();
    void RenderTextOverlayPreview();
    void SaveTextOverlayImage();

    static constexpr SDL_Color TEXT_PRESET_COLORS[10] = {
        {0xff, 0xff, 0xff, 0xff}, // white
        {0x00, 0x00, 0x00, 0xff}, // black
        {0xff, 0x00, 0x00, 0xff}, // red
        {0x00, 0xff, 0x00, 0xff}, // green
        {0x00, 0x00, 0xff, 0xff}, // blue
        {0xff, 0xff, 0x00, 0xff}, // yellow
        {0xff, 0x00, 0xff, 0xff}, // magenta
        {0x00, 0xff, 0xff, 0xff}, // cyan
        {0xff, 0x80, 0x00, 0xff}, // orange
        {0x80, 0x40, 0x00, 0xff}, // brown
    };

    Uint32       mLastAutoRefreshTime = 0;
    time_t       mLastDirMtimeScreenshots = 0;
    time_t       mLastDirMtimeVideos     = 0;
    void         CheckAutoRefresh();

    void HandleTouch(const Input& input);
    int  TouchHitTestGrid(int tx, int ty) const;
    int  TouchHitTestSidebar(int tx, int ty) const;
    bool TouchHitRect(int tx, int ty, int rx, int ry, int rw, int rh) const;

    void UpdatePointerPosition(const Input& input);
    void HandlePointer(const Input& input);

    void ResumeVideoAudio();
    void OpenSidebarOverlay(int idx);
    bool HandleQRTransferClick(int px, int py);
    bool HandleDeleteConfirmClick(int px, int py);
    bool HandleViewerSidePanelClick(int px, int py);
    bool HandleViewerFooterClick(int px, int py);
    bool HandleOverlayClick(int px, int py);
    void HandleGridItemClick(int filteredIdx);
    bool HandleFooterBackClick(int px, int py);
    bool mPointerDraw    = false;
    bool mPointerAClickWasDown = false;
    bool mPointerConsumedClick = false;
    int  mPointerScreenX = 0;
    int  mPointerScreenY = 0;
    bool  mVideoBarSeeking  = false;
    double mVideoBarSeekTarget = 0.0;

    bool mTouchViewerSwipeHandled = false;

    // Double-tap detection for overlays
    int    mTouchLastOverlaySel = -1;
    Uint32 mTouchLastOverlayTapTime = 0;
    static constexpr Uint32 DOUBLE_TAP_MS = 350;

    int mTouchGridPressIdx = -1;
    float mTouchGridScrollAccum = 0.f;
};

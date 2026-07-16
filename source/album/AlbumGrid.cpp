#include "Album.hpp"
#include "../ui/Glyphs.hpp"
#include <algorithm>
#include <whb/log.h>

void Album::EnterMultiSelect() {
    mMultiSelect = true;
    mSelected.assign(mFiltered.size(), false);
    mSidebarFocus = false;
    WHBLogPrintf("[ALBUM] Multi-select enabled");
}

void Album::ExitMultiSelect(bool keepCursor) {
    mMultiSelect = false;
    mSelected.clear();
    if (!keepCursor) mSidebarFocus = true;
    WHBLogPrintf("[ALBUM] Multi-select disabled");
}

void Album::ExecuteMultiDelete() {
    if (mSelected.empty()) return;

    StopThumbWorkers();

    std::vector<int> toDelete;
    for (int fi = 0; fi < (int)mFiltered.size(); fi++) {
        if (mSelected[fi]) toDelete.push_back(mFiltered[fi]);
    }
    WHBLogPrintf("[ALBUM] Multi-delete: removing %d items", (int)toDelete.size());

    mMultiSelect = false;
    mSelected.clear();

    std::sort(toDelete.begin(), toDelete.end(), std::greater<int>());

    for (int idx : toDelete) {
        auto& item = mAllItems[idx];
        remove(item.path.c_str());
        if (item.thumbnail)      Gfx::DestroyTexture(item.thumbnail);
        if (item.pendingSurface) SDL_FreeSurface(item.pendingSurface);
    }

    for (int idx : toDelete)
        mAllItems.erase(mAllItems.begin() + idx);

    for (auto& mi : mAllItems)
        if (mi.thumbRequested && !mi.thumbnail) mi.thumbRequested = false;

    StartThumbWorkers();

    ApplyFilterSort();
    mGridCursor = 0;
    mScrollRow  = 0;
    mSidebarFocus = true;
    WHBLogPrintf("[ALBUM] Multi-delete done — %d items remain", (int)mAllItems.size());
}

void Album::UpdateOverlay(const Input& input) {
    switch (mOverlay) {
        case Overlay::Filter:        UpdateFilterOverlay(input);        break;
        case Overlay::Sort:          UpdateSortOverlay(input);          break;
        case Overlay::DeleteConfirm: UpdateDeleteConfirmOverlay(input); break;
        case Overlay::TransferMode:  UpdateTransferModeOverlay(input);  break;
        case Overlay::QuickAccess:   UpdateQuickAccessOverlay(input);   break;
        case Overlay::Settings:
            if (input.IsPressed(Input::BUTTON_B) || (input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick))
                CloseOverlay();
            break;
        default: break;
    }
}

void Album::UpdateFilterOverlay(const Input& input) {
    int total = 3 + 1 + (int)mAppNames.size();
    auto wrapSel = [&](int dir) {
        int n = mOverlaySel;
        do { n = (n + dir + total) % total; } while (n == 3);
        mOverlaySel = n;
    };
    if (input.IsPressed(Input::BUTTON_DOWN))  wrapSel(1);
    if (input.IsPressed(Input::BUTTON_UP))    wrapSel(-1);
    if (input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) {
        if (mOverlaySel < 3) {
            mFilter = (FilterMode)mOverlaySel;
            mFilterApp.clear();
        } else {
            mFilter = FilterMode::All;
            mFilterApp = (mOverlaySel >= 4) ? mAppNames[mOverlaySel - 4] : "";
        }
        ApplyFilterSort();
        SaveConfig();
    }
    if (input.IsPressed(Input::BUTTON_B) || input.IsPressed(Input::BUTTON_LEFT)) CloseOverlay();
}

void Album::UpdateSortOverlay(const Input& input) {
    int numOpts = 2;
    if (input.IsPressed(Input::BUTTON_DOWN))  mOverlaySel = (mOverlaySel + 1) % numOpts;
    if (input.IsPressed(Input::BUTTON_UP))    mOverlaySel = (mOverlaySel + numOpts - 1) % numOpts;
    if (input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) {
        mSort = (SortOrder)mOverlaySel;
        ApplyFilterSort();
        SaveConfig();
    }
    if (input.IsPressed(Input::BUTTON_B) || input.IsPressed(Input::BUTTON_LEFT)) CloseOverlay();
}

void Album::UpdateDeleteConfirmOverlay(const Input& input) {
    if (input.IsPressed(Input::BUTTON_LEFT) || input.IsPressed(Input::BUTTON_RIGHT)) {
        mOverlaySel = 1 - mOverlaySel;
    }
    if (input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) {
        if (mOverlaySel == 0) {
            CloseOverlay();
            if (mMultiSelect) {
                ExecuteMultiDelete();
            } else {
                ExecuteDelete();
            }
        } else {
            CloseOverlay();
        }
    }
    if (input.IsPressed(Input::BUTTON_B)) {
        CloseOverlay();
    }
}

void Album::UpdateTransferModeOverlay(const Input& input) {
    int numOpts = 2;
    if (input.IsPressed(Input::BUTTON_DOWN)) mOverlaySel = (mOverlaySel + 1) % numOpts;
    if (input.IsPressed(Input::BUTTON_UP))   mOverlaySel = (mOverlaySel + numOpts - 1) % numOpts;
    if (input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) {
        if (mOverlaySel == 0) {
            mTransferMode = TransferMode::Single;
            CloseOverlay();
            if (mPendingTransferIdx >= 0) {
                StartQRTransfer(mPendingTransferIdx);
            } else {
                mSidebarFocus = false;
            }
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
    if (input.IsPressed(Input::BUTTON_B)) CloseOverlay();
}

void Album::DrawHeader() {
    SDL_Color hdrBg  = mMultiSelect ? SDL_Color{0xff, 0xe8, 0xe8, 0xff} : Gfx::COLOR_HEADER_BG;
    SDL_Color hdrSep = mMultiSelect ? Gfx::COLOR_DELETE : Gfx::COLOR_SEPARATOR;
    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, HEADER_H, hdrBg);
    Gfx::DrawRectFilled(0, HEADER_H - 1, Gfx::SCREEN_WIDTH, 1, hdrSep);

    int tx = 30, ty = HEADER_H / 2;
    Gfx::DrawRectRounded(tx, ty - 14, 28, 22, 4, Gfx::COLOR_TEXT);
    Gfx::DrawCircleFilled(tx + 14, ty - 3, 6, Gfx::COLOR_HEADER_BG);
    Gfx::Print(tx + 38, ty, 30, Gfx::COLOR_TEXT, "Album",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    if (mMultiSelect) {
        int selCount = 0;
        for (bool s : mSelected) if (s) selCount++;
        std::string selHint = std::to_string(selCount) + " selected   "
            + Glyphs::A + " Select   " + Glyphs::X + " Delete";
        Gfx::Print(Gfx::SCREEN_WIDTH - 30, ty, 26, Gfx::COLOR_DELETE,
                   selHint, Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
    } else if (mTransferMultiSelect) {
        std::string selHint = std::to_string(mTransferSelectCount) + "/5 selected   "
            + Glyphs::Y + " Send  " + Glyphs::A + " Toggle";
        Gfx::Print(Gfx::SCREEN_WIDTH - 30, ty, 26, Gfx::COLOR_ACCENT,
                   selHint, Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
    } else if (mTransferMode == TransferMode::Single) {
        std::string selHint = std::string(Glyphs::A) + " Select file to send   "
            + Glyphs::B + " Cancel";
        Gfx::Print(Gfx::SCREEN_WIDTH - 30, ty, 26, Gfx::COLOR_ACCENT,
                   selHint, Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
    } else {
        std::string info = GetSortStr() + "  |  " + GetFilterStr() + "  (" + GetCountStr() + ")";
        Gfx::Print(Gfx::SCREEN_WIDTH - 30, ty, 26, Gfx::COLOR_TEXT_DIM, info,
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
    }
}

void Album::DrawFooter() {
    int y = Gfx::SCREEN_HEIGHT - FOOTER_H;
    SDL_Color ftrBg  = Gfx::COLOR_FOOTER_BG;
    SDL_Color ftrSep = Gfx::COLOR_SEPARATOR;
    if (mMultiSelect) { ftrBg = {0xff, 0xe8, 0xe8, 0xff}; ftrSep = Gfx::COLOR_DELETE; }
    else if (mTransferMultiSelect) { ftrBg = {0xe0, 0xf4, 0xfb, 0xff}; ftrSep = Gfx::COLOR_ACCENT; }
    else if (mTransferMode == TransferMode::Single) { ftrBg = {0xe0, 0xf4, 0xfb, 0xff}; ftrSep = Gfx::COLOR_ACCENT; }
    Gfx::DrawRectFilled(0, y, Gfx::SCREEN_WIDTH, FOOTER_H, ftrBg);
    Gfx::DrawRectFilled(0, y, Gfx::SCREEN_WIDTH, 1, ftrSep);

    int cx = Gfx::SCREEN_WIDTH - 30;
    int cy = y + FOOTER_H / 2;
    constexpr int ICON_SZ = 28;
    constexpr int LBL_SZ  = 24;
    constexpr int GAP     = 8;

    if (mTransferMultiSelect) {
        int bx = cx - 80;
        Gfx::PrintIcon(bx, cy, ICON_SZ, Gfx::COLOR_BTN_B, Glyphs::B, Gfx::ALIGN_CENTER);
        Gfx::Print(bx + ICON_SZ / 2 + GAP, cy, LBL_SZ, Gfx::COLOR_TEXT, "Cancel",
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    } else if (mTransferMode == TransferMode::Single) {
        int bx = cx - 80;
        Gfx::PrintIcon(bx, cy, ICON_SZ, Gfx::COLOR_BTN_B, Glyphs::B, Gfx::ALIGN_CENTER);
        Gfx::Print(bx + ICON_SZ / 2 + GAP, cy, LBL_SZ, Gfx::COLOR_TEXT, "Cancel",
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    } else {
        int bx = cx - 180;
        Gfx::PrintIcon(bx, cy, ICON_SZ, Gfx::COLOR_BTN_B, Glyphs::B, Gfx::ALIGN_CENTER);
        Gfx::Print(bx + ICON_SZ / 2 + GAP, cy, LBL_SZ, Gfx::COLOR_TEXT, "Back",
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

        bx = cx - 80;
        Gfx::PrintIcon(bx, cy, ICON_SZ, Gfx::COLOR_BTN_A, Glyphs::A, Gfx::ALIGN_CENTER);
        Gfx::Print(bx + ICON_SZ / 2 + GAP, cy, LBL_SZ, Gfx::COLOR_TEXT, "OK",
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }
}

void Album::DrawSidebarItem(int idx, int x, int y, int size,
                             const std::string& /*icon*/, const std::string& label,
                             bool selected, bool hasCircle) {
    int cx = x + size / 2;
    int cy = y + size / 2;

    if (selected && hasCircle) {
        Gfx::DrawCircleFilled(cx, cy, size / 2 + 4, Gfx::COLOR_SIDEBAR_SEL);
        Gfx::DrawRectRoundedOutline(cx - size / 2 - 4, cy - size / 2 - 4,
                                    size + 8, size + 8, size / 2 + 4,
                                    Gfx::COLOR_ACCENT, 3);
        int bx = x + size + 14;
        int bw = Gfx::GetTextWidth(26, label) + 24;
        int bh = 40;
        int by = cy - bh / 2;
        Gfx::DrawRectRounded(bx, by, bw, bh, 8, Gfx::COLOR_SIDEBAR_SEL);
        Gfx::DrawRectRoundedOutline(bx, by, bw, bh, 8, Gfx::COLOR_ACCENT, 2);
        Gfx::Print(bx + bw / 2, cy, 26, Gfx::COLOR_ACCENT, label, Gfx::ALIGN_CENTER);
    }

    SDL_Color iconColor = selected ? Gfx::COLOR_ACCENT : Gfx::COLOR_TEXT_DIM;

    int ix = cx - 11, iy = cy - 11, iw = 22, ih = 22;
    switch (idx) {
        case 0:
            Gfx::DrawRectFilled(ix, iy, iw, ih, iconColor);
            Gfx::DrawRectFilled(ix + 3, iy + 3, iw - 6, ih - 6, Gfx::COLOR_SIDEBAR_BG);
            Gfx::DrawLine(ix + 5, iy + 14, ix + 9, iy + 17, iconColor);
            Gfx::DrawLine(ix + 9, iy + 17, ix + 17, iy + 5, iconColor);
            break;
        case 1:
        {
            for (int row = 0; row <= 13; row++) {
                int halfW = 11 - row * 10 / 13;
                Gfx::DrawLine(cx - halfW, iy + row, cx + halfW, iy + row, iconColor);
            }
            Gfx::DrawRectFilled(cx - 1, iy + 13, 3, 9, iconColor);
            break;
        }
        case 2:
        {
            for (int row = 0; row <= 8; row++) {
                int halfW = row * 5 / 8;
                if (halfW < 1) halfW = 1;
                Gfx::DrawLine(cx - halfW, iy + 2 + row, cx + halfW, iy + 2 + row, iconColor);
            }
            for (int row = 0; row <= 8; row++) {
                int halfW = (8 - row) * 5 / 8;
                if (halfW < 1) halfW = 1;
                Gfx::DrawLine(cx - halfW, cy + 2 + row, cx + halfW, cy + 2 + row, iconColor);
            }
            break;
        }
        case 3:
        {
            int teeth = 8;
            for (int t = 0; t < teeth; t++) {
                float angle = t * 3.14159f / (teeth / 2.0f);
                int tx = cx + (int)(std::cos(angle) * 10) - 2;
                int ty = cy + (int)(std::sin(angle) * 10) - 2;
                Gfx::DrawRectFilled(tx, ty, 5, 5, iconColor);
            }
            Gfx::DrawCircleFilled(cx, cy, 7, iconColor);
            Gfx::DrawCircleFilled(cx, cy, 3, Gfx::COLOR_SIDEBAR_BG);
            break;
        }
        case 4:
            Gfx::DrawRectRoundedOutline(ix, iy, iw, ih, 3, iconColor, 2);
            Gfx::DrawRectFilled(ix + 3, iy + 3, 6, 6, iconColor);
            Gfx::DrawRectFilled(ix + 3, iy + ih - 9, 6, 6, iconColor);
            Gfx::DrawRectFilled(ix + iw - 9, iy + 3, 6, 6, iconColor);
            Gfx::DrawRectFilled(ix + iw - 9, iy + ih - 9, 6, 6, iconColor);
            Gfx::DrawRectFilled(ix + iw/2-2, iy + iw/2-2, 6, 6, iconColor);
            break;
    }
}

void Album::DrawSidebar() {
    Gfx::DrawRectFilled(0, 0, SIDEBAR_W, Gfx::SCREEN_HEIGHT, Gfx::COLOR_SIDEBAR_BG);
    Gfx::DrawRectFilled(SIDEBAR_W - 1, 0, 1, Gfx::SCREEN_HEIGHT, Gfx::COLOR_SEPARATOR);

    int iconSize = 44;
    int spacing  = 80;
    int startY   = HEADER_H + 40;

    static const char* labels[] = { "Quick Actions", "Filter", "Sort", "Settings" };

    for (int i = 0; i < 4; i++) {
        int ix = (SIDEBAR_W - iconSize) / 2;
        int iy = startY + i * spacing;
        bool selected = mSidebarFocus && (mSidebarSel == i) && mOverlay == Overlay::None;
        DrawSidebarItem(i, ix, iy, iconSize, "", labels[i], selected, true);
    }
}

void Album::DrawGrid() {
    if (mFiltered.empty()) {
        int cx = GRID_X + GRID_W / 2;
        int cy = GRID_Y + (GRID_H > 300 ? GRID_H / 2 - 80 : GRID_Y + 30);
        Gfx::Print(cx, cy, 30, Gfx::COLOR_TEXT_DIM, "No items found.", Gfx::ALIGN_CENTER);
        cy += 44;
        Gfx::Print(cx, cy, 22, Gfx::COLOR_TEXT_LIGHT,
                   "Screenshots: " + mPathScreenshots, Gfx::ALIGN_CENTER);
        cy += 32;
        Gfx::Print(cx, cy, 22, Gfx::COLOR_TEXT_LIGHT,
                   "Videos:      " + mPathVideos, Gfx::ALIGN_CENTER);
        cy += 44;
        if (!mScanDiagnostics.empty()) {
            std::string diag = mScanDiagnostics;
            size_t pos = 0;
            while (pos < diag.size()) {
                size_t nl = diag.find('\n', pos);
                std::string line = (nl == std::string::npos)
                    ? diag.substr(pos) : diag.substr(pos, nl - pos);
                if (!line.empty()) {
                    Gfx::Print(cx, cy, 20, {0xde, 0x3b, 0x2e, 0xff}, line, Gfx::ALIGN_CENTER);
                    cy += 28;
                }
                pos = (nl == std::string::npos) ? diag.size() : nl + 1;
            }
        }
        return;
    }

    int totalRows = ((int)mFiltered.size() + COLS - 1) / COLS;
    int endRow    = std::min(mScrollRow + ROWS_VIS + 1, totalRows);

    int cellW = THUMB_W + THUMB_PAD;
    int cellH = THUMB_H + THUMB_PAD;

    int gridContentW = COLS * cellW;
    int gridOffsetX  = GRID_X + (GRID_W - gridContentW) / 2;

    for (int row = mScrollRow; row < endRow; row++) {
        for (int col = 0; col < COLS; col++) {
            int fi = row * COLS + col;
            if (fi >= (int)mFiltered.size()) break;

            int x = gridOffsetX + col * cellW;
            int y = GRID_Y + (row - mScrollRow) * cellH;

            if (y + cellH > GRID_Y + GRID_H) continue;

            int idx = mFiltered[fi];
            auto& item = mAllItems[idx];

            bool isCursor = (!mSidebarFocus && mGridCursor == fi);

            Gfx::DrawRectFilled(x + 4, y + 4, THUMB_W, THUMB_H, Gfx::COLOR_SHADOW);

            SDL_Color bg = {0x33, 0x33, 0x33, 0xff};
            Gfx::DrawRectFilled(x + (cellW - THUMB_W) / 2, y + (cellH - THUMB_H) / 2,
                               THUMB_W, THUMB_H, bg);

            if (item.thumbnail) {
                Gfx::DrawTextureCover(item.thumbnail, x + (cellW - THUMB_W) / 2,
                                     y + (cellH - THUMB_H) / 2, THUMB_W, THUMB_H);
            }

            if (item.type == MediaType::Video) {
                std::string dur = item.durationSec > 0
                    ? FormatDuration(item.durationSec) : "Video";
                int bw = Gfx::GetTextWidth(22, dur) + 14;
                int bh = 28;
                int bx = x + THUMB_W - bw - 4;
                int by = y + THUMB_H - bh - 4;
                Gfx::DrawRectRounded(bx, by, bw, bh, 4, Gfx::COLOR_VIDEO_BADGE);
                Gfx::Print(bx + bw / 2, by + bh / 2, 22, Gfx::COLOR_WHITE,
                           dur, Gfx::ALIGN_CENTER);
            }

            if (mMultiSelect && fi < (int)mSelected.size()) {
                int cx2 = x + THUMB_W - 20;
                int cy2 = y + 14;
                bool sel = mSelected[fi];
                SDL_Color color = sel ? Gfx::COLOR_DELETE : Gfx::COLOR_TEXT_DIM;

                Gfx::DrawCircleFilled(cx2, cy2, 16, color);
                Gfx::DrawCircleFilled(cx2, cy2, 13, {0xff, 0xff, 0xff, 0xff});
                if (sel) {
                    Gfx::DrawLine(cx2 - 7, cy2,     cx2 - 2, cy2 + 6, color);
                    Gfx::DrawLine(cx2 - 2, cy2 + 6, cx2 + 7, cy2 - 5, color);
                }
            }

            if (mTransferMultiSelect && fi < (int)mTransferSelected.size()) {
                bool sel = mTransferSelected[fi];
                int cx2 = x + THUMB_W - 20;
                int cy2 = y + 14;
                SDL_Color color = sel ? Gfx::COLOR_ACCENT : Gfx::COLOR_TEXT_DIM;
                Gfx::DrawCircleFilled(cx2, cy2, 16, color);
                Gfx::DrawCircleFilled(cx2, cy2, 13, {0xff, 0xff, 0xff, 0xff});
                if (sel) {
                    Gfx::DrawLine(cx2 - 6, cy2,     cx2 - 2, cy2 + 5, color);
                    Gfx::DrawLine(cx2 - 2, cy2 + 5, cx2 + 6, cy2 - 5, color);
                }
            }

            if (isCursor) {
                int thumbX = x + (cellW - THUMB_W) / 2;
                int thumbY = y + (cellH - THUMB_H) / 2;
                SDL_Color ringColor = Gfx::COLOR_SELECTED_RING;
                if (mMultiSelect) ringColor = Gfx::COLOR_DELETE;
                else if (mTransferMultiSelect) ringColor = Gfx::COLOR_ACCENT;
                Gfx::DrawRectOutline(thumbX - 6, thumbY - 6, THUMB_W + 12, THUMB_H + 12,
                                     ringColor, 8);
            }
        }
    }

    if (totalRows > ROWS_VIS) {
        int sbX = GRID_X + GRID_W + 6;
        int sbH = GRID_H;
        float thumb  = (float)ROWS_VIS / totalRows;
        float offset = (float)mScrollRow / totalRows;
        int   tbH    = (int)(sbH * thumb);
        int   tbY    = GRID_Y + (int)(sbH * offset);
        Gfx::DrawRectFilled(sbX, GRID_Y, 6, sbH, Gfx::COLOR_SEPARATOR);
        Gfx::DrawRectFilled(sbX, tbY,    6, tbH, Gfx::COLOR_ACCENT);
    }
}

void Album::DrawOverlay() {
    if (mOverlay == Overlay::None) return;

    if (mOverlay == Overlay::Filter) {
        DrawFilterPanel();
        return;
    }

    if (mOverlay == Overlay::Sort) {
        DrawSortPanel();
        return;
    }

    if (mOverlay == Overlay::QuickAccess) {
        DrawQuickAccessPanel();
        return;
    }

    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT,
                        {0, 0, 0, 120});

    int bw = 400, bh = 0;
    std::vector<std::string> opts;
    std::string title;

    if (mOverlay == Overlay::Settings) {
        title = "Settings";
        opts  = { "About: Wii U Album v1.0" };
    } else if (mOverlay == Overlay::TransferMode) {
        title = "Transfer to Device";
        opts  = { "Send single", "Send multiple" };
    } else if (mOverlay == Overlay::DeleteConfirm) {
        int n = 0;
        for (bool s : mSelected) if (s) n++;
        if (!mMultiSelect && mViewerItem >= 0) n++;
        title = "Delete " + std::to_string(n) + " item"
              + (n != 1 ? "s" : "") + "?";

        DrawDeleteConfirmDialog(title);
        return;
    }

    int itemH = 56;
    bh = 60 + (int)opts.size() * itemH + 16;
    int bx = (Gfx::SCREEN_WIDTH - bw) / 2;
    int by = (Gfx::SCREEN_HEIGHT - bh) / 2;

    Gfx::DrawRectRounded(bx, by, bw, bh, 12, {0xff, 0xff, 0xff, 0xff});
    Gfx::DrawRectRoundedOutline(bx, by, bw, bh, 12, Gfx::COLOR_ACCENT, 2);

    Gfx::Print(bx + bw / 2, by + 30, 30, Gfx::COLOR_TEXT, title, Gfx::ALIGN_CENTER);
    Gfx::DrawRectFilled(bx + 16, by + 54, bw - 32, 1, Gfx::COLOR_SEPARATOR);

    for (int i = 0; i < (int)opts.size(); i++) {
        int oy = by + 60 + i * itemH;
        if (i == mOverlaySel) {
            Gfx::DrawRectRounded(bx + 8, oy + 4, bw - 16, itemH - 8, 8,
                                 Gfx::COLOR_SIDEBAR_SEL);
        }
        SDL_Color tc = (i == mOverlaySel) ? Gfx::COLOR_ACCENT : Gfx::COLOR_TEXT;
        Gfx::Print(bx + bw / 2, oy + itemH / 2, 26, tc, opts[i], Gfx::ALIGN_CENTER);
    }
}

void Album::DrawDeleteConfirmDialog(const std::string& title, int bw, int bh) {
    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT,
                        {0, 0, 0, 140});

    int bx = (Gfx::SCREEN_WIDTH  - bw) / 2;
    int by = (Gfx::SCREEN_HEIGHT - bh) / 2;

    Gfx::DrawRectRounded(bx, by, bw, bh, 12, {0xff, 0xff, 0xff, 0xff});
    Gfx::DrawRectRoundedOutline(bx, by, bw, bh, 12, Gfx::COLOR_ACCENT, 2);

    Gfx::Print(bx + bw / 2, by + 44, 28, Gfx::COLOR_TEXT,
               title, Gfx::ALIGN_CENTER);

    int btnY = by + 110;
    int gap  = 40;
    int btnW = 160, btnH = 52;

    const char* labels[2] = { "Delete", "Cancel" };
    SDL_Color colors[2]   = { {0xde, 0x3b, 0x2e, 0xff}, Gfx::COLOR_TEXT_DIM };

    for (int i = 0; i < 2; i++) {
        int btnX = bx + (bw - btnW * 2 - gap) / 2 + i * (btnW + gap);
        bool sel = (i == mOverlaySel);
        Gfx::DrawRectRounded(btnX, btnY, btnW, btnH, 8,
                             sel ? colors[i] : Gfx::COLOR_SIDEBAR_SEL);
        if (sel) {
            Gfx::DrawRectRoundedOutline(btnX, btnY, btnW, btnH, 8,
                                       Gfx::COLOR_ACCENT, 4);
        }
        Gfx::Print(btnX + btnW / 2, btnY + btnH / 2, 26,
                   sel ? Gfx::COLOR_WHITE : colors[i],
                   labels[i], Gfx::ALIGN_CENTER);
    }
}

void Album::DrawFilterPanel() {
    const int PW = 280;
    int startY = HEADER_H + 40;
    int spacing = 80;
    int py = startY + 1 * spacing;
    int itemH = 42;
    int numItems = 3 + 1 + (int)mAppNames.size();
    int sepIdx = 3;
    int ph = 20 + numItems * itemH + 16;
    if (py + ph > Gfx::SCREEN_HEIGHT - 10)
        py = Gfx::SCREEN_HEIGHT - 10 - ph;
    if (py < 10) py = 10;

    int px = SIDEBAR_W + 4;

    Gfx::DrawRectFilled(px + 3, py + 3, PW, ph, {0, 0, 0, 80});
    Gfx::DrawRectRounded(px, py, PW, ph, 8, {0xf0, 0xf0, 0xf0, 0xff});
    Gfx::DrawRectRoundedOutline(px, py, PW, ph, 8, Gfx::COLOR_ACCENT, 2);

    int yo = py + 10;
    for (int i = 0; i < numItems; i++) {
        if (i == sepIdx) {
            yo += 8;
            Gfx::DrawRectFilled(px + 12, yo, PW - 24, 1, {0xcc, 0xcc, 0xcc, 0xff});
            yo += 8;
            continue;
        }

        std::string label;
        bool active = false;
        if (i == 0)      { label = "All Media";     active = (mFilter == FilterMode::All && mFilterApp.empty()); }
        else if (i == 1) { label = "Screenshots";   active = (mFilter == FilterMode::Screenshots && mFilterApp.empty()); }
        else if (i == 2) { label = "Videos";        active = (mFilter == FilterMode::Videos && mFilterApp.empty()); }
        else {
            label = mAppNames[i - 4];
            active = (mFilterApp == label);
        }

        const int maxLabelW = PW - 14 - 28;
        std::string displayLabel = label;
        if (Gfx::GetTextWidth(24, label) > maxLabelW) {
            while (!displayLabel.empty() && Gfx::GetTextWidth(24, displayLabel + "\u2026") > maxLabelW)
                displayLabel.pop_back();
            displayLabel += "\u2026";
        }

        bool sel = (i == mOverlaySel);
        SDL_Color bg = sel ? Gfx::COLOR_ACCENT : Gfx::COLOR_SIDEBAR_SEL;
        SDL_Color tc = (sel && active) ? Gfx::COLOR_WHITE : (active ? Gfx::COLOR_ACCENT : (sel ? Gfx::COLOR_WHITE : Gfx::COLOR_TEXT));

        if (sel || active) {
            Gfx::DrawRectRounded(px + 6, yo, PW - 12, itemH - 4, 6, bg);
        }

        Gfx::Print(px + 14, yo + itemH / 2 - 2, 24, tc, displayLabel, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

        SDL_Color checkColor = (sel && active) ? Gfx::COLOR_WHITE : (active ? Gfx::COLOR_ACCENT : (sel ? Gfx::COLOR_WHITE : Gfx::COLOR_TEXT_DIM));
        Gfx::Print(px + PW - 14, yo + itemH / 2 - 2, 22, checkColor,
                   active ? "[x]" : "[ ]",
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
        yo += itemH;
    }
}

void Album::UpdateQuickAccessOverlay(const Input& input) {
    int numOpts = 3;
    if (input.IsPressed(Input::BUTTON_DOWN))  mOverlaySel = (mOverlaySel + 1) % numOpts;
    if (input.IsPressed(Input::BUTTON_UP))    mOverlaySel = (mOverlaySel + numOpts - 1) % numOpts;
    if (input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) {
        if (mOverlaySel == 0) {
            CloseOverlay();
            Refresh();
        } else if (mOverlaySel == 1) {
            mPendingTransferIdx = -1;
            CloseOverlay();
            mOverlaySel = 0;
            mOverlay = Overlay::TransferMode;
        } else if (mOverlaySel == 2) {
            CloseOverlay();
            if (!mFiltered.empty()) EnterMultiSelect();
        }
    }
    if (input.IsPressed(Input::BUTTON_B) || input.IsPressed(Input::BUTTON_LEFT)) CloseOverlay();
}

void Album::DrawQuickAccessPanel() {
    const int PW = 280;
    int startY = HEADER_H + 40;
    int spacing = 80;
    int py = startY + 0 * spacing;
    int itemH = 42;
    int numItems = 3;
    int ph = 20 + numItems * itemH + 16;
    if (py + ph > Gfx::SCREEN_HEIGHT - 10)
        py = Gfx::SCREEN_HEIGHT - 10 - ph;
    if (py < 10) py = 10;

    int px = SIDEBAR_W + 4;

    Gfx::DrawRectFilled(px + 3, py + 3, PW, ph, {0, 0, 0, 80});
    Gfx::DrawRectRounded(px, py, PW, ph, 8, {0xf0, 0xf0, 0xf0, 0xff});
    Gfx::DrawRectRoundedOutline(px, py, PW, ph, 8, Gfx::COLOR_ACCENT, 2);

    int yo = py + 10;
    for (int i = 0; i < numItems; i++) {
        std::string label;
        if (i == 0) label = "Refresh";
        else if (i == 1) label = "Transfer to Device";
        else label = "Delete";

        bool sel = (i == mOverlaySel);
        SDL_Color bg = sel ? Gfx::COLOR_ACCENT : Gfx::COLOR_SIDEBAR_SEL;
        SDL_Color tc = sel ? Gfx::COLOR_WHITE : Gfx::COLOR_TEXT;

        if (sel) {
            Gfx::DrawRectRounded(px + 6, yo, PW - 12, itemH - 4, 6, bg);
        }

        Gfx::Print(px + 14, yo + itemH / 2 - 2, 24, tc, label, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        yo += itemH;
    }
}

void Album::DrawSortPanel() {
    const int PW = 280;
    int startY = HEADER_H + 40;
    int spacing = 80;
    int py = startY + 2 * spacing;
    int itemH = 42;
    int numItems = 2;
    int ph = 20 + numItems * itemH + 16;
    if (py + ph > Gfx::SCREEN_HEIGHT - 10)
        py = Gfx::SCREEN_HEIGHT - 10 - ph;
    if (py < 10) py = 10;

    int px = SIDEBAR_W + 4;

    Gfx::DrawRectFilled(px + 3, py + 3, PW, ph, {0, 0, 0, 80});
    Gfx::DrawRectRounded(px, py, PW, ph, 8, {0xf0, 0xf0, 0xf0, 0xff});
    Gfx::DrawRectRoundedOutline(px, py, PW, ph, 8, Gfx::COLOR_ACCENT, 2);

    int yo = py + 10;
    for (int i = 0; i < numItems; i++) {
        std::string label = (i == 0) ? "Newest First" : "Oldest First";
        bool active = ((int)mSort == i);

        bool sel = (i == mOverlaySel);
        SDL_Color bg = sel ? Gfx::COLOR_ACCENT : Gfx::COLOR_SIDEBAR_SEL;
        SDL_Color tc = (sel && active) ? Gfx::COLOR_WHITE : (active ? Gfx::COLOR_ACCENT : (sel ? Gfx::COLOR_WHITE : Gfx::COLOR_TEXT));

        if (sel || active) {
            Gfx::DrawRectRounded(px + 6, yo, PW - 12, itemH - 4, 6, bg);
        }

        Gfx::Print(px + 14, yo + itemH / 2 - 2, 24, tc, label, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

        SDL_Color checkColor = (sel && active) ? Gfx::COLOR_WHITE : (active ? Gfx::COLOR_ACCENT : (sel ? Gfx::COLOR_WHITE : Gfx::COLOR_TEXT_DIM));
        Gfx::Print(px + PW - 14, yo + itemH / 2 - 2, 22, checkColor,
                   active ? "[x]" : "[ ]",
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
        yo += itemH;
    }
}

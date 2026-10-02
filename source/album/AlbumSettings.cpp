#include "Album.hpp"
#include "../ui/Glyphs.hpp"
#include <whb/log.h>

static const char* kCategories[] = { "Display", "Camera" };
static constexpr int NUM_CATEGORIES = 2;

static int GetItemCount(int cat) {
    switch (cat) {
        case 0: return 1;
        case 1: return 5;
        default: return 0;
    }
}

static constexpr int CAT_W       = 340;
static constexpr int CAT_ITEM_H  = 76;
static constexpr int CAT_START_Y = 72 + 16;

void Album::OpenSettings() {
    mSettingsOpen     = true;
    mSettingsCatFocus = true;
    mSettingsCatSel   = 0;
    mSettingsItemSel  = 0;
    WHBLogPrintf("[SETTINGS] Opened");
}

void Album::CloseSettings() {
    mSettingsOpen = false;
    mSidebarFocus = true;
    WHBLogPrintf("[SETTINGS] Closed");
}

void Album::UpdateSettings(const Input& input) {
    if (mSettingsCatFocus) {
        if (input.IsPressed(Input::BUTTON_UP))
            mSettingsCatSel = (mSettingsCatSel - 1 + NUM_CATEGORIES) % NUM_CATEGORIES;
        if (input.IsPressed(Input::BUTTON_DOWN))
            mSettingsCatSel = (mSettingsCatSel + 1) % NUM_CATEGORIES;

        if (input.IsPressed(Input::BUTTON_RIGHT)) {
            if (GetItemCount(mSettingsCatSel) > 0) {
                mSettingsCatFocus = false;
                mSettingsItemSel  = 0;
            }
        }

        if (input.IsPressed(Input::BUTTON_B) || input.IsPressed(Input::BUTTON_LEFT))
            CloseSettings();

    } else {
        int numItems = GetItemCount(mSettingsCatSel);

        if (input.IsPressed(Input::BUTTON_UP))
            mSettingsItemSel = (mSettingsItemSel - 1 + numItems) % numItems;
        if (input.IsPressed(Input::BUTTON_DOWN))
            mSettingsItemSel = (mSettingsItemSel + 1) % numItems;

        if (input.IsPressed(Input::BUTTON_LEFT) || input.IsPressed(Input::BUTTON_B))
            mSettingsCatFocus = true;

        if (input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) {
            if (mSettingsCatSel == 0 && mSettingsItemSel == 0) {
                // Dark mode toggle
                mSettingsDarkMode = !mSettingsDarkMode;
                Gfx::SetDarkMode(mSettingsDarkMode);
                SaveConfig();
                WHBLogPrintf("[SETTINGS] Dark mode: %s",
                             mSettingsDarkMode ? "on" : "off");
            } else if (mSettingsCatSel == 1) {
                switch (mSettingsItemSel) {
                    case 0: mSettingsCamMirror = !mSettingsCamMirror; break;
                    case 1: mSettingsCamFps    = (mSettingsCamFps >= 30) ? 15 : 30; break;
                    case 2: mSettingsCamGrid   = !mSettingsCamGrid; break;
                    case 3: mSettingsCamSource = mSettingsCamSource ? 0 : 1; break;
                    default: break;
                }
                SaveConfig();
            }
        }
    }
}

static void DrawGearIcon(int cx, int cy, int r, SDL_Color color) {
    const int teeth = 8;
    for (int t = 0; t < teeth; t++) {
        float angle = t * 3.14159f / (teeth / 2.0f);
        int tx = cx + (int)(std::cos(angle) * r) - 2;
        int ty = cy + (int)(std::sin(angle) * r) - 2;
        Gfx::DrawRectFilled(tx, ty, 5, 5, color);
    }
    Gfx::DrawCircleFilled(cx, cy, (int)(r * 0.65f), color);
    Gfx::DrawCircleFilled(cx, cy, (int)(r * 0.28f), Gfx::Theme().sidebarBg);
}

static void DrawToggle(int cx, int cy, bool on) {
    constexpr int TW = 68, TH = 34, R = 17;
    int tx = cx - TW / 2;
    int ty = cy - TH / 2;

    SDL_Color track = on ? Gfx::COLOR_ACCENT : Gfx::Theme().separator;
    Gfx::DrawRectRounded(tx, ty, TW, TH, R, track);

    int knobX = on ? tx + TW - R : tx + R;
    Gfx::DrawCircleFilled(knobX, cy, R - 4, Gfx::COLOR_WHITE);
}

void Album::DrawSettingsCategory(int catX, int catW,
                                 int contentY, int catItemH) {
    const auto& th = Gfx::Theme();

    Gfx::DrawRectFilled(catX, contentY, catW,
                        Gfx::SCREEN_HEIGHT - contentY - FOOTER_H, th.sidebarBg);
    Gfx::DrawRectFilled(catX + catW - 1, contentY, 1,
                        Gfx::SCREEN_HEIGHT - contentY - FOOTER_H, th.separator);

    for (int i = 0; i < NUM_CATEGORIES; i++) {
        int iy   = contentY + i * catItemH;
        bool sel = (i == mSettingsCatSel);

        if (sel) {
            SDL_Color rowBg = mSettingsCatFocus ? th.accentBg : th.sidebarSel;
            Gfx::DrawRectFilled(catX, iy, catW - 1, catItemH - 1, rowBg);

            SDL_Color stripe = mSettingsCatFocus ? Gfx::COLOR_ACCENT : th.separator;
            Gfx::DrawRectFilled(catX, iy, 4, catItemH - 1, stripe);
        }

        if (i < NUM_CATEGORIES - 1)
            Gfx::DrawRectFilled(catX + 16, iy + catItemH - 1,
                                catW - 32, 1, th.separator);

        SDL_Color tc = sel && mSettingsCatFocus ? Gfx::COLOR_ACCENT
                     : sel                      ? th.text
                                                 : th.textDim;
        Gfx::Print(catX + 28, iy + catItemH / 2, 28, tc,
                   kCategories[i], Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }
}

void Album::DrawSettingsContent(int contentX, int contentW, int contentY) {
    const auto& th = Gfx::Theme();
    constexpr int ITEM_H    = 80;
    constexpr int ITEM_PAD  = 14;

    auto drawRow = [&](int iy, bool sel) {
        if (sel) {
            Gfx::DrawRectRounded(contentX - 4, iy + 4,
                                 contentW + 8, ITEM_H - 8, 8, th.accentBg);
            Gfx::DrawRectRoundedOutline(contentX - 4, iy + 4,
                                        contentW + 8, ITEM_H - 8, 8,
                                        Gfx::COLOR_ACCENT, 3);
        } else {
            Gfx::DrawRectFilled(contentX, iy + ITEM_H - 1,
                                contentW, 1, th.separator);
        }
    };

    if (mSettingsCatSel == 0) {
        int iy  = contentY;
        bool sel = (!mSettingsCatFocus && mSettingsItemSel == 0);

        drawRow(iy, sel);

        Gfx::Print(contentX + ITEM_PAD,
                   iy + ITEM_H / 2, 28, th.text, "Dark Mode",
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

        SDL_Color valColor = mSettingsDarkMode ? Gfx::COLOR_ACCENT : th.textDim;
        std::string valLabel = mSettingsDarkMode ? "On" : "Off";
        int toggleCX = contentX + contentW - ITEM_PAD - 80;
        Gfx::Print(toggleCX - 14,
                   iy + ITEM_H / 2, 28, valColor, valLabel,
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);

        DrawToggle(contentX + contentW - ITEM_PAD - 36,
                   iy + ITEM_H / 2, mSettingsDarkMode);

    } else if (mSettingsCatSel == 1) {
        struct Row { const char* label; std::string value; bool isToggle; bool on; };
        Row rows[4] = {
            {"Mirror Preview",   mSettingsCamMirror ? "On" : "Off",
             true,  mSettingsCamMirror},
            {"Frame Rate",       std::to_string(mSettingsCamFps) + " fps",
             false, true},
            {"Grid Guides",      mSettingsCamGrid ? "On" : "Off",
             true,  mSettingsCamGrid},
            {"Camera Source",    mSettingsCamSource ? "USB / DLC" : "GamePad",
             false, true},
        };

        for (int i = 0; i < 4; i++) {
            int iy  = contentY + i * ITEM_H;
            bool sel = (!mSettingsCatFocus && mSettingsItemSel == i);

            drawRow(iy, sel);

            Gfx::Print(contentX + ITEM_PAD, iy + ITEM_H / 2, 28, th.text,
                       rows[i].label, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

            SDL_Color valColor = rows[i].on ? Gfx::COLOR_ACCENT : th.textDim;
            int valX = contentX + contentW - ITEM_PAD - 36;
            if (rows[i].isToggle) {
                DrawToggle(valX, iy + ITEM_H / 2, rows[i].on);
            } else {
                Gfx::Print(valX, iy + ITEM_H / 2, 28, valColor, rows[i].value,
                           Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
            }
        }

        Gfx::Print(contentX, contentY + 4 * ITEM_H + 16, 22, th.textDim,
                   "These are the defaults the camera starts with; they can also",
                   Gfx::ALIGN_LEFT);
        Gfx::Print(contentX, contentY + 4 * ITEM_H + 46, 22, th.textDim,
                   "be changed while the camera is open.",
                   Gfx::ALIGN_LEFT);
    }
}

void Album::DrawSettings() {
    const auto& th = Gfx::Theme();

    Gfx::Clear(th.bg);

    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, HEADER_H, th.headerBg);
    Gfx::DrawRectFilled(0, HEADER_H - 1, Gfx::SCREEN_WIDTH, 1, th.separator);

    DrawGearIcon(36, HEADER_H / 2, 14, th.text);

    Gfx::Print(64, HEADER_H / 2, 32, th.text, "Settings", 
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    int titleW = Gfx::GetTextWidth(32, "Settings");
    Gfx::Print(Gfx::SCREEN_WIDTH - titleW +100, HEADER_H / 2, 32, th.text, 
                "V2.0", 
                Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);

    DrawSettingsCategory(0, CAT_W, HEADER_H, CAT_ITEM_H);

    Gfx::DrawRectFilled(CAT_W, HEADER_H, 1,
                        Gfx::SCREEN_HEIGHT - HEADER_H - FOOTER_H, th.separator);

    constexpr int CONTENT_PAD  = 48;
    int contentX = CAT_W + CONTENT_PAD;
    int contentW = Gfx::SCREEN_WIDTH - contentX - CONTENT_PAD;
    int contentY = HEADER_H + 32;

    DrawSettingsContent(contentX, contentW, contentY);

    int fy = Gfx::SCREEN_HEIGHT - FOOTER_H;
    Gfx::DrawRectFilled(0, fy, Gfx::SCREEN_WIDTH, FOOTER_H, th.footerBg);
    Gfx::DrawRectFilled(0, fy, Gfx::SCREEN_WIDTH, 1, th.separator);

    constexpr int ICON_SZ = 28;
    constexpr int LBL_SZ  = 24;
    constexpr int GAP      = 8;
    int cy  = fy + FOOTER_H / 2;
    int bx  = Gfx::SCREEN_WIDTH - 30;

    if (!mSettingsCatFocus && GetItemCount(mSettingsCatSel) > 0) {
        Gfx::Print(bx, cy, LBL_SZ, th.text, "OK",
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
        bx -= Gfx::GetTextWidth(LBL_SZ, "OK") + GAP;
        Gfx::PrintIcon(bx, cy, ICON_SZ, Gfx::COLOR_BTN_A, Glyphs::A,
                       Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
        bx -= ICON_SZ + 32;
    }

    Gfx::Print(bx, cy, LBL_SZ, th.text, "Back",
               Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
    bx -= Gfx::GetTextWidth(LBL_SZ, "Back") + GAP;
    Gfx::PrintIcon(bx, cy, ICON_SZ, Gfx::COLOR_BTN_B, Glyphs::B,
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
}

#include "Album.hpp"
#include "../ui/Keyboard.hpp"
#include "../ui/Gfx.hpp"
#include "../ui/Glyphs.hpp"
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <SDL2_rotozoom.h>
#include <sys/stat.h>
#include <whb/log.h>
#include <coreinit/memory.h>
#include <cmath>

void Album::EnterTextOverlay() {
    if (mViewerItem < 0 || mViewerItem >= (int)mFiltered.size()) return;
    int idx = mFiltered[mViewerItem];
    if (mAllItems[idx].type != MediaType::Screenshot) return;

    if (mTextOverlayOrigSurface) SDL_FreeSurface(mTextOverlayOrigSurface);
    mTextOverlayOrigSurface = IMG_Load(mAllItems[idx].path.c_str());
    if (!mTextOverlayOrigSurface) {
        WHBLogPrintf("[ALBUM] TextOverlay: failed to load image");
        return;
    }

    int maxDim = (mTextOverlayOrigSurface->w > mTextOverlayOrigSurface->h) ? mTextOverlayOrigSurface->w : mTextOverlayOrigSurface->h;
    int targetDim = 1280;
    if (maxDim > targetDim) {
        mTextOverlayDownscale = (float)targetDim / maxDim;
    } else {
        mTextOverlayDownscale = 1.0f;
    }

    int pw = mTextOverlayOrigSurface->w;
    int ph = mTextOverlayOrigSurface->h;
    if (mTextOverlayDownscale < 1.0f) {
        pw = (int)(pw * mTextOverlayDownscale);
        ph = (int)(ph * mTextOverlayDownscale);
    }
    SDL_Surface* tempSurf = SDL_ConvertSurfaceFormat(mTextOverlayOrigSurface, SDL_PIXELFORMAT_RGBA32, 0);
    mTextOverlayPreviewSurf = NULL;
    if (tempSurf) {
        if (mTextOverlayDownscale < 1.0f) {
            float zx = (float)pw / tempSurf->w;
            float zy = (float)ph / tempSurf->h;
            mTextOverlayPreviewSurf = zoomSurface(tempSurf, zx, zy, SMOOTHING_ON);
        } else {
            mTextOverlayPreviewSurf = tempSurf;
            tempSurf = NULL;
        }
        if (tempSurf) SDL_FreeSurface(tempSurf);
    }

    mTextOverlayText = "";
    mTextOverlaySize = 48.0f;
    mTextOverlayPosX = mTextOverlayOrigSurface->w / 2.0f;
    mTextOverlayPosY = mTextOverlayOrigSurface->h / 2.0f;
    mTextOverlayAngle = 0.0f;
    mTextOverlayFocus = 0;
    mTextOverlayColorSel = 0;
    mTextOverlayColor = TEXT_PRESET_COLORS[0];
    mTextOverlayAdvancedColor = false;
    mTextOverlayPlacing = false;
    mTextOverlayColorHue = 0.0f;
    mTextOverlayColorSat = 1.0f;
    mTextOverlayColorVal = 1.0f;
    mTextOverlayCursorX = 1.0f;
    mTextOverlayCursorY = 0.0f;
    mTextOverlayPreviewTex = nullptr;

    mTextOverlayActive = true;
    mViewerShowUI = false;

    Keyboard::RequestKeyboard(
        "",
        "Enter text to place on image",
        [this](bool confirmed, const std::string& text) {
            if (confirmed && !text.empty()) {
                mTextOverlayText = text;
                RenderTextOverlayPreview();
            } else {
                ExitTextOverlay();
            }
        },
        SDL_WIIU_SWKBD_KEYBOARD_MODE_FULL
    );
}

void Album::ExitTextOverlay() {
    mTextOverlayActive = false;
    mViewerShowUI = true;
    if (mTextOverlayPreviewTex) { SDL_DestroyTexture(mTextOverlayPreviewTex); mTextOverlayPreviewTex = nullptr; }
    if (mTextOverlayPreviewSurf) { SDL_FreeSurface(mTextOverlayPreviewSurf); mTextOverlayPreviewSurf = nullptr; }
    if (mTextOverlayOrigSurface) { SDL_FreeSurface(mTextOverlayOrigSurface); mTextOverlayOrigSurface = nullptr; }
}

static bool LoadSysFontOnce(void** outData, uint32_t* outSize) {
    static void* sFontData = nullptr;
    static uint32_t sFontDataSize = 0;
    static bool sLoaded = false;
    if (!sLoaded) {
        sLoaded = OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &sFontData, &sFontDataSize);
    }
    *outData = sFontData;
    *outSize = sFontDataSize;
    return sLoaded;
}

void Album::RenderTextOverlayPreview() {
    if (!mTextOverlayOrigSurface || mTextOverlayText.empty()) return;

    // Work on a copy of the downscaled preview surface
    SDL_Surface* working = NULL;
    if (mTextOverlayPreviewSurf) {
        working = SDL_ConvertSurfaceFormat(mTextOverlayPreviewSurf, SDL_PIXELFORMAT_RGBA32, 0);
    }
    if (!working) {
        // Fallback - downscale on-the-fly from original
        working = SDL_ConvertSurfaceFormat(mTextOverlayOrigSurface, SDL_PIXELFORMAT_RGBA32, 0);
        if (!working) return;
        int w = working->w;
        int h = working->h;
        int maxDim = (w > h) ? w : h;
        if (maxDim > 1280) {
            float sc = 1280.0f / maxDim;
            SDL_Surface* zoomed = zoomSurface(working, sc, sc, SMOOTHING_OFF);
            SDL_FreeSurface(working);
            if (!zoomed) return;
            working = zoomed;
        }
    }

    int fontSize = (int)mTextOverlaySize;
    if (fontSize < 12) fontSize = 12;
    if (fontSize > 300) fontSize = 300;

    void* fontData = nullptr;
    uint32_t fontDataSize = 0;
    if (!LoadSysFontOnce(&fontData, &fontDataSize) || !fontData) {
        SDL_FreeSurface(working);
        return;
    }

    TTF_Font* sysFont = TTF_OpenFontRW(SDL_RWFromMem(fontData, fontDataSize), 0, fontSize);
    if (!sysFont) { SDL_FreeSurface(working); return; }

    SDL_Surface* textSurface = TTF_RenderUTF8_Blended(sysFont, mTextOverlayText.c_str(), mTextOverlayColor);
    TTF_CloseFont(sysFont);
    if (!textSurface) { SDL_FreeSurface(working); return; }

    SDL_Surface* rotatedText = rotozoomSurface(textSurface, (double)mTextOverlayAngle, 1.0, SMOOTHING_ON);
    SDL_FreeSurface(textSurface);
    if (!rotatedText) { SDL_FreeSurface(working); return; }

    float curScale = 1.0f;
    if (mTextOverlayPreviewSurf && mTextOverlayDownscale > 0.0f) {
        curScale = (float)mTextOverlayPreviewSurf->w / mTextOverlayOrigSurface->w;
    } else if (!mTextOverlayPreviewSurf) {
        curScale = (float)working->w / mTextOverlayOrigSurface->w;
    }

    int dx = (int)(mTextOverlayPosX * curScale - rotatedText->w / 2);
    int dy = (int)(mTextOverlayPosY * curScale - rotatedText->h / 2);
    SDL_Rect dst = {dx, dy, rotatedText->w, rotatedText->h};
    SDL_BlitSurface(rotatedText, nullptr, working, &dst);
    SDL_FreeSurface(rotatedText);

    // Convert to texture for display
    if (mTextOverlayPreviewTex) SDL_DestroyTexture(mTextOverlayPreviewTex);
    mTextOverlayPreviewTex = SDL_CreateTextureFromSurface(Gfx::GetRenderer(), working);
    SDL_FreeSurface(working);
}

void Album::UpdateTextOverlay(const Input& input) {
    if (Keyboard::IsActive()) return;

    if (mTextOverlayText.empty()) {
        Keyboard::RequestKeyboard(
            "",
            "Enter text to place on image",
            [this](bool confirmed, const std::string& text) {
                if (confirmed && !text.empty()) {
                    mTextOverlayText = text;
                    RenderTextOverlayPreview();
                } else {
                    ExitTextOverlay();
                }
            },
            SDL_WIIU_SWKBD_KEYBOARD_MODE_FULL
        );
        return;
    }

    if (mTextOverlayPlacing) {
        if (input.IsPressed(Input::BUTTON_B)) {
            mTextOverlayPlacing = false;
            mTextOverlayPosX = mTextOverlayOrigSurface ? mTextOverlayOrigSurface->w / 2.0f : 480.0f;
            mTextOverlayPosY = mTextOverlayOrigSurface ? mTextOverlayOrigSurface->h / 2.0f : 270.0f;
            mTextOverlayAngle = 0.0f;
            RenderTextOverlayPreview();
            return;
        }
        if (input.IsPressed(Input::BUTTON_A)) {
            mTextOverlayPlacing = false;
            RenderTextOverlayPreview();
            return;
        }

        float lx = input.GetLeftStickX();
        float ly = input.GetLeftStickY();
        if (lx != 0.0f || ly != 0.0f) {
            mTextOverlayPosX += lx * 4.0f;
            mTextOverlayPosY -= ly * 4.0f;
            RenderTextOverlayPreview();
        }

        int fine = 1;
        if (input.IsPressed(Input::BUTTON_LEFT))  { mTextOverlayPosX -= fine; RenderTextOverlayPreview(); }
        if (input.IsPressed(Input::BUTTON_RIGHT)) { mTextOverlayPosX += fine; RenderTextOverlayPreview(); }
        if (input.IsPressed(Input::BUTTON_UP))    { mTextOverlayPosY -= fine; RenderTextOverlayPreview(); }
        if (input.IsPressed(Input::BUTTON_DOWN))  { mTextOverlayPosY += fine; RenderTextOverlayPreview(); }

        if (input.IsHeld(Input::BUTTON_L) || input.IsPressed(Input::BUTTON_L)) { mTextOverlayAngle += 5.0f; RenderTextOverlayPreview(); }
        if (input.IsHeld(Input::BUTTON_R) || input.IsPressed(Input::BUTTON_R)) { mTextOverlayAngle -= 5.0f; RenderTextOverlayPreview(); }
        return;
    }

    if (mTextOverlayAdvancedColor) {
        float rx = input.GetRightStickX();
        float ry = input.GetRightStickY();

        if (rx != 0.0f || ry != 0.0f) {
            mTextOverlayCursorX += rx * 0.01f;
            mTextOverlayCursorY -= ry * 0.01f;
            if (mTextOverlayCursorX > 1.0f) mTextOverlayCursorX = 1.0f;
            if (mTextOverlayCursorX < -1.0f) mTextOverlayCursorX = -1.0f;
            if (mTextOverlayCursorY > 1.0f) mTextOverlayCursorY = 1.0f;
            if (mTextOverlayCursorY < -1.0f) mTextOverlayCursorY = -1.0f;

            float sat = sqrt(mTextOverlayCursorX * mTextOverlayCursorX + mTextOverlayCursorY * mTextOverlayCursorY);
            if (sat > 1.0f) { sat = 1.0f; }
            mTextOverlayColorSat = sat;

            if (sat < 0.01f) {
                mTextOverlayColorHue = 0.0f;
            } else {
                mTextOverlayColorHue = atan2(-mTextOverlayCursorY, mTextOverlayCursorX) * 180.0f / 3.14159f;
                if (mTextOverlayColorHue < 0.0f) mTextOverlayColorHue += 360.0f;
            }

            int region = (int)(mTextOverlayColorHue / 60.0f) % 6;
            float f = (mTextOverlayColorHue / 60.0f) - region;
            float p = mTextOverlayColorVal * (1.0f - mTextOverlayColorSat);
            float q = mTextOverlayColorVal * (1.0f - f * mTextOverlayColorSat);
            float t = mTextOverlayColorVal * (1.0f - (1.0f - f) * mTextOverlayColorSat);
            float v = mTextOverlayColorVal;
            float rr, gg, bb;
            switch (region) {
                case 0: rr = v; gg = t; bb = p; break;
                case 1: rr = q; gg = v; bb = p; break;
                case 2: rr = p; gg = v; bb = t; break;
                case 3: rr = p; gg = q; bb = v; break;
                case 4: rr = t; gg = p; bb = v; break;
                default: rr = v; gg = p; bb = q; break;
            }
            mTextOverlayColor.r = (uint8_t)(rr * 255.0f);
            mTextOverlayColor.g = (uint8_t)(gg * 255.0f);
            mTextOverlayColor.b = (uint8_t)(bb * 255.0f);
            mTextOverlayColor.a = 255;
            mTextOverlayUpdateThrottle++;
            if (mTextOverlayUpdateThrottle % 4 == 0) {
                RenderTextOverlayPreview();
            }
        }

        if (input.IsPressed(Input::BUTTON_A)) {
            mTextOverlayAdvancedColor = false;
            for (int i = 0; i < 10; i++) {
                if (mTextOverlayColor.r == TEXT_PRESET_COLORS[i].r &&
                    mTextOverlayColor.g == TEXT_PRESET_COLORS[i].g &&
                    mTextOverlayColor.b == TEXT_PRESET_COLORS[i].b) {
                    mTextOverlayColorSel = i;
                    break;
                }
            }
            RenderTextOverlayPreview();
        }
        if (input.IsPressed(Input::BUTTON_B)) {
            mTextOverlayColor = TEXT_PRESET_COLORS[mTextOverlayColorSel];
            mTextOverlayAdvancedColor = false;
            RenderTextOverlayPreview();
        }
        return;
    }

    // Right stick - adjusts text size (left/right)
    float rx = input.GetRightStickX();
    if (rx != 0.0f) {
        mTextOverlaySize += rx * 1.5f;
        if (mTextOverlaySize < 8.0f) mTextOverlaySize = 8.0f;
        if (mTextOverlaySize > 200.0f) mTextOverlaySize = 200.0f;
        RenderTextOverlayPreview();
    }

    if (input.IsPressed(Input::BUTTON_B)) {
        if (mTextOverlayPlacing) {
            mTextOverlayPlacing = false;
            mTextOverlayPosX = mTextOverlayOrigSurface ? mTextOverlayOrigSurface->w / 2.0f : 480.0f;
            mTextOverlayPosY = mTextOverlayOrigSurface ? mTextOverlayOrigSurface->h / 2.0f : 270.0f;
            mTextOverlayAngle = 0.0f;
            RenderTextOverlayPreview();
        } else {
            ExitTextOverlay();
        }
        return;
    }

    if (input.IsPressed(Input::BUTTON_UP)) {
        if (mTextOverlayFocus == 2) {
            int col = mTextOverlayColorSel % 5;
            int row = mTextOverlayColorSel / 5;
            if (row > 0) {
                row--;
                mTextOverlayColorSel = row * 5 + col;
            } else {
                mTextOverlayFocus = 1;
            }
        } else {
            mTextOverlayFocus = (mTextOverlayFocus - 1 + 4) % 4;
            if (mTextOverlayFocus == 2) mTextOverlayFocus = 1;
        }
        return;
    }
    if (input.IsPressed(Input::BUTTON_DOWN)) {
        if (mTextOverlayFocus == 2) {
            int col = mTextOverlayColorSel % 5;
            int row = mTextOverlayColorSel / 5;
            if (row < 1) {
                row++;
                mTextOverlayColorSel = row * 5 + col;
            } else {
                mTextOverlayFocus = 3;
            }
        } else {
            mTextOverlayFocus = (mTextOverlayFocus + 1) % 4;
        }
        return;
    }

    if (input.IsPressed(Input::BUTTON_LEFT)) {
        if (mTextOverlayFocus == 0) {
            mTextOverlaySize -= 2.0f;
            if (mTextOverlaySize < 8.0f) mTextOverlaySize = 8.0f;
            RenderTextOverlayPreview();
        } else if (mTextOverlayFocus == 2) {
            int col = mTextOverlayColorSel % 5;
            if (col > 0) {
                col--;
            } else {
                col = 4;
            }
            mTextOverlayColorSel = (mTextOverlayColorSel / 5) * 5 + col;
        }
        return;
    }
    if (input.IsPressed(Input::BUTTON_RIGHT)) {
        if (mTextOverlayFocus == 0) {
            mTextOverlaySize += 2.0f;
            if (mTextOverlaySize > 200.0f) mTextOverlaySize = 200.0f;
            RenderTextOverlayPreview();
        } else if (mTextOverlayFocus == 2) {
            int col = mTextOverlayColorSel % 5;
            if (col < 4) {
                col++;
            } else {
                col = 0;
            }
            mTextOverlayColorSel = (mTextOverlayColorSel / 5) * 5 + col;
        }
        return;
    }

    if (input.IsPressed(Input::BUTTON_A)) {
        if (mTextOverlayFocus == 2) {
            mTextOverlayColor = TEXT_PRESET_COLORS[mTextOverlayColorSel];
            RenderTextOverlayPreview();
        } else if (mTextOverlayFocus == 1) {
            mTextOverlayPlacing = true;
            RenderTextOverlayPreview();
        } else if (mTextOverlayFocus == 3) {
            SaveTextOverlayImage();
        }
        return;
    }

    if (input.IsPressed(Input::BUTTON_Y)) {
        mTextOverlayPlacing = true;
        RenderTextOverlayPreview();
        return;
    }

    if (input.IsPressed(Input::BUTTON_X)) {
        if (mTextOverlayFocus == 2) {
            mTextOverlayAdvancedColor = true;
            return;
        }
    }
}

void Album::DrawTextOverlay() {
    if (!mTextOverlayActive) return;

    int w = Gfx::SCREEN_WIDTH;
    int h = Gfx::SCREEN_HEIGHT;

    Gfx::DrawRectFilled(0, 0, w, h, {0x1a, 0x1a, 0x1a, 0xff});

    int leftW = (int)(w * 0.6f);
    int rightX = leftW;
    int rightW = w - rightX;

    // Image preview on the left
    if (mTextOverlayPreviewTex) {
        int texW = 0, texH = 0;
        SDL_QueryTexture(mTextOverlayPreviewTex, nullptr, nullptr, &texW, &texH);

        float scaleX = (float)leftW / texW;
        float scaleY = (float)h / texH;
        float scale = (scaleX < scaleY ? scaleX : scaleY);

        int drawW = (int)(texW * scale);
        int drawH = (int)(texH * scale);
        int drawX = (leftW - drawW) / 2;
        int drawY = (h - drawH) / 2;

        SDL_Rect src = {0, 0, texW, texH};
        SDL_Rect dst = {drawX, drawY, drawW, drawH};
        SDL_RenderCopy(Gfx::GetRenderer(), mTextOverlayPreviewTex, &src, &dst);

        if (mTextOverlayPlacing && !mTextOverlayText.empty()) {
            int fontSize = (int)mTextOverlaySize;
            if (fontSize < 12) fontSize = 12;
            int tw = Gfx::GetTextWidth(fontSize, mTextOverlayText);
            int th = Gfx::GetTextHeight(fontSize, mTextOverlayText);
            float mapScale = (float)drawW / texW;

            // Scale the text center position to screen coords
            int cx = (int)(mTextOverlayPosX * mapScale + drawX);
            int cy = (int)(mTextOverlayPosY * mapScale + drawY);
            int hw = (int)(tw * mapScale / 2 + 4);
            int hh = (int)(th * mapScale / 2 + 4);

            // Draw rotated outline around the text using corner points
            double rad = -mTextOverlayAngle * 3.14159265 / 180.0;
            double c = cos(rad);
            double s = sin(rad);
            int corners[4][2] = {
                {cx + (int)(-hw * c - (-hh) * s), cy + (int)(-hw * s + (-hh) * c)},
                {cx + (int)( hw * c - (-hh) * s), cy + (int)( hw * s + (-hh) * c)},
                {cx + (int)( hw * c -  hh  * s), cy + (int)( hw * s +  hh  * c)},
                {cx + (int)(-hw * c -  hh  * s), cy + (int)(-hw * s +  hh  * c)}
            };
            for (int i = 0; i < 4; i++) {
                int j = (i + 1) % 4;
                Gfx::DrawLine(corners[i][0], corners[i][1], corners[j][0], corners[j][1], Gfx::COLOR_ACCENT);
            }

            // Also draw rotated crosshair lines at center
            int lineLen = 20;
            int cx1 = cx + (int)(lineLen * c);
            int cy1 = cy + (int)(lineLen * s);
            int cx2 = cx - (int)(lineLen * c);
            int cy2 = cy - (int)(lineLen * s);
            int cy3 = cy - (int)(lineLen * c);
            int cx3 = cx - (int)(lineLen * s);
            int cy4 = cy + (int)(lineLen * c);
            int cx4 = cx + (int)(lineLen * s);
            Gfx::DrawLine(cx1, cy1, cx2, cy2, Gfx::COLOR_ACCENT);
            Gfx::DrawLine(cx3, cy3, cx4, cy4, Gfx::COLOR_ACCENT);
        }

        if (mTextOverlayPlacing) {
            Gfx::Print(leftW / 2, 30, 28, Gfx::COLOR_WHITE, "Move the text to desired position", Gfx::ALIGN_CENTER);
            Gfx::Print(leftW / 2, 64, 22, {0xaa, 0xaa, 0xaa, 0xff},
                       Glyphs::STICK_L + " Move   " + Glyphs::DPAD_LEFT_RIGHT + " Fine   " + Glyphs::L + "/" + Glyphs::R + " Rotate", Gfx::ALIGN_CENTER);
            Gfx::Print(leftW / 2, 88, 22, {0xaa, 0xaa, 0xaa, 0xff},
                       Glyphs::A + " Accept   " + Glyphs::B + " Revert changes", Gfx::ALIGN_CENTER);
        }
    } else {
        Gfx::Print(leftW / 2, h / 2, 28, {0x88, 0x88, 0x88, 0xff}, "No preview", Gfx::ALIGN_CENTER);
    }

    Gfx::DrawRectFilled(rightX - 2, 0, 4, h, Gfx::COLOR_ACCENT);

    // Right panel
    int px = rightX + 20;
    int py = 30;
    int pw = rightW - 40;

    Gfx::Print(rightX + rightW / 2, py, 30, Gfx::COLOR_WHITE, "Text Customization", Gfx::ALIGN_CENTER);
    if (!mTextOverlayText.empty()) {
        std::string displayStr = "\"" + mTextOverlayText + "\"";
        Gfx::Print(rightX + rightW / 2, py + 34, 20, Gfx::COLOR_WHITE, displayStr, Gfx::ALIGN_CENTER);
    }
    py += 80;

    // lambda for drawing a highlighted section row
    auto drawSection = [&](int sectionIdx, int sy, int sh, const std::string& label) {
        bool focused = (mTextOverlayFocus == sectionIdx);
        if (focused) {
            Gfx::DrawRectFilled(px - 4, sy - 4, pw + 8, sh + 8, {0x00, 0x9a, 0xc7, 0x20});
            Gfx::DrawRectOutline(px - 4, sy - 4, pw + 8, sh + 8, Gfx::COLOR_ACCENT, 2);
        }
        Gfx::Print(px, sy + 4, 24, focused ? Gfx::COLOR_ACCENT : Gfx::COLOR_TEXT_DIM, label);
    };

    // Size section
    drawSection(0, py - 4, 48, "Size");
    {
        char sbuf[32];
        snprintf(sbuf, sizeof(sbuf), "%.0f", mTextOverlaySize);
        Gfx::Print(px + 100, py + 4, 24, Gfx::COLOR_WHITE, sbuf);

        int barX = px + 50;
        int barY = py + 34;
        int barW = pw - 50;
        int barH = 8;
        Gfx::DrawRectFilled(barX, barY, barW, barH, {0x3c, 0x3c, 0x3c, 0xff});
        float sizeFrac = (mTextOverlaySize - 8.0f) / (192.0f);
        if (sizeFrac < 0.0f) sizeFrac = 0.0f;
        if (sizeFrac > 1.0f) sizeFrac = 1.0f;
        Gfx::DrawRectFilled(barX, barY, (int)(barW * sizeFrac), barH, Gfx::COLOR_ACCENT);
    }
    py += 68;

    // Position & Angle section
    drawSection(1, py - 4, 52, "Position & Angle");
    {
        int changeBtnW = 120;
        int changeBtnH = 36;
        int changeBtnX = px + pw - changeBtnW;
        SDL_Color btnColor = mTextOverlayPlacing ? SDL_Color{0x66, 0x66, 0x66, 0xff} : Gfx::COLOR_ACCENT;
        Gfx::DrawRectRounded(changeBtnX, py + 4, changeBtnW, changeBtnH, 6, btnColor);
        Gfx::Print(changeBtnX + changeBtnW / 2, py + 22, 22, Gfx::COLOR_WHITE,
                   mTextOverlayPlacing ? "Placing..." : "Change", Gfx::ALIGN_CENTER);
    }
    py += 78;

    // Colors section
    drawSection(2, py - 4, 200, "Colors");
    py += 36;

    if (mTextOverlayAdvancedColor) {
        int cwX = px + pw / 2;
        int cwY = py + 60;
        int cwRadius = 60;

        Gfx::DrawCircleFilled(cwX, cwY, cwRadius, {0x33, 0x33, 0x33, 0xff});

        for (int a = 0; a < 360; a += 10) {
            float rad = a * 3.14159f / 180.0f;
            for (int r = 0; r < cwRadius; r += 4) {
                float sat = (float)r / cwRadius;
                int region = a / 60;
                float f = (float)(a % 60) / 60.0f;
                float v = 1.0f;
                float p2 = v * (1.0f - sat);
                float q2 = v * (1.0f - f * sat);
                float t2 = v * (1.0f - (1.0f - f) * sat);
                float rr, gg, bb;
                switch (region) {
                    case 0: rr = v; gg = t2; bb = p2; break;
                    case 1: rr = q2; gg = v; bb = p2; break;
                    case 2: rr = p2; gg = v; bb = t2; break;
                    case 3: rr = p2; gg = q2; bb = v; break;
                    case 4: rr = t2; gg = p2; bb = v; break;
                    default: rr = v; gg = p2; bb = q2; break;
                }
                Gfx::DrawCircleFilled(cwX + (int)(r * cos(rad)), cwY + (int)(r * sin(rad)), 2,
                    {(uint8_t)(rr * 255), (uint8_t)(gg * 255), (uint8_t)(bb * 255), 255});
            }
        }

        int indX = cwX + (int)(cwRadius * mTextOverlayCursorX);
        int indY = cwY + (int)(cwRadius * mTextOverlayCursorY);
        Gfx::DrawCircleFilled(indX, indY, 8, Gfx::COLOR_WHITE);
        Gfx::DrawCircleFilled(indX, indY, 6, mTextOverlayColor);

        Gfx::Print(px + pw / 2, cwY + cwRadius + 24, 18, Gfx::COLOR_TEXT_DIM,
                   Glyphs::STICK_R + " Cursor  " + Glyphs::A + " Accept  " + Glyphs::B + " Reject", Gfx::ALIGN_CENTER);
    } else {
        int cw = (pw - 4 * 5) / 5;
        if (cw < 32) cw = 32;
        if (cw > 50) cw = 50;
        int ch = 32;
        int colorGap = 4;
        int totalColorW = 5 * cw + 4 * colorGap;
        int colorStartX = px + (pw - totalColorW) / 2;

        for (int i = 0; i < 10; i++) {
            int row = i / 5;
            int col = i % 5;
            int cx = colorStartX + col * (cw + colorGap);
            int cy = py + row * (ch + colorGap);

            Gfx::DrawRectRounded(cx, cy, cw, ch, 6, TEXT_PRESET_COLORS[i]);

            bool isSelected = (i == mTextOverlayColorSel);
            if (isSelected) {
                Gfx::DrawRectRoundedOutline(cx - 2, cy - 2, cw + 4, ch + 4, 6, Gfx::COLOR_ACCENT, 4);
            } else {
                Gfx::DrawRectOutline(cx, cy, cw, ch, {0x55, 0x55, 0x55, 0xff}, 1);
            }
        }

        py += 90;

        // Advanced button
        int advBtnW = 180;
        int advBtnH = 36;
        int advBtnX = px + (pw - advBtnW) / 2;
        bool advFocused = false;
        Gfx::DrawRectRounded(advBtnX, py, advBtnW, advBtnH, 8, {0x44, 0x44, 0x44, 0xff});
        if (advFocused) {
            Gfx::DrawRectRoundedOutline(advBtnX, py, advBtnW, advBtnH, 8, Gfx::COLOR_ACCENT, 3);
        } else {
            Gfx::DrawRectOutline(advBtnX, py, advBtnW, advBtnH, {0x66, 0x66, 0x66, 0xff}, 1);
        }
        Gfx::Print(advBtnX + advBtnW / 2, py + advBtnH / 2, 22, Gfx::COLOR_WHITE, "Advanced " + Glyphs::X + "", Gfx::ALIGN_CENTER);
    }

    // Done button
    int doneBtnW = 200;
    int doneBtnH = 50;
    int doneBtnX = rightX + (rightW - doneBtnW) / 2;
    int doneBtnY = h - doneBtnH - 30;
    bool doneFocused = (mTextOverlayFocus == 3);
    Gfx::DrawRectRounded(doneBtnX, doneBtnY, doneBtnW, doneBtnH, 10, Gfx::COLOR_ACCENT);
    if (doneFocused) {
        Gfx::DrawRectRoundedOutline(doneBtnX - 2, doneBtnY - 2, doneBtnW + 4, doneBtnH + 4, 10, Gfx::COLOR_WHITE, 3);
    }
    Gfx::Print(doneBtnX + doneBtnW / 2, doneBtnY + doneBtnH / 2, 28, Gfx::COLOR_WHITE, "Done", Gfx::ALIGN_CENTER);

    // Controls hint
    std::string hint;
    if (mTextOverlayFocus == 0) hint = Glyphs::DPAD_LEFT_RIGHT + " Adjust size  " + Glyphs::DPAD_UP_DOWN + " Navigate  " + Glyphs::A + " Select  " + Glyphs::B + " Back";
    else if (mTextOverlayFocus == 1) hint = Glyphs::A + " Change position  " + Glyphs::DPAD_UP_DOWN + " Navigate  " + Glyphs::B + " Back";
    else if (mTextOverlayFocus == 2) hint = Glyphs::DPAD_LEFT_RIGHT + " Pick color  " + Glyphs::DPAD_UP_DOWN + " Navigate  " + Glyphs::X + " Advanced  " + Glyphs::B + " Back";
    else if (mTextOverlayFocus == 3) hint = Glyphs::A + " Save  " + Glyphs::DPAD_UP + " Navigate  " + Glyphs::B + " Back";
    Gfx::Print(rightX + rightW / 2, h - 18, 16, Gfx::COLOR_WHITE, hint, Gfx::ALIGN_CENTER);

    if (mPointerDraw) {
        Gfx::DrawCircleFilled(mPointerScreenX, mPointerScreenY, 10, Gfx::COLOR_WHITE);
        Gfx::DrawCircleOutline(mPointerScreenX, mPointerScreenY, 12, Gfx::COLOR_BLACK, 2);
    }
}

void Album::SaveTextOverlayImage() {
    if (!mTextOverlayOrigSurface || mTextOverlayText.empty()) return;

    int fontSize = (int)mTextOverlaySize;
    if (fontSize < 12) fontSize = 12;
    if (fontSize > 300) fontSize = 300;

    void* fontData = nullptr;
    uint32_t fontDataSize = 0;
    if (!LoadSysFontOnce(&fontData, &fontDataSize) || !fontData) return;

    TTF_Font* sysFont = TTF_OpenFontRW(SDL_RWFromMem(fontData, fontDataSize), 0, fontSize);
    if (!sysFont) return;

    SDL_Surface* textSurface = TTF_RenderUTF8_Blended(sysFont, mTextOverlayText.c_str(), mTextOverlayColor);
    TTF_CloseFont(sysFont);
    if (!textSurface) return;

    // Rotate the text surface
    SDL_Surface* rotatedText = rotozoomSurface(textSurface, (double)mTextOverlayAngle, 1.0, SMOOTHING_ON);
    SDL_FreeSurface(textSurface);
    if (!rotatedText) return;

    // Copy the original image surface
    SDL_Surface* result = SDL_ConvertSurfaceFormat(mTextOverlayOrigSurface, SDL_PIXELFORMAT_RGBA32, 0);
    if (!result) { SDL_FreeSurface(rotatedText); return; }

    // Blit rotated text onto the copy
    int dx = (int)(mTextOverlayPosX - rotatedText->w / 2);
    int dy = (int)(mTextOverlayPosY - rotatedText->h / 2);
    SDL_Rect dst = {dx, dy, rotatedText->w, rotatedText->h};
    SDL_BlitSurface(rotatedText, nullptr, result, &dst);
    SDL_FreeSurface(rotatedText);

    // Save to file
    int idx = mFiltered[mViewerItem];
    std::string baseName = mAllItems[idx].filename;
    size_t dot = baseName.rfind('.');
    if (dot != std::string::npos) baseName = baseName.substr(0, dot);

    std::string outPath = mPathScreenshots + "/" + baseName + "_text.png";

    struct stat st;
    int counter = 1;
    while (stat(outPath.c_str(), &st) == 0) {
        outPath = mPathScreenshots + "/" + baseName + "_text_" + std::to_string(counter) + ".png";
        counter++;
    }

    bool ok = (IMG_SavePNG(result, outPath.c_str()) == 0);
    SDL_FreeSurface(result);

    if (ok) {
        WHBLogPrintf("[ALBUM] Text overlay saved: %s", outPath.c_str());
        ExitTextOverlay();
        mSaveNotifEndTime = SDL_GetTicks() + 2000;
        mPendingRefresh = true;
    } else {
        WHBLogPrintf("[ALBUM] Failed to save text overlay: %s", outPath.c_str());
    }
}

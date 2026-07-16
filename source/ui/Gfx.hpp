#pragma once

#include <SDL.h>
#include <SDL_image.h>
#include <string>
#include <cmath>

namespace Gfx {
    constexpr int SCREEN_WIDTH  = 1920;
    constexpr int SCREEN_HEIGHT = 1080;

    constexpr SDL_Color COLOR_BG            = {0xeb, 0xeb, 0xeb, 0xff};
    constexpr SDL_Color COLOR_SIDEBAR_BG    = {0xf5, 0xf5, 0xf5, 0xff};
    constexpr SDL_Color COLOR_HEADER_BG     = {0xff, 0xff, 0xff, 0xff};
    constexpr SDL_Color COLOR_FOOTER_BG     = {0xff, 0xff, 0xff, 0xff};
    constexpr SDL_Color COLOR_SEPARATOR     = {0xcc, 0xcc, 0xcc, 0xff};
    constexpr SDL_Color COLOR_ACCENT        = {0x00, 0x9a, 0xc7, 0xff};
    constexpr SDL_Color COLOR_ACCENT_BG     = {0xe0, 0xf4, 0xfb, 0xff};
    constexpr SDL_Color COLOR_TEXT          = {0x1a, 0x1a, 0x1a, 0xff};
    constexpr SDL_Color COLOR_TEXT_DIM      = {0x77, 0x77, 0x77, 0xff};
    constexpr SDL_Color COLOR_TEXT_LIGHT    = {0x99, 0x99, 0x99, 0xff};
    constexpr SDL_Color COLOR_WHITE         = {0xff, 0xff, 0xff, 0xff};
    constexpr SDL_Color COLOR_BLACK         = {0x00, 0x00, 0x00, 0xff};
    constexpr SDL_Color COLOR_SHADOW        = {0x00, 0x00, 0x00, 0x28};
    constexpr SDL_Color COLOR_OVERLAY       = {0x00, 0x00, 0x00, 0xb0};
    constexpr SDL_Color COLOR_SELECTED_RING = {0x00, 0x9a, 0xc7, 0xff};
    constexpr SDL_Color COLOR_VIDEO_BADGE   = {0x00, 0x00, 0x00, 0xaa};
    constexpr SDL_Color COLOR_SIDEBAR_SEL   = {0xe0, 0xf4, 0xfb, 0xff};

    // Button colours
    constexpr SDL_Color COLOR_BTN_A         = {0x00, 0x9a, 0xc7, 0xff};
    constexpr SDL_Color COLOR_BTN_B         = {0xde, 0x3b, 0x2e, 0xff};
    constexpr SDL_Color COLOR_BTN_Y         = {0xe6, 0xb8, 0x00, 0xff};
    constexpr SDL_Color COLOR_BTN_X         = {0x2e, 0x9a, 0x2e, 0xff};
    constexpr SDL_Color COLOR_DELETE        = {0xde, 0x3b, 0x2e, 0xff};

    enum AlignFlags {
        ALIGN_LEFT       = 1 << 0,
        ALIGN_RIGHT      = 1 << 1,
        ALIGN_HORIZONTAL = 1 << 2,
        ALIGN_TOP        = 1 << 3,
        ALIGN_BOTTOM     = 1 << 4,
        ALIGN_VERTICAL   = 1 << 5,
        ALIGN_CENTER     = ALIGN_HORIZONTAL | ALIGN_VERTICAL,
    };

    static constexpr inline AlignFlags operator|(AlignFlags lhs, AlignFlags rhs) {
        return static_cast<AlignFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    bool Init();
    void Shutdown();

    void Clear(SDL_Color color);
    void Render();

    void DrawRectFilled(int x, int y, int w, int h, SDL_Color color);
    void DrawRectOutline(int x, int y, int w, int h, SDL_Color color, int thickness = 2);
    void DrawRectRounded(int x, int y, int w, int h, int radius, SDL_Color color);
    void DrawRectRoundedOutline(int x, int y, int w, int h, int radius, SDL_Color color, int thickness = 3);
    void DrawRectGradient(int x, int y, int w, int h, SDL_Color topColor, SDL_Color bottomColor);
    void DrawCircleFilled(int cx, int cy, int radius, SDL_Color color);
    void DrawCircleOutline(int cx, int cy, int radius, SDL_Color color, int thickness = 2);
    void DrawLine(int x1, int y1, int x2, int y2, SDL_Color color);

    // Text
    void Print(int x, int y, int size, SDL_Color color, const std::string& text,
               AlignFlags align = ALIGN_LEFT | ALIGN_TOP);
    int  GetTextWidth(int size, const std::string& text);
    int  GetTextHeight(int size, const std::string& text);

    // Icon glyphs
    void PrintIcon(int x, int y, int size, SDL_Color color, const std::string& text,
                   AlignFlags align = ALIGN_LEFT | ALIGN_TOP);
    int  GetIconTextWidth(int size, const std::string& text);

    // Textures
    SDL_Texture* LoadTexture(const std::string& path);
    void         DestroyTexture(SDL_Texture* tex);
    void         DrawTexture(SDL_Texture* texture, int x, int y, int w, int h, uint8_t alpha = 255);
    // Draw texture with aspect-ratio-correct cropping (cover fill)
    void         DrawTextureCover(SDL_Texture* texture, int x, int y, int w, int h, uint8_t alpha = 255);

    // Render text directly onto an SDL_Surface at the given position with the given color
    // Returns the bounding rect of the rendered text on the surface (or empty rect on failure)
    SDL_Rect     RenderTextOnSurface(SDL_Surface* target, int x, int y, int size,
                                     SDL_Color color, const std::string& text,
                                     AlignFlags align = ALIGN_LEFT | ALIGN_TOP);

    SDL_Renderer* GetRenderer();
    uint32_t      GetTicks();
}

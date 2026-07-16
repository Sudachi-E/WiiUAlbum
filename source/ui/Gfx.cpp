#include "Gfx.hpp"
#include <SDL_ttf.h>
#include <coreinit/memory.h>
#include <coreinit/debug.h>
#include <map>

namespace Gfx {

    static SDL_Window*   sWindow   = nullptr;
    static SDL_Renderer* sRenderer = nullptr;
    static TTF_Font*     sFontBase = nullptr; // loaded at size 32, scaled via point size map
    static std::map<int, TTF_Font*> sFontBySize;
    static std::map<int, TTF_Font*> sIconFontBySize;
    static void*    sFontData = nullptr;
    static uint32_t sFontDataSize = 0;

    static TTF_Font* GetFontForSize(int size) {
        auto it = sFontBySize.find(size);
        if (it != sFontBySize.end()) return it->second;
        if (!sFontData) return sFontBase;
        TTF_Font* f = TTF_OpenFontRW(SDL_RWFromMem(sFontData, sFontDataSize), 0, size);
        if (f) sFontBySize[size] = f;
        return f ? f : sFontBase;
    }

    static TTF_Font* GetIconFontForSize(int size) {
        auto it = sIconFontBySize.find(size);
        if (it != sIconFontBySize.end()) return it->second;
        if (!sFontData) return nullptr;
        TTF_Font* f = TTF_OpenFontRW(SDL_RWFromMem(sFontData, sFontDataSize), 0, size);
        if (f) sIconFontBySize[size] = f;
        return f;
    }

    bool Init() {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) return false;

        if (TTF_Init() < 0) {
            SDL_Quit();
            return false;
        }

        int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
        if (!(IMG_Init(imgFlags) & imgFlags)) {
            TTF_Quit();
            SDL_Quit();
            return false;
        }

        sWindow = SDL_CreateWindow("WiiU Album",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   SCREEN_WIDTH, SCREEN_HEIGHT, 0);
        if (!sWindow) { IMG_Quit(); TTF_Quit(); SDL_Quit(); return false; }

        sRenderer = SDL_CreateRenderer(sWindow, -1,
                                       SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!sRenderer) { SDL_DestroyWindow(sWindow); IMG_Quit(); TTF_Quit(); SDL_Quit(); return false; }

        SDL_SetRenderDrawBlendMode(sRenderer, SDL_BLENDMODE_BLEND);

        // Load system font
        if (OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &sFontData, &sFontDataSize)) {
            sFontBase = TTF_OpenFontRW(SDL_RWFromMem(sFontData, sFontDataSize), 0, 32);
        }

        return true;
    }

    void Shutdown() {
        for (auto& [key, fnt] : sFontBySize) TTF_CloseFont(fnt);
        sFontBySize.clear();

        for (auto& [key, fnt] : sIconFontBySize) TTF_CloseFont(fnt);
        sIconFontBySize.clear();

        if (sFontBase) { TTF_CloseFont(sFontBase); sFontBase = nullptr; }
        if (sRenderer) { SDL_DestroyRenderer(sRenderer); sRenderer = nullptr; }
        if (sWindow)   { SDL_DestroyWindow(sWindow);   sWindow   = nullptr; }
        IMG_Quit();
        TTF_Quit();
        SDL_Quit();
    }

    void Clear(SDL_Color c) {
        SDL_SetRenderDrawColor(sRenderer, c.r, c.g, c.b, c.a);
        SDL_RenderClear(sRenderer);
    }

    void Render() { SDL_RenderPresent(sRenderer); }

    void DrawRectFilled(int x, int y, int w, int h, SDL_Color c) {
        SDL_Rect rect = {x, y, w, h};
        SDL_SetRenderDrawColor(sRenderer, c.r, c.g, c.b, c.a);
        SDL_RenderFillRect(sRenderer, &rect);
    }

    void DrawRectOutline(int x, int y, int w, int h, SDL_Color c, int thickness) {
        SDL_SetRenderDrawColor(sRenderer, c.r, c.g, c.b, c.a);
        for (int i = 0; i < thickness; i++) {
            SDL_Rect rect = {x + i, y + i, w - 2*i, h - 2*i};
            SDL_RenderDrawRect(sRenderer, &rect);
        }
    }

    void DrawRectRounded(int x, int y, int w, int h, int radius, SDL_Color c) {
        SDL_SetRenderDrawColor(sRenderer, c.r, c.g, c.b, c.a);
        SDL_Rect rects[3] = {
            {x + radius, y,              w - 2*radius, h},
            {x,          y + radius,     radius,        h - 2*radius},
            {x + w - radius, y + radius, radius,        h - 2*radius}
        };
        SDL_RenderFillRects(sRenderer, rects, 3);
        for (int dy = 0; dy < radius; dy++) {
            int dx = (int)std::sqrt((float)(radius*radius - dy*dy));
            SDL_RenderDrawLine(sRenderer, x + radius - dx, y + radius - dy, x + radius, y + radius - dy);
            SDL_RenderDrawLine(sRenderer, x + w - radius, y + radius - dy, x + w - radius + dx, y + radius - dy);
            SDL_RenderDrawLine(sRenderer, x + radius - dx, y + h - radius + dy, x + radius, y + h - radius + dy);
            SDL_RenderDrawLine(sRenderer, x + w - radius, y + h - radius + dy, x + w - radius + dx, y + h - radius + dy);
        }
    }

    void DrawRectRoundedOutline(int x, int y, int w, int h, int radius, SDL_Color c, int thickness) {
        for (int i = 0; i < thickness; i++)
            DrawRectOutline(x + i, y + i, w - 2*i, h - 2*i, c, 1);
    }

    void DrawRectGradient(int x, int y, int w, int h, SDL_Color top, SDL_Color bot) {
        for (int i = 0; i < h; i++) {
            float r = (float)i / h;
            SDL_Color c = {
                (uint8_t)(top.r + (bot.r - top.r) * r),
                (uint8_t)(top.g + (bot.g - top.g) * r),
                (uint8_t)(top.b + (bot.b - top.b) * r),
                (uint8_t)(top.a + (bot.a - top.a) * r)
            };
            SDL_SetRenderDrawColor(sRenderer, c.r, c.g, c.b, c.a);
            SDL_RenderDrawLine(sRenderer, x, y + i, x + w, y + i);
        }
    }

    void DrawCircleFilled(int cx, int cy, int radius, SDL_Color c) {
        SDL_SetRenderDrawColor(sRenderer, c.r, c.g, c.b, c.a);
        for (int y = -radius; y <= radius; y++) {
            int x = (int)std::sqrt((float)(radius*radius - y*y));
            SDL_RenderDrawLine(sRenderer, cx - x, cy + y, cx + x, cy + y);
        }
    }

    void DrawCircleOutline(int cx, int cy, int radius, SDL_Color c, int thickness) {
        SDL_SetRenderDrawColor(sRenderer, c.r, c.g, c.b, c.a);
        for (int t = 0; t < thickness; t++) {
            int r = radius - t;
            if (r < 0) break;
            for (int y = -r; y <= r; y++) {
                int x = (int)std::sqrt((float)(r*r - y*y));
                SDL_RenderDrawPoint(sRenderer, cx - x, cy + y);
                SDL_RenderDrawPoint(sRenderer, cx + x, cy + y);
            }
        }
    }

    void DrawLine(int x1, int y1, int x2, int y2, SDL_Color c) {
        SDL_SetRenderDrawColor(sRenderer, c.r, c.g, c.b, c.a);
        SDL_RenderDrawLine(sRenderer, x1, y1, x2, y2);
    }

    void Print(int x, int y, int size, SDL_Color c, const std::string& text, AlignFlags align) {
        if (text.empty()) return;
        TTF_Font* font = GetFontForSize(size);
        if (!font) return;

        SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), c);
        if (!surface) return;
        SDL_Texture* texture = SDL_CreateTextureFromSurface(sRenderer, surface);
        int tw = surface->w, th = surface->h;
        SDL_FreeSurface(surface);
        if (!texture) return;

        if (align & ALIGN_HORIZONTAL) x -= tw / 2;
        else if (align & ALIGN_RIGHT) x -= tw;
        if (align & ALIGN_VERTICAL)   y -= th / 2;
        else if (align & ALIGN_BOTTOM) y -= th;

        SDL_Rect dst = {x, y, tw, th};
        SDL_RenderCopy(sRenderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }

    int GetTextWidth(int size, const std::string& text) {
        TTF_Font* font = GetFontForSize(size);
        if (!font || text.empty()) return 0;
        int w = 0;
        TTF_SizeUTF8(font, text.c_str(), &w, nullptr);
        return w;
    }

    int GetTextHeight(int size, const std::string& text) {
        TTF_Font* font = GetFontForSize(size);
        if (!font || text.empty()) return 0;
        int h = 0;
        TTF_SizeUTF8(font, text.c_str(), nullptr, &h);
        return h;
    }

    void PrintIcon(int x, int y, int size, SDL_Color c, const std::string& text, AlignFlags align) {
        TTF_Font* font = GetIconFontForSize(size);
        if (!font || text.empty()) return;

        SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), c);
        if (!surface) return;
        SDL_Texture* texture = SDL_CreateTextureFromSurface(sRenderer, surface);
        int tw = surface->w, th = surface->h;
        SDL_FreeSurface(surface);
        if (!texture) return;

        if (align & ALIGN_HORIZONTAL) x -= tw / 2;
        else if (align & ALIGN_RIGHT) x -= tw;
        if (align & ALIGN_VERTICAL)   y -= th / 2;
        else if (align & ALIGN_BOTTOM) y -= th;

        SDL_Rect dst = {x, y, tw, th};
        SDL_RenderCopy(sRenderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }

    int GetIconTextWidth(int size, const std::string& text) {
        TTF_Font* font = GetIconFontForSize(size);
        if (!font || text.empty()) return 0;
        int w = 0;
        TTF_SizeUTF8(font, text.c_str(), &w, nullptr);
        return w;
    }

    SDL_Texture* LoadTexture(const std::string& path) {
        SDL_Surface* surface = IMG_Load(path.c_str());
        if (!surface) return nullptr;
        SDL_Texture* texture = SDL_CreateTextureFromSurface(sRenderer, surface);
        SDL_FreeSurface(surface);
        return texture;
    }

    void DestroyTexture(SDL_Texture* tex) {
        if (tex) SDL_DestroyTexture(tex);
    }

    void DrawTexture(SDL_Texture* texture, int x, int y, int w, int h, uint8_t alpha) {
        if (!texture) return;
        SDL_SetTextureAlphaMod(texture, alpha);
        SDL_Rect dst = {x, y, w, h};
        SDL_RenderCopy(sRenderer, texture, nullptr, &dst);
    }

    void DrawTextureCover(SDL_Texture* texture, int x, int y, int w, int h, uint8_t alpha) {
        if (!texture) return;
        SDL_SetTextureAlphaMod(texture, alpha);

        int texW = 0, texH = 0;
        SDL_QueryTexture(texture, nullptr, nullptr, &texW, &texH);
        if (texW == 0 || texH == 0) return;

        float scaleX = (float)w / texW;
        float scaleY = (float)h / texH;
        float scale  = (scaleX > scaleY) ? scaleX : scaleY;

        int srcW = (int)(w / scale);
        int srcH = (int)(h / scale);
        if (srcW > texW) srcW = texW;
        if (srcH > texH) srcH = texH;

        int srcX = (texW - srcW) / 2;
        int srcY = (texH - srcH) / 2;

        SDL_Rect srcRect = {srcX, srcY, srcW, srcH};
        SDL_Rect dstRect = {x,    y,    w,    h};
        SDL_RenderCopy(sRenderer, texture, &srcRect, &dstRect);
    }

    SDL_Renderer* GetRenderer() { return sRenderer; }
    uint32_t      GetTicks()    { return SDL_GetTicks(); }

    SDL_Rect RenderTextOnSurface(SDL_Surface* target, int x, int y, int size,
                                 SDL_Color color, const std::string& text,
                                 AlignFlags align) {
        SDL_Rect result = {0, 0, 0, 0};
        if (!target || text.empty()) return result;

        TTF_Font* font = GetFontForSize(size);
        if (!font) return result;

        SDL_Surface* textSurface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!textSurface) return result;

        int tw = textSurface->w;
        int th = textSurface->h;

        int dx = x, dy = y;
        if (align & ALIGN_HORIZONTAL) dx -= tw / 2;
        else if (align & ALIGN_RIGHT) dx -= tw;
        if (align & ALIGN_VERTICAL)   dy -= th / 2;
        else if (align & ALIGN_BOTTOM) dy -= th;

        SDL_Rect dst = {dx, dy, tw, th};
        SDL_BlitSurface(textSurface, nullptr, target, &dst);
        SDL_FreeSurface(textSurface);

        result.x = dx; result.y = dy; result.w = tw; result.h = th;
        return result;
    }

}

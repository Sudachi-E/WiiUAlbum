#pragma once
#include <string>
#include <vector>
#include <SDL.h>

class QRCode {
public:
    bool Encode(const std::string& data);
    int  GetSize()  const { return mSize; }
    bool GetModule(int x, int y) const;
    SDL_Texture* Render(SDL_Renderer* renderer, int modulePixels) const;

private:
    int  mVersion = 0;
    int  mSize    = 0;
    int  mMask    = 0;
    std::vector<unsigned char> mFrame;

    void InitModules();
    void SetModule(int x, int y, bool val);
    void SetFixed(int x, int y);
    bool IsFixed(int x, int y) const;

    void CreateFrame();
    void PutFinder(int ox, int oy);
    void PutAlignmentMarker(int ox, int oy);
    void PutAlignment();

    struct FrameFiller {
        int width;
        unsigned char *frame;
        int x, y, dir, bit;
        void Init(int w, unsigned char *f);
        unsigned char *Next();
    };

    static int  PickVersion(const std::string& data);
    static int  CalcPenalty(int width, unsigned char *frame);
    static int  CalcRunPenalty(int width, unsigned char *frame, bool horizontal);
};

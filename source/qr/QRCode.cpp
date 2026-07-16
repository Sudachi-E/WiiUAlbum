#include "QRCode.hpp"
#include "QREncode.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>

void QRCode::InitModules() {
    mSize = QRGetWidth(mVersion);
    mFrame.assign(mSize * mSize, 0);
}

bool QRCode::GetModule(int x, int y) const {
    if (x < 0 || x >= mSize || y < 0 || y >= mSize) return false;
    return mFrame[y * mSize + x] & 1;
}

void QRCode::SetModule(int x, int y, bool val) {
    if (x < 0 || x >= mSize || y < 0 || y >= mSize) return;
    mFrame[y * mSize + x] = (mFrame[y * mSize + x] & 0xfe) | (val ? 1 : 0);
}

void QRCode::SetFixed(int x, int y) {
    if (x < 0 || x >= mSize || y < 0 || y >= mSize) return;
    mFrame[y * mSize + x] |= 0x80;
}

bool QRCode::IsFixed(int x, int y) const {
    if (x < 0 || x >= mSize || y < 0 || y >= mSize) return true;
    return (mFrame[y * mSize + x] & 0x80) != 0;
}

// Pattern placement

void QRCode::PutFinder(int ox, int oy) {
    static const unsigned char finder[] = {
        0xc1, 0xc1, 0xc1, 0xc1, 0xc1, 0xc1, 0xc1,
        0xc1, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc1,
        0xc1, 0xc0, 0xc1, 0xc1, 0xc1, 0xc0, 0xc1,
        0xc1, 0xc0, 0xc1, 0xc1, 0xc1, 0xc0, 0xc1,
        0xc1, 0xc0, 0xc1, 0xc1, 0xc1, 0xc0, 0xc1,
        0xc1, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc1,
        0xc1, 0xc1, 0xc1, 0xc1, 0xc1, 0xc1, 0xc1,
    };
    unsigned char *p = &mFrame[oy * mSize + ox];
    const unsigned char *s = finder;
    for (int y = 0; y < 7; y++) {
        memcpy(p, s, 7);
        p += mSize;
        s += 7;
    }
}

void QRCode::PutAlignmentMarker(int ox, int oy) {
    static const unsigned char marker[] = {
        0xa1, 0xa1, 0xa1, 0xa1, 0xa1,
        0xa1, 0xa0, 0xa0, 0xa0, 0xa1,
        0xa1, 0xa0, 0xa1, 0xa0, 0xa1,
        0xa1, 0xa0, 0xa0, 0xa0, 0xa1,
        0xa1, 0xa1, 0xa1, 0xa1, 0xa1,
    };
    unsigned char *p = &mFrame[(oy - 2) * mSize + ox - 2];
    const unsigned char *s = marker;
    for (int y = 0; y < 5; y++) {
        memcpy(p, s, 5);
        p += mSize;
        s += 5;
    }
}

void QRCode::PutAlignment() {
    if (mVersion < 2) return;

    const int *ap = QRGetAlignPattern(mVersion);
    int d = ap[1] - ap[0];
    int w = (d < 0) ? 2 : (mSize - ap[0]) / d + 2;

    if (w * w - 3 == 1) {
        PutAlignmentMarker(ap[0], ap[0]);
        return;
    }

    int cx = ap[0];
    for (int x = 1; x < w - 1; x++) {
        PutAlignmentMarker(6, cx);
        PutAlignmentMarker(cx, 6);
        cx += d;
    }

    int cy = ap[0];
    for (int y = 0; y < w - 1; y++) {
        cx = ap[0];
        for (int x = 0; x < w - 1; x++) {
            PutAlignmentMarker(cx, cy);
            cx += d;
        }
        cy += d;
    }
}

// Frame construction

void QRCode::CreateFrame() {
    InitModules();

    PutFinder(0, 0);
    PutFinder(mSize - 7, 0);
    PutFinder(0, mSize - 7);

    unsigned char *p = &mFrame[0];
    unsigned char *q = &mFrame[(mSize - 7) * mSize];
    for (int y = 0; y < 7; y++) {
        p[7] = 0xc0;
        p[mSize - 8] = 0xc0;
        q[7] = 0xc0;
        p += mSize;
        q += mSize;
    }
    memset(&mFrame[7 * mSize], 0xc0, 8);
    memset(&mFrame[8 * mSize - 8], 0xc0, 8);
    memset(&mFrame[(mSize - 8) * mSize], 0xc0, 8);

    memset(&mFrame[8 * mSize], 0x84, 9);
    memset(&mFrame[9 * mSize - 8], 0x84, 8);
    p = &mFrame[8];
    for (int y = 0; y < 8; y++) {
        *p = 0x84;
        p += mSize;
    }
    p = &mFrame[(mSize - 7) * mSize + 8];
    for (int y = 0; y < 7; y++) {
        *p = 0x84;
        p += mSize;
    }

    p = &mFrame[6 * mSize + 8];
    unsigned char *r = &mFrame[8 * mSize + 6];
    for (int x = 1; x < mSize - 15; x++) {
        *p = (unsigned char)(0x90 | (x & 1));
        *r = (unsigned char)(0x90 | (x & 1));
        p++;
        r += mSize;
    }

    PutAlignment();

    mFrame[(mSize - 8) * mSize + 8] = 0x81;
}

// FrameFiller

void QRCode::FrameFiller::Init(int w, unsigned char *f) {
    width = w;
    frame = f;
    x = w - 1;
    y = w - 1;
    dir = -1;
    bit = -1;
}

unsigned char *QRCode::FrameFiller::Next() {
    if (bit == -1) {
        bit = 0;
        return &frame[y * width + x];
    }

    int cx = x;
    int cy = y;

    if (bit == 0) {
        cx--;
        bit++;
    } else {
        cx++;
        cy += dir;
        bit--;
    }

    if (dir < 0) {
        if (cy < 0) {
            cy = 0;
            cx -= 2;
            dir = 1;
            if (cx == 6) { cx--; cy = 9; }
        }
    } else if (cy == width) {
        cy = width - 1;
        cx -= 2;
        dir = -1;
        if (cx == 6) { cx--; cy -= 8; }
    }
    if (cx < 0 || cy < 0) return nullptr;

    x = cx;
    y = cy;

    if (frame[cy * width + cx] & 0x80)
        return Next();
    return &frame[cy * width + cx];
}

// Version selection

int QRCode::PickVersion(const std::string& data) {
    int len = (int)data.size();
    for (int v = 2; v <= 6; v++) {
        int dataWords = QRGetDataWords(v);
        int eccBytes  = QRGetECCBytes(v, 1);
        int dataBytes = dataWords - eccBytes;
        if (len + 2 <= dataBytes) return v;
    }
    return 0;
}

// Encoding pipeline

bool QRCode::Encode(const std::string& data) {
    mVersion = PickVersion(data);
    if (mVersion < 2) {
        return false;
    }

    int level    = 1;
    int version  = mVersion;
    int width    = QRGetWidth(version);
    int dataWords = QRGetDataWords(version);
    int eccTotal  = QRGetECCBytes(version, level);
    int dataBytes = dataWords - eccTotal;
    int remainder = QRGetRemainder(version);

    // Build bitstream
    std::vector<unsigned char> bitStream;
    auto addBits = [&](unsigned int val, int nbits) {
        for (int i = nbits - 1; i >= 0; i--)
            bitStream.push_back((unsigned char)((val >> i) & 1));
    };

    addBits(4, 4);
    addBits((unsigned int)data.size(), 8);
    for (char c : data)
        addBits((unsigned char)c, 8);

    int totalBits = dataBytes * 8;
    int termBits = std::min(4, totalBits - (int)bitStream.size());
    for (int i = 0; i < termBits; i++) bitStream.push_back(0);

    while ((int)bitStream.size() % 8 != 0) bitStream.push_back(0);

    static const unsigned char padBytes[] = {0xEC, 0x11};
    int pi = 0;
    while ((int)bitStream.size() < totalBits) {
        addBits(padBytes[pi & 1], 8);
        pi++;
    }

    // Convert to bytes
    std::vector<unsigned char> dataBytesVec;
    for (int i = 0; i < dataBytes; i++) {
        unsigned char byte = 0;
        for (int j = 0; j < 8; j++)
            if (bitStream[i * 8 + j]) byte |= (1 << (7 - j));
        dataBytesVec.push_back(byte);
    }

    // ECC block specs
    int spec[5];
    QRGetEccSpec(version, level, spec);
    int b1 = spec[0], dc1 = spec[1], ecc1 = spec[2];
    int b2 = spec[3], dc2 = spec[4];
    int totalBlocks = b1 + b2;

    // RS encoding
    struct RSBlock { int dl, el; unsigned char *data; unsigned char ecc[30]; };
    std::vector<RSBlock> blocks(totalBlocks);
    unsigned char *dp = dataBytesVec.data();
    for (int i = 0; i < b1; i++) {
        blocks[i].dl = dc1; blocks[i].el = ecc1;
        blocks[i].data = dp; dp += dc1;
        QRRSEncode((size_t)dc1, (size_t)ecc1, blocks[i].data, blocks[i].ecc);
    }
    for (int i = 0; i < b2; i++) {
        blocks[b1 + i].dl = dc2; blocks[b1 + i].el = ecc1;
        blocks[b1 + i].data = dp; dp += dc2;
        QRRSEncode((size_t)dc2, (size_t)ecc1, blocks[b1 + i].data, blocks[b1 + i].ecc);
    }

    // Interleave
    std::vector<unsigned char> interleaved;
    int maxDL = (b2 > 0) ? dc2 : dc1;
    for (int i = 0; i < maxDL; i++)
        for (int b = 0; b < totalBlocks; b++)
            if (i < blocks[b].dl)
                interleaved.push_back(blocks[b].data[i]);
    for (int i = 0; i < ecc1; i++)
        for (int b = 0; b < totalBlocks; b++)
            interleaved.push_back(blocks[b].ecc[i]);

    // Create frame and place data
    CreateFrame();

    FrameFiller ff;
    ff.Init(width, mFrame.data());

    for (size_t i = 0; i < interleaved.size(); i++) {
        unsigned char code = interleaved[i];
        unsigned char bit = 0x80;
        for (int j = 0; j < 8; j++) {
            unsigned char *p = ff.Next();
            if (!p) {
                return false;
            }
            *p = (bit & code) ? 1 : 0;
            bit >>= 1;
        }
    }

    for (int i = 0; i < remainder; i++) {
        unsigned char *p = ff.Next();
        if (!p) break;
        *p = 0;
    }

    // Mask evaluation
    unsigned char *tmpFrame = (unsigned char *)malloc((size_t)(width * width));
    unsigned char *bestMasked = nullptr;
    if (!tmpFrame) {
        return false;
    }

    int bestMask = 0;
    int bestScore = 999999;

    for (int mask = 0; mask < 8; mask++) {
        memcpy(tmpFrame, mFrame.data(), (size_t)(width * width));

        for (int y = 0; y < width; y++) {
            for (int x = 0; x < width; x++) {
                unsigned char val = tmpFrame[y * width + x];
                if (val & 0x80) continue;

                bool invert = false;
                switch (mask) {
                    case 0: invert = ((x + y) & 1) == 0; break;
                    case 1: invert = (y & 1) == 0; break;
                    case 2: invert = (x % 3) == 0; break;
                    case 3: invert = ((x + y) % 3) == 0; break;
                    case 4: invert = (((y / 2) + (x / 3)) & 1) == 0; break;
                    case 5: invert = (((x * y) & 1) + ((x * y) % 3)) == 0; break;
                    case 6: invert = ((((x * y) & 1) + ((x * y) % 3)) & 1) == 0; break;
                    case 7: invert = ((((x * y) % 3) + ((x + y) & 1)) & 1) == 0; break;
                }
                if (invert) val ^= 1;
                tmpFrame[y * width + x] = val;
            }
        }

        unsigned int fmt = QRFormatInfo[level][mask];
        for (int i = 0; i < 8; i++) {
            unsigned char v = (fmt & 1) ? (unsigned char)0x85 : (unsigned char)0x84;
            tmpFrame[8 * width + width - 1 - i] = v;
            if (i < 6)
                tmpFrame[i * width + 8] = v;
            else
                tmpFrame[(i + 1) * width + 8] = v;
            fmt >>= 1;
        }
        for (int i = 0; i < 7; i++) {
            unsigned char v = (fmt & 1) ? (unsigned char)0x85 : (unsigned char)0x84;
            tmpFrame[(width - 7 + i) * width + 8] = v;
            if (i == 0)
                tmpFrame[8 * width + 7] = v;
            else
                tmpFrame[8 * width + 6 - i] = v;
            fmt >>= 1;
        }

        int demerit = CalcPenalty(width, tmpFrame);

        if (demerit < bestScore) {
            bestScore = demerit;
            bestMask = mask;
            free(bestMasked);
            bestMasked = (unsigned char *)malloc((size_t)(width * width));
            if (bestMasked) memcpy(bestMasked, tmpFrame, (size_t)(width * width));
        }
    }

    free(tmpFrame);

    if (bestMasked) {
        memcpy(mFrame.data(), bestMasked, (size_t)(width * width));
        free(bestMasked);
    }

    mMask = bestMask;
    return true;
}

// Penalty calculation

int QRCode::CalcRunPenalty(int width, unsigned char *frame, bool horizontal) {
    int demerit = 0;
    int dim = width;

    for (int i = 0; i < dim; i++) {
        int runLen[256];
        int head = 0;

        int idx0 = horizontal ? (i * width) : i;
        if (frame[idx0] & 1) { runLen[0] = -1; head = 1; }
        else { runLen[0] = 1; head = 0; }
        unsigned char prev = frame[idx0];

        for (int j = 1; j < dim; j++) {
            int idx = horizontal ? (i * width + j) : (j * width + i);
            if ((frame[idx] ^ prev) & 1) {
                head++;
                runLen[head] = 1;
                prev = frame[idx];
            } else {
                runLen[head]++;
            }
        }

        int len = head + 1;
        for (int j = 0; j < len; j++) {
            if (runLen[j] >= 5)
                demerit += 3 + (runLen[j] - 5);
        }
        for (int j = 0; j < len; j++) {
            if ((j & 1) && j >= 3 && j < len - 2 && (runLen[j] % 3) == 0) {
                int fact = runLen[j] / 3;
                if (runLen[j - 2] == fact && runLen[j - 1] == fact &&
                    runLen[j + 1] == fact && runLen[j + 2] == fact) {
                    if (j == 3 || runLen[j - 3] >= 4 * fact)
                        demerit += 40;
                    else if (j + 4 >= len || runLen[j + 3] >= 4 * fact)
                        demerit += 40;
                }
            }
        }
    }
    return demerit;
}

int QRCode::CalcPenalty(int width, unsigned char *frame) {
    int demerit = 0;

    for (int y = 1; y < width; y++) {
        for (int x = 1; x < width; x++) {
            unsigned char b = frame[y * width + x];
            if (((b & frame[y * width + x - 1] &
                  frame[(y - 1) * width + x] &
                  frame[(y - 1) * width + x - 1]) & 1) ||
                ((~(b | frame[y * width + x - 1] |
                    frame[(y - 1) * width + x] |
                    frame[(y - 1) * width + x - 1])) & 1)) {
                demerit += 3;
            }
        }
    }

    demerit += CalcRunPenalty(width, frame, true);
    demerit += CalcRunPenalty(width, frame, false);

    return demerit;
}

// Rendering

SDL_Texture *QRCode::Render(SDL_Renderer *renderer, int modulePixels) const {
    int pad = modulePixels * 4;
    int px = mSize * modulePixels + pad * 2;

    SDL_Surface *surf = SDL_CreateRGBSurface(0, px, px, 32,
                                             0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
    if (!surf) return nullptr;

    SDL_FillRect(surf, nullptr, SDL_MapRGBA(surf->format, 255, 255, 255, 255));
    Uint32 black = SDL_MapRGBA(surf->format, 0, 0, 0, 255);

    for (int y = 0; y < mSize; y++) {
        for (int x = 0; x < mSize; x++) {
            if (mFrame[y * mSize + x] & 1) {
                SDL_Rect r = {pad + x * modulePixels, pad + y * modulePixels,
                              modulePixels, modulePixels};
                SDL_FillRect(surf, &r, black);
            }
        }
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    return tex;
}

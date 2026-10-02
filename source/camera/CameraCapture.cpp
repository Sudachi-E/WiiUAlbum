#include "CameraCapture.hpp"
#include "../ui/Log.hpp"

#include <camera/camera.h>
#include <coreinit/cache.h>
#include <coreinit/thread.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>

namespace Camera {

    static std::mutex             sSurfaceLock;
    static std::mutex             sFrameLock;

    static CAMHandle              sHandle      = -1;
    static Status                 sStatus      = Status::Closed;
    static char                   sMessage[128] = {0};
    static bool                   sOpen        = false;

    static void*                  sWorkMem     = nullptr;
    static size_t                 sWorkMemSize = 0;

    static constexpr int          NUM_SURFACES = 4;
    static CAMSurface             sSurfaces[NUM_SURFACES];
    static void*                  sSurfaceBufs[NUM_SURFACES] = {nullptr};

    static uint8_t*               sFrameBuf    = nullptr;
    static uint8_t*               sSnapshot    = nullptr;
    static size_t                 sFrameBufSize = 0;
    static bool                   sFrameReady  = false;
    static bool                   sHaveFrame   = false;

    static SDL_Surface*           sScratch     = nullptr;

    static int                    sWindowFrames = 0;
    static Uint32                 sWindowStart  = 0;
    static float                  sMeasuredFps  = 0.f;

    static int                    sLuma[256];

#define CAM_DIAG                  1
#define CAM_DIAG_WARMUP_FRAMES    24
#define CAM_DIAG_VERIFY_HANDOFF   1
#define CAM_DIAG_VERIFY_CACHE     1
#define CAM_DIAG_VERIFY_SETTLE    1
#define CAM_DIAG_DUMP_NV12        0

    static std::mutex             sDiagLock;
    static Diagnostics            sDiag;
    static std::string            sDumpPath;

    static uint32_t               sProducerHash = 0;
    static uint32_t               sConsumerHash = 0;
    static uint64_t               sProducedSeq  = 0;
    static uint64_t               sConsumedSeq  = 0;
    static uint64_t               sHashedSeq    = 0;

    static uint8_t*               sDiagScratch    = nullptr;
    static size_t                 sDiagScratchSize = 0;

    static std::atomic<int>       sFramesSinceOpen{0};
    static std::atomic<bool>      sWarmupDone{false};

    static void SetStatus(Status status, const char* fmt, ...) {
        sStatus = status;
        if (fmt) {
            va_list args;
            va_start(args, fmt);
            vsnprintf(sMessage, sizeof(sMessage), fmt, args);
            va_end(args);
        } else {
            sMessage[0] = '\0';
        }
        if (sMessage[0])
            ALBUM_LOG("[CAM] status=%d msg=\"%s\"", (int)sStatus, sMessage);
    }

    static void* AlignedAlloc(size_t size) {
        size_t aligned = (size + (size_t)(CAMERA_YUV_BUFFER_ALIGNMENT - 1)) &
                         ~(size_t)(CAMERA_YUV_BUFFER_ALIGNMENT - 1);
        return aligned_alloc(CAMERA_YUV_BUFFER_ALIGNMENT, aligned);
    }

    static void FreeSurfaceBufs() {
        for (int i = 0; i < NUM_SURFACES; i++) {
            if (sSurfaceBufs[i]) { free(sSurfaceBufs[i]); sSurfaceBufs[i] = nullptr; }
            memset(&sSurfaces[i], 0, sizeof(sSurfaces[i]));
        }
    }

    static void ResetCounters() {
        sWindowFrames = 0;
        sWindowStart  = SDL_GetTicks();
        sMeasuredFps  = 0.f;
    }

    static inline uint32_t WordHash(const void* data, size_t bytes) {
        const uint8_t* p = (const uint8_t*)data;
        uint32_t h = 2166136261u;
        size_t i = 0;
        for (; i + 4 <= bytes; i += 4) {
            uint32_t w;
            memcpy(&w, p + i, 4);
            h = (h ^ w) * 16777619u;
        }
        for (; i < bytes; i++)
            h = (h ^ p[i]) * 16777619u;
        return h;
    }

    static bool InWarmup() {
        if (sWarmupDone.load()) return false;
        if (sFramesSinceOpen.fetch_add(1) >= CAM_DIAG_WARMUP_FRAMES) {
            sWarmupDone.store(true);
            return false;
        }
        return true;
    }

    static void RecordProducerHash() {
#if CAM_DIAG_VERIFY_HANDOFF
        sProducerHash = WordHash(sFrameBuf, sFrameBufSize);
        sHashedSeq    = sProducedSeq;
#endif
    }

    static void VerifyConsumerHash() {
#if CAM_DIAG_VERIFY_HANDOFF
        uint32_t h = WordHash(sFrameBuf, sFrameBufSize);
        sConsumerHash = h;
        bool same = (h == sProducerHash) && (sConsumedSeq == sHashedSeq);
        std::lock_guard<std::mutex> lock(sDiagLock);
        sDiag.handoffChecks++;
        if (!same) {
            sDiag.handoffMismatch++;
            ALBUM_LOG_QUIET("[CAM][diag] HANDOFF MISMATCH prod=%08x cons=%08x seq=%llu hashed=%llu",
                         (unsigned)sProducerHash, (unsigned)h,
                         (unsigned long long)sConsumedSeq,
                         (unsigned long long)sHashedSeq);
        }
#endif
    }

    static void CheckCacheCoherency(void* surface) {
#if CAM_DIAG_VERIFY_CACHE
        if (!sDiagScratch || sDiagScratchSize < sFrameBufSize) return;

        memcpy(sDiagScratch, surface, sFrameBufSize);
        uint32_t before = WordHash(sDiagScratch, sFrameBufSize);

        DCInvalidateRange(surface, (uint32_t)sFrameBufSize);
        uint32_t after = WordHash(surface, sFrameBufSize);

        std::lock_guard<std::mutex> lock(sDiagLock);
        sDiag.cacheChecks++;
        if (before == after) return;

        sDiag.cacheStale++;
        size_t stale = 0, total = 0;
        for (size_t off = 0; off + 32 <= sFrameBufSize; off += 32) {
            total++;
            if (memcmp(sDiagScratch + off, (uint8_t*)surface + off, 32) != 0)
                stale++;
        }
        if (total)
            sDiag.cacheStaleLinesPct += (100.f * (float)stale) / (float)total;

        ALBUM_LOG_QUIET("[CAM][diag] STALE CACHE before=%08x after=%08x stale=%zu/%zu lines",
                     (unsigned)before, (unsigned)after, stale, total);
#endif
    }

    static void CheckDmaSettled(const void* surface) {
#if CAM_DIAG_VERIFY_SETTLE
        uint32_t first  = WordHash(surface, sFrameBufSize);
        SDL_Delay(3);
        uint32_t second = WordHash(surface, sFrameBufSize);

        std::lock_guard<std::mutex> lock(sDiagLock);
        sDiag.settleChecks++;
        if (first != second) {
            sDiag.settleChanged++;
            ALBUM_LOG_QUIET("[CAM][diag] DMA STILL MOVING %08x -> %08x",
                         (unsigned)first, (unsigned)second);
        }
#endif
    }

    static void ComputePlaneStats(const uint8_t* nv12) {
        const uint8_t* yPlane  = nv12;
        const uint8_t* uvPitch = nv12 + (size_t)CAMERA_PITCH * HEIGHT;
        const uint8_t* uvWidth = uvPitch;
        const uint8_t* uvPlain = nv12 + (size_t)WIDTH * HEIGHT;

        double sum = 0, sumSq = 0, count = 0;
        for (int row = 0; row < HEIGHT; row++) {
            const uint8_t* y = yPlane + (size_t)row * CAMERA_PITCH;
            for (int col = 0; col < WIDTH; col++) {
                double v = y[col];
                sum  += v;
                sumSq += v * v;
                count += 1.0;
            }
        }
        float yMean = (float)(sum / count);
        float yVar  = (float)(sumSq / count) - yMean * yMean;
        if (yVar < 0.f) yVar = 0.f;

        struct Hypothesis {
            const char* label;
            const uint8_t* base;
            int stride;
            float uMean, vMean, vSpread;
        };
        Hypothesis hs[3] = {
            {"stride=768 base=pad", uvPitch, CAMERA_PITCH, 0, 0, 0},
            {"stride=640 base=pad", uvWidth, WIDTH,         0, 0, 0},
            {"stride=640 base=raw", uvPlain, WIDTH,         0, 0, 0},
        };

        for (int h = 0; h < 3; h++) {
            double su = 0, sv = 0, n = 0;
            for (int row = 0; row < HEIGHT / 2; row++) {
                const uint8_t* p = hs[h].base + (size_t)row * hs[h].stride;
                for (int col = 0; col < WIDTH; col += 2) {
                    su += p[col];
                    sv += p[col + 1];
                    n  += 1.0;
                }
            }
            hs[h].uMean = (float)(su / n);
            hs[h].vMean = (float)(sv / n);

            double dev = 0;
            for (int row = 0; row < HEIGHT / 2; row++) {
                const uint8_t* p = hs[h].base + (size_t)row * hs[h].stride;
                for (int col = 0; col < WIDTH; col += 2) {
                    double d = (double)p[col + 1] - (double)hs[h].vMean;
                    dev += d * d;
                }
            }
            hs[h].vSpread = (float)std::sqrt(dev / n);
        }

        {
            std::lock_guard<std::mutex> lock(sDiagLock);
            sDiag.yMean  = yMean;
            sDiag.yStd   = std::sqrt(yVar);
            sDiag.uMeanPitchStride = hs[0].uMean;
            sDiag.vMeanPitchStride = hs[0].vMean;
            sDiag.vSpreadPitchStride = hs[0].vSpread;
            sDiag.uMeanWidthStride = hs[1].uMean;
            sDiag.vMeanWidthStride = hs[1].vMean;
            sDiag.vSpreadWidthStride = hs[1].vSpread;
            sDiag.uMeanUnpadded = hs[2].uMean;
            sDiag.vMeanUnpadded = hs[2].vMean;
            sDiag.vSpreadUnpadded = hs[2].vSpread;
            sDiag.valid = true;
        }

        ALBUM_LOG_QUIET("[CAM][diag] luma  y=%.1f std=%.1f", yMean, std::sqrt(yVar));
        for (int h = 0; h < 3; h++)
            ALBUM_LOG_QUIET("[CAM][diag] chroma %-18s u=%6.1f v=%6.1f vspread=%6.1f",
                         hs[h].label, hs[h].uMean, hs[h].vMean, hs[h].vSpread);
    }

#if CAM_DIAG_DUMP_NV12
    static void DumpNv12Frame(const uint8_t* nv12) {
        if (sDumpPath.empty()) return;
        FILE* f = fopen(sDumpPath.c_str(), "wb");
        if (!f) {
            ALBUM_LOG_QUIET("[CAM][diag] could not open dump path '%s'", sDumpPath.c_str());
            return;
        }
        fwrite(nv12, 1, sFrameBufSize, f);
        fclose(f);
        ALBUM_LOG_QUIET("[CAM][diag] wrote raw NV12 frame to %s (%d bytes)",
                     sDumpPath.c_str(), (int)sFrameBufSize);
    }
#endif

    static float sRowMean[HEIGHT];
    static float sOutR, sOutG, sOutB;
    static bool  sOutValid = false;

    static void RunTearProbe(const uint8_t* rgba, int pitch) {
        for (int row = 0; row < HEIGHT; row++) {
            const uint8_t* p = rgba + (size_t)row * pitch;
            int acc = 0, n = 0;
            long rSum = 0, gSum = 0, bSum = 0;
            for (int col = 0; col < WIDTH; col += 8) {
                int r = p[col * 4 + 0], g = p[col * 4 + 1], b = p[col * 4 + 2];
                acc += r + g + b;
                rSum += r;
                gSum += g;
                bSum += b;
                n += 3;
            }
            sRowMean[row] = n ? (float)acc / (float)n : 0.f;

            float samples = (float)(n / 3);
            sOutR = samples > 0.f ? (float)rSum / samples : 0.f;
            sOutG = samples > 0.f ? (float)gSum / samples : 0.f;
            sOutB = samples > 0.f ? (float)bSum / samples : 0.f;
            sOutValid = true;
        }

        int suspect = 0;
        float worstDelta = 0.f;
        for (int row = 1; row < HEIGHT - 1; row++) {
            float up = sRowMean[row - 1];
            float dn = sRowMean[row + 1];
            float neighbourSpread = fabsf(up - dn);
            float centreDelta    = fabsf(sRowMean[row] - (up + dn) * 0.5f);

            if (centreDelta > worstDelta) worstDelta = centreDelta;

            if (neighbourSpread < 4.0f && centreDelta > 14.0f)
                suspect++;
        }

        std::lock_guard<std::mutex> lock(sDiagLock);

        sDiag.outRMean = sOutR;
        sDiag.outGMean = sOutG;
        sDiag.outBMean = sOutB;
        sDiag.outValid = sOutValid;

        if (!suspect) return;

        sDiag.tearFrames++;
        sDiag.tearRows += (uint64_t)suspect;
        sDiag.tearLastFrameAt = SDL_GetTicks();
        if (suspect > sDiag.tearWorstRow) sDiag.tearWorstRow = suspect;
        if (worstDelta > sDiag.tearWorstDelta) sDiag.tearWorstDelta = worstDelta;
    }

    static bool sSampleDone = false;

    static void DumpSamplePixels(const uint8_t* nv12, int pitch,
                                 const uint8_t* rgba, int rgbaPitch) {
        if (sSampleDone) return;
        sSampleDone = true;

        static const int kSamples[][2] = {
            {0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0},
            {100, 0}, {320, 0}, {638, 0}, {639, 0},
            {0, 240}, {320, 240}, {639, 239}, {639, 479}, {0, 479},
        };

        ALBUM_LOG_QUIET("[CAM][diag] pixel dump  col/row : Y  U  V  ->  R G B A");
        for (auto& s : kSamples) {
            int col = s[0], row = s[1];
            int y = nv12[(size_t)row * pitch + col];
            int cj = col / 2, ci = row / 2;
            const uint8_t* uv = nv12 + (size_t)pitch * HEIGHT + (size_t)ci * pitch;
            int u = uv[cj * 2 + 0];
            int v = uv[cj * 2 + 1];

            const uint8_t* p = rgba + (size_t)row * rgbaPitch + (size_t)col * 4;
            ALBUM_LOG_QUIET("[CAM][diag]   %3d/%3d : %3d %3d %3d  ->  %3d %3d %3d %3d",
                         col, row, y, u, v, p[0], p[1], p[2], p[3]);
        }
    }

    static inline int Clamp8i(int v) {
        unsigned int u          = (unsigned int)v;
        unsigned int negative  = (unsigned int)(v >> 31);
        unsigned int capped    = u > 255u ? 255u : u;
        return (int)(capped & ~negative);
    }

    static inline void StorePixel(uint8_t* p, int r, int g, int b) {
        p[0] = (uint8_t)r;
        p[1] = (uint8_t)g;
        p[2] = (uint8_t)b;
        p[3] = 0xff;
    }

    static void Nv12ToRgba(const uint8_t* nv12, int pitch,
                           bool mirror, uint8_t* dst, int dstPitch,
                           int beginRow = 0, int endRow = HEIGHT) {
        constexpr int CW = WIDTH / 2;

        const uint8_t* yPlane  = nv12;
        const uint8_t* uvPlane = nv12 + (size_t)pitch * HEIGHT;

        for (int row = beginRow; row < endRow; row += 2) {
            const uint8_t* yTop = yPlane + (size_t)row * pitch;
            const uint8_t* yBot = yTop + pitch;
            const uint8_t* uv   = uvPlane + (size_t)(row / 2) * pitch;

            uint8_t* dTop = dst + (size_t)row * dstPitch;
            uint8_t* dBot = dTop + dstPitch;

            for (int col = 0; col < WIDTH; col += 2) {
                int a = col / 2 - 1;
                if (a < 0) a = 0;
                if (a > CW - 3) a = CW - 3;

                const uint8_t* pU = uv + a * 2;
                const uint8_t* pV = pU + 1;

                int uL, vL, uR, vR;
                if (col == 0) {
                    uL = pU[0];
                    vL = pV[0];
                    uR = (pU[0] * 192 + pU[2] * 64) >> 8;
                    vR = (pV[0] * 192 + pV[2] * 64) >> 8;
                } else {
                    uL = (pU[0] *  64 + pU[2] * 192) >> 8;
                    vL = (pV[0] *  64 + pV[2] * 192) >> 8;
                    uR = (pU[2] * 192 + pU[4] * 64) >> 8;
                    vR = (pV[2] * 192 + pV[4] * 64) >> 8;
                }

                int duL = uL - 128, dvL = vL - 128;
                int duR = uR - 128, dvR = vR - 128;

                int rvL = 409 * dvL, guL = 100 * duL, gvL = 208 * dvL, buL = 516 * duL;
                int rvR = 409 * dvR, guR = 100 * duR, gvR = 208 * dvR, buR = 516 * duR;

                int x0 = mirror ? (WIDTH - 1 - col) : col;
                int x1 = mirror ? (WIDTH - 2 - col) : (col + 1);

                int c = sLuma[yTop[col]];
                StorePixel(dTop + x0 * 4, Clamp8i((c + rvL + 128) >> 8),
                           Clamp8i((c - guL - gvL + 128) >> 8),
                           Clamp8i((c + buL + 128) >> 8));

                c = sLuma[yTop[col + 1]];
                StorePixel(dTop + x1 * 4, Clamp8i((c + rvR + 128) >> 8),
                           Clamp8i((c - guR - gvR + 128) >> 8),
                           Clamp8i((c + buR + 128) >> 8));

                c = sLuma[yBot[col]];
                StorePixel(dBot + x0 * 4, Clamp8i((c + rvL + 128) >> 8),
                           Clamp8i((c - guL - gvL + 128) >> 8),
                           Clamp8i((c + buL + 128) >> 8));

                c = sLuma[yBot[col + 1]];
                StorePixel(dBot + x1 * 4, Clamp8i((c + rvR + 128) >> 8),
                           Clamp8i((c - guR - gvR + 128) >> 8),
                           Clamp8i((c + buR + 128) >> 8));
            }
        }
    }

    static constexpr int NUM_CONVERT_THREADS = 2;
    static constexpr int NUM_NV12_SLOTS      = 2;
    static constexpr int NUM_RGBA_SLOTS      = 2;
    static constexpr int RGBA_PITCH          = WIDTH * 4;

    // Slot states
    static constexpr int SLOT_FREE    = 0;
    static constexpr int SLOT_PENDING = 1;
    static constexpr int SLOT_READY   = 2;

    static uint8_t*     sNv12Slot[NUM_NV12_SLOTS] = {nullptr};
    static uint8_t*     sRgbaSlot[NUM_RGBA_SLOTS] = {nullptr};
    static SDL_Surface* sRgbaSurf[NUM_RGBA_SLOTS] = {nullptr};
    static int          sNv12State[NUM_NV12_SLOTS] = {SLOT_FREE, SLOT_FREE};
    static int          sRgbaState[NUM_RGBA_SLOTS] = {SLOT_FREE, SLOT_FREE};
    static int          sRgbaHandedOut = -1;

    static std::thread  sConvertThreads[NUM_CONVERT_THREADS];
    static bool         sConvertStop  = false;
    static bool         sConvertReady = false;

    static std::mutex              sJobMutex;
    static std::condition_variable sJobCv;
    static bool         sJobActive  = false;
    static uint8_t*     sJobSrc     = nullptr;
    static uint8_t*     sJobDst     = nullptr;
    static bool         sJobMirror  = false;
    static int          sJobRgbaIndex = -1;
    static int          sJobNv12Index = -1;
    static int          sJobWorkersDone = 0;
    static int          sJobBands[NUM_CONVERT_THREADS + 1] = {0};

    static void ConvertWorker(int id) {
        for (;;) {
            uint8_t* src = nullptr;
            uint8_t* dst = nullptr;
            bool     mirror = false;
            int      begin = 0, end = 0;

            {
                std::unique_lock<std::mutex> lock(sJobMutex);
                sJobCv.wait(lock, [&]{ return sConvertStop || sJobActive; });
                if (sConvertStop) return;

                src    = sJobSrc;
                dst    = sJobDst;
                mirror = sJobMirror;
                begin  = sJobBands[id];
                end    = sJobBands[id + 1];
            }

            Nv12ToRgba(src, CAMERA_PITCH, mirror, dst, RGBA_PITCH, begin, end);

            {
                std::lock_guard<std::mutex> lock(sJobMutex);
                if (++sJobWorkersDone == NUM_CONVERT_THREADS) {
                    sJobActive   = false;
                    sRgbaState[sJobRgbaIndex] = SLOT_READY;
                    if (sJobNv12Index >= 0)
                        sNv12State[sJobNv12Index] = SLOT_FREE;
                    {
                        std::lock_guard<std::mutex> dlock(sDiagLock);
                        sDiag.jobsCompleted++;
                    }
                    sJobCv.notify_all();
                }
            }
        }
    }

    static void EnsureConvertWorkers() {
        if (sConvertReady) return;

        for (int i = 0; i < NUM_NV12_SLOTS; i++)
            sNv12Slot[i] = (uint8_t*)malloc(sFrameBufSize);
        for (int i = 0; i < NUM_RGBA_SLOTS; i++) {
            sRgbaSlot[i] = (uint8_t*)malloc((size_t)RGBA_PITCH * HEIGHT);
            if (sRgbaSlot[i])
                sRgbaSurf[i] = SDL_CreateRGBSurfaceWithFormatFrom(
                    sRgbaSlot[i], WIDTH, HEIGHT, 32, RGBA_PITCH,
                    SDL_PIXELFORMAT_RGBA32);
        }

        if (!sNv12Slot[0] || !sRgbaSlot[0]) {
            ALBUM_LOG("[CAM] could not allocate conversion slots");
            return;
        }

        sConvertStop = false;
        for (int i = 0; i < NUM_CONVERT_THREADS; i++)
            sConvertThreads[i] = std::thread(ConvertWorker, i);
        sConvertReady = true;
        ALBUM_LOG("[CAM] %d conversion threads started", NUM_CONVERT_THREADS);
    }

    static void StopConvertWorkers() {
        if (!sConvertReady) return;
        {
            std::lock_guard<std::mutex> lock(sJobMutex);
            sConvertStop = true;
        }
        sJobCv.notify_all();
        for (int i = 0; i < NUM_CONVERT_THREADS; i++)
            if (sConvertThreads[i].joinable()) sConvertThreads[i].join();

        for (int i = 0; i < NUM_NV12_SLOTS; i++) {
            free(sNv12Slot[i]);
            sNv12Slot[i] = nullptr;
            sNv12State[i] = SLOT_FREE;
        }
        for (int i = 0; i < NUM_RGBA_SLOTS; i++) {
            if (sRgbaSurf[i]) { SDL_FreeSurface(sRgbaSurf[i]); sRgbaSurf[i] = nullptr; }
            free(sRgbaSlot[i]);
            sRgbaSlot[i] = nullptr;
            sRgbaState[i] = SLOT_FREE;
        }
        sRgbaHandedOut = -1;
        sJobActive     = false;
        sConvertReady  = false;
        ALBUM_LOG("[CAM] conversion threads stopped");
    }

    static void OnCameraEvent(CAMEventData* data) {
        if (!data) return;

        if (data->eventType == CAMERA_DRC_DETACH) {
            std::lock_guard<std::mutex> lock(sDiagLock);
            sDiag.drcDetach++;
            sDiag.lastDetachAt = SDL_GetTicks();
            ALBUM_LOG("[CAM] DRC_DETACH connected=%d handle=%d",
                         (int)data->detach.connected, (int)data->detach.handle);
            return;
        }
        if (data->eventType != CAMERA_DECODE_DONE) return;

        void* surface = data->decode.surfaceBuffer;

        if (data->decode.failed) {
            std::lock_guard<std::mutex> lock(sDiagLock);
            sDiag.decodeFailed++;
        }

        if (!data->decode.failed && surface && sFrameBuf) {
            if (InWarmup()) {
                CheckCacheCoherency(surface);
                CheckDmaSettled(surface);
                ComputePlaneStats((const uint8_t*)surface);
#if CAM_DIAG_DUMP_NV12
                DumpNv12Frame((const uint8_t*)surface);
#endif
            } else {
                DCInvalidateRange(surface, (uint32_t)sFrameBufSize);
            }

            std::lock_guard<std::mutex> lock(sFrameLock);
            if (sFrameReady) {
                std::lock_guard<std::mutex> dlock(sDiagLock);
                sDiag.framesDropped++;
            }
            memcpy(sFrameBuf, surface, sFrameBufSize);
            sProducedSeq++;
            RecordProducerHash();
            sFrameReady = true;
            sHaveFrame  = true;

            std::lock_guard<std::mutex> dlock(sDiagLock);
            sDiag.framesProduced++;
        }

        std::lock_guard<std::mutex> lock(sSurfaceLock);
        if (sHandle < 0) return;
        bool resubmitted = false;
        for (int i = 0; i < NUM_SURFACES; i++) {
            if (sSurfaceBufs[i] == surface) {
                CAMSubmitTargetSurface(sHandle, &sSurfaces[i]);
                resubmitted = true;
                break;
            }
        }
        if (!resubmitted) {
            std::lock_guard<std::mutex> dlock(sDiagLock);
            sDiag.resyncFailures++;
        }
    }

    static void StopLibrary(bool closeStream) {
        if (sHandle < 0) return;
        CAMHandle handle = sHandle;
        {
            std::lock_guard<std::mutex> lock(sSurfaceLock);
            sHandle = -1;
        }
        if (closeStream) CAMClose(handle);
        CAMExit(handle);
    }

    static void ReleaseResources() {
        FreeSurfaceBufs();

        uint8_t* frameBuf = sFrameBuf;
        uint8_t* snapshot = sSnapshot;
        sFrameBuf = nullptr;
        sSnapshot = nullptr;
        if (frameBuf) free(frameBuf);
        if (snapshot) free(snapshot);

        SDL_Surface* scratch = sScratch;
        sScratch = nullptr;
        if (scratch) SDL_FreeSurface(scratch);

        uint8_t* diagScratch = sDiagScratch;
        sDiagScratch = nullptr;
        sDiagScratchSize = 0;
        if (diagScratch) free(diagScratch);

        sFrameBufSize = 0;

        {
            std::lock_guard<std::mutex> lock(sFrameLock);
            sFrameReady = false;
            sHaveFrame  = false;
        }

        void* workMem = sWorkMem;
        sWorkMem     = nullptr;
        sWorkMemSize = 0;
        if (workMem) free(workMem);
    }

    bool IsOpen() {
        return sStatus == Status::Running || sStatus == Status::Starting;
    }

    Status GetStatus() {
        return sStatus;
    }

    bool Open(int instance, bool highFrameRate) {
        if (sOpen) return true;

        for (int i = 0; i < 256; i++)
            sLuma[i] = 298 * (i - 16);

        ResetDiagnostics();
        sFramesSinceOpen.store(0);
        sWarmupDone.store(false);
        sSampleDone = false;
        sProducedSeq = 0;
        sConsumedSeq = 0;

        SetStatus(Status::Starting, nullptr);

        sFrameBufSize = (size_t)CAMERA_PITCH * HEIGHT * 3 / 2;
        sFrameBuf = (uint8_t*)calloc(1, sFrameBufSize);
        sSnapshot = (uint8_t*)malloc(sFrameBufSize);
        sDiagScratchSize = sFrameBufSize;
        sDiagScratch = (uint8_t*)malloc(sDiagScratchSize);
        sScratch  = SDL_CreateRGBSurfaceWithFormat(0, WIDTH, HEIGHT, 32,
                                                   SDL_PIXELFORMAT_RGBA32);
        if (!sFrameBuf || !sSnapshot || !sScratch) {
            SetStatus(Status::Error, "Out of memory preparing the camera buffers");
            ReleaseResources();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(sDiagLock);
            sDiag.scratchPitch = sScratch->pitch;
            sDiag.scratchPixelsAligned =
                ((((uintptr_t)sScratch->pixels) & 3u) == 0) ? 1 : 0;
        }
        ALBUM_LOG("[CAM] scratch surface %dx%d pitch=%d pixels=%p align4=%d",
                     sScratch->w, sScratch->h, (int)sScratch->pitch,
                     sScratch->pixels, (int)((((uintptr_t)sScratch->pixels) & 3u) == 0));

        CAMSetupInfo setup;
        memset(&setup, 0, sizeof(setup));
        setup.streamInfo.type   = CAMERA_STREAM_TYPE_1;
        setup.streamInfo.height = CAMERA_HEIGHT;
        setup.streamInfo.width  = CAMERA_WIDTH;

        setup.workMem.size = (uint32_t)CAMGetMemReq(&setup.streamInfo);
        if ((int32_t)setup.workMem.size <= 0) {
            SetStatus(Status::NoCamera, "The camera did not report its memory requirement");
            ReleaseResources();
            return false;
        }

        sWorkMemSize = setup.workMem.size;
        sWorkMem = AlignedAlloc(sWorkMemSize);
        if (!sWorkMem) {
            SetStatus(Status::Error, "Could not reserve %d bytes for camera.rpl",
                      (int)sWorkMemSize);
            ReleaseResources();
            return false;
        }
        setup.workMem.pMem = sWorkMem;
        setup.eventHandler = OnCameraEvent;
        setup.mode.forceDrc = FALSE;
        setup.mode.fps      = highFrameRate ? CAMERA_FPS_30 : CAMERA_FPS_15;
        setup.threadAffinity = OS_THREAD_ATTRIB_AFFINITY_ANY;

        CAMError segErr = CAMCheckMemSegmentation(sWorkMem, sWorkMemSize);
        if (segErr != CAMERA_ERROR_OK)
            ALBUM_LOG("[CAM] CAMCheckMemSegmentation reported %d", (int)segErr);

        CAMError err = CAMERA_ERROR_OK;
        sHandle = CAMInit(instance, &setup, &err);
        if (sHandle < 0) {
            sHandle = -1;
            switch (err) {
                case CAMERA_ERROR_UVC:
                    SetStatus(Status::NoCamera, "No camera is connected to the console");
                    break;
                case CAMERA_ERROR_DEVICE_IN_USE:
                    SetStatus(Status::NoCamera, "The camera is in use by another app");
                    break;
                case CAMERA_ERROR_INSUFFICIENT_MEMORY:
                    SetStatus(Status::Error, "Not enough free memory for the camera");
                    break;
                case CAMERA_ERROR_SEGMENT_VIOLATION:
                    SetStatus(Status::Error, "The camera rejected the work memory layout");
                    break;
                case CAMERA_ERROR_INVALID_ARG:
                    SetStatus(Status::Error, "The camera rejected the stream setup");
                    break;
                default:
                    SetStatus(Status::NoCamera, "The camera could not be started (error %d)", (int)err);
                    break;
            }
            ReleaseResources();
            return false;
        }

        for (int i = 0; i < NUM_SURFACES; i++) {
            sSurfaceBufs[i] = AlignedAlloc(CAMERA_YUV_BUFFER_SIZE);
            if (!sSurfaceBufs[i]) {
                SetStatus(Status::Error, "Could not allocate the camera frame buffers");
                StopLibrary(false);
                ReleaseResources();
                return false;
            }
            memset(sSurfaceBufs[i], 0, CAMERA_YUV_BUFFER_SIZE);

            sSurfaces[i].surfaceSize   = (int32_t)CAMERA_YUV_BUFFER_SIZE;
            sSurfaces[i].surfaceBuffer = sSurfaceBufs[i];
            sSurfaces[i].height        = CAMERA_HEIGHT;
            sSurfaces[i].width         = CAMERA_WIDTH;
            sSurfaces[i].pitch         = CAMERA_PITCH;
            sSurfaces[i].alignment     = CAMERA_YUV_BUFFER_ALIGNMENT;
            sSurfaces[i].tileMode      = 0;
            sSurfaces[i].pixelFormat   = 0;
        }

        CAMError openErr = CAMOpen(sHandle);
        if (openErr != CAMERA_ERROR_OK) {
            SetStatus(Status::Error, "The camera refused to open (error %d)", (int)openErr);
            StopLibrary(false);
            ReleaseResources();
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(sSurfaceLock);
            int queued = 0;
            for (int i = 0; i < NUM_SURFACES; i++) {
                if (CAMSubmitTargetSurface(sHandle, &sSurfaces[i]) == CAMERA_ERROR_OK)
                    queued++;
                else
                    break;
            }
            ALBUM_LOG("[CAM] queued %d/%d target surfaces", queued, NUM_SURFACES);
        }

        sOpen = true;
        SetStatus(Status::Starting, nullptr);
        ResetCounters();
        EnsureConvertWorkers();

        ALBUM_LOG("[CAM] opened instance=%d handle=%d workmem=%d fps=%d pitch=%d",
                     instance, (int)sHandle, (int)sWorkMemSize,
                     highFrameRate ? 30 : 15, (int)CAMERA_PITCH);
        return true;
    }

    void Close() {
        StopConvertWorkers();
        StopLibrary(true);
        sOpen = false;
        ReleaseResources();
        SetStatus(Status::Closed, nullptr);
        ALBUM_LOG("[CAM] closed");
    }

    const char* GetStatusMessage() {
        return sMessage[0] ? sMessage : "";
    }

    SDL_Surface* AcquireFrame(bool mirror, bool force) {
        if (!sScratch || !sFrameBuf || !sSnapshot) return nullptr;

        {
            std::lock_guard<std::mutex> lock(sJobMutex);
            if (sRgbaHandedOut >= 0) {
                sRgbaState[sRgbaHandedOut] = SLOT_FREE;
                sRgbaHandedOut = -1;
            }
        }

        bool haveNewInput = false;

        {
            std::lock_guard<std::mutex> lock(sFrameLock);
            if (!sHaveFrame) return nullptr;

            if (!force) {
                int nvIdx = -1, rgbaIdx = -1;
                {
                    std::lock_guard<std::mutex> jobLock(sJobMutex);
                    if (!sJobActive) {
                        for (int i = 0; i < NUM_NV12_SLOTS; i++)
                            if (sNv12State[i] == SLOT_FREE) { nvIdx = i; break; }
                        for (int i = 0; i < NUM_RGBA_SLOTS; i++)
                            if (sRgbaState[i] == SLOT_FREE) { rgbaIdx = i; break; }
                    }
                }

                if (nvIdx >= 0 && rgbaIdx >= 0) {
                    memcpy(sNv12Slot[nvIdx], sFrameBuf, sFrameBufSize);
                    sFrameReady = false;

                    std::lock_guard<std::mutex> jobLock(sJobMutex);
                    sNv12State[nvIdx]  = SLOT_PENDING;
                    sRgbaState[rgbaIdx] = SLOT_PENDING;
                    sJobSrc       = sNv12Slot[nvIdx];
                    sJobDst       = sRgbaSlot[rgbaIdx];
                    sJobRgbaIndex = rgbaIdx;
                    sJobNv12Index = nvIdx;
                    sJobMirror    = mirror;
                    sJobWorkersDone = 0;

                    int rowsPerBand = ((HEIGHT / NUM_CONVERT_THREADS) + 1) & ~1;
                    for (int i = 0; i <= NUM_CONVERT_THREADS; i++)
                        sJobBands[i] = std::min(i * rowsPerBand, HEIGHT);

                    sJobActive = true;
                    {
                        std::lock_guard<std::mutex> dlock(sDiagLock);
                        sDiag.jobsStarted++;
                    }
                    sJobCv.notify_all();
                    haveNewInput = true;
                } else {
                    std::lock_guard<std::mutex> dlock(sDiagLock);
                    sDiag.jobsRejected++;
                }
            } else {
                sConsumedSeq = sProducedSeq;
                VerifyConsumerHash();
                memcpy(sSnapshot, sFrameBuf, sFrameBufSize);
                sFrameReady = false;
            }
        }

        if (sStatus == Status::Starting)
            SetStatus(Status::Running, nullptr);

        SDL_Surface* result = nullptr;
        Uint32 t0 = SDL_GetTicks();

        if (force) {
            Nv12ToRgba(sSnapshot, CAMERA_PITCH, mirror,
                       (uint8_t*)sScratch->pixels, sScratch->pitch);
            result = sScratch;
        } else {
            std::lock_guard<std::mutex> lock(sJobMutex);
            for (int i = 0; i < NUM_RGBA_SLOTS; i++) {
                if (sRgbaState[i] == SLOT_READY && sRgbaSurf[i]) {
                    sRgbaHandedOut = i;
                    result = sRgbaSurf[i];
                    break;
                }
            }
        }

        float convertMs = (float)(SDL_GetTicks() - t0);
        if (result) {
            RunTearProbe((const uint8_t*)result->pixels, result->pitch);
            if (force)
                DumpSamplePixels(sSnapshot, CAMERA_PITCH,
                                 (const uint8_t*)result->pixels, result->pitch);
        }

        Uint32 now     = SDL_GetTicks();
        Uint32 started = sWindowStart;
        if (started == 0) {
            sWindowStart = now;
        } else if (now - started >= 500) {
            if (sWindowFrames > 0)
                sMeasuredFps = (float)sWindowFrames * 1000.f / (float)(now - started);
            sWindowStart  = now;
            sWindowFrames = 0;
        }
        sWindowFrames++;

        {
            std::lock_guard<std::mutex> lock(sDiagLock);
            if (haveNewInput || result) sDiag.framesConsumed++;
            sDiag.consumedFps = sMeasuredFps;
            float n = (float)(sDiag.framesConsumed ? sDiag.framesConsumed : 1);
            sDiag.convertMsAvg += (convertMs - sDiag.convertMsAvg) / n;
            if (convertMs > sDiag.convertMsMax) sDiag.convertMsMax = convertMs;
        }

        return result;
    }

    bool HasNewFrame() {
        std::lock_guard<std::mutex> lock(sFrameLock);
        return sFrameReady;
    }

    uint64_t GetFrameCount() {
        std::lock_guard<std::mutex> lock(sDiagLock);
        return sDiag.framesProduced;
    }

    float GetMeasuredFps() {
        return sMeasuredFps;
    }

    void ResetStats() {
        ResetCounters();
    }

    Diagnostics GetDiagnostics() {
        std::lock_guard<std::mutex> lock(sDiagLock);
        return sDiag;
    }

    void LogDiagnostics(const char* tag) {
        Diagnostics d = GetDiagnostics();
        const char* t = tag ? tag : "";

        ALBUM_LOG_QUIET("[CAM][diag] %s produced=%llu consumed=%llu dropped=%llu failed=%llu resync=%llu",
                     t,
                     (unsigned long long)d.framesProduced, (unsigned long long)d.framesConsumed,
                     (unsigned long long)d.framesDropped,  (unsigned long long)d.decodeFailed,
                     (unsigned long long)d.resyncFailures);
        ALBUM_LOG_QUIET("[CAM][diag] %s handoff %llu checked / %llu MISMATCH",
                     t, (unsigned long long)d.handoffChecks,
                     (unsigned long long)d.handoffMismatch);
        ALBUM_LOG_QUIET("[CAM][diag] %s cache   %llu checked / %llu STALE (avg %.1f%% of 32B lines)",
                     t, (unsigned long long)d.cacheChecks,
                     (unsigned long long)d.cacheStale, d.cacheStaleLinesPct);
        ALBUM_LOG_QUIET("[CAM][diag] %s dma     %llu checked / %llu STILL-CHANGING",
                     t, (unsigned long long)d.settleChecks,
                     (unsigned long long)d.settleChanged);
        ALBUM_LOG_QUIET("[CAM][diag] %s convert avg=%.2fms max=%.2fms consumedFps=%.1f",
                     t, d.convertMsAvg, d.convertMsMax, d.consumedFps);
        ALBUM_LOG_QUIET("[CAM][diag] %s TEAR frames=%llu rows=%llu worst=%d rows (delta %.1f) | DRCdetach=%llu",
                     t, (unsigned long long)d.tearFrames, (unsigned long long)d.tearRows,
                     d.tearWorstRow, d.tearWorstDelta, (unsigned long long)d.drcDetach);
        if (d.valid) {
            ALBUM_LOG_QUIET("[CAM][diag] %s luma  y=%.1f std=%.1f", t, d.yMean, d.yStd);
            ALBUM_LOG_QUIET("[CAM][diag] %s chroma pitch768 u=%6.1f v=%6.1f vspread=%6.1f",
                         t, d.uMeanPitchStride, d.vMeanPitchStride, d.vSpreadPitchStride);
            ALBUM_LOG_QUIET("[CAM][diag] %s chroma width640 u=%6.1f v=%6.1f vspread=%6.1f",
                         t, d.uMeanWidthStride, d.vMeanWidthStride, d.vSpreadWidthStride);
            ALBUM_LOG_QUIET("[CAM][diag] %s chroma unpadded u=%6.1f v=%6.1f vspread=%6.1f",
                         t, d.uMeanUnpadded, d.vMeanUnpadded, d.vSpreadUnpadded);
        }
    }

    void ResetDiagnostics() {
        std::lock_guard<std::mutex> lock(sDiagLock);
        sDiag = Diagnostics();
    }

    void SetDumpPath(const std::string& path) {
        sDumpPath = path;
    }

}
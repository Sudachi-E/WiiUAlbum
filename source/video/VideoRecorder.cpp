#include "VideoRecorder.hpp"
#include "../ui/Log.hpp"
#include "../audio/MicCapture.hpp"

#include <jpeglib.h>

#include <SDL.h>

#include <cstdlib>
#include <cstring>

static void W16(FILE* f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

static void W32(FILE* f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
    fwrite(b, 1, 4, f);
}

static void WI32(FILE* f, int32_t v) { W32(f, (uint32_t)v); }

static void WFourCC(FILE* f, const char* cc) { fwrite(cc, 1, 4, f); }

static long BeginChunk(FILE* f, const char* fourCC) {
    WFourCC(f, fourCC);
    long off = ftell(f);
    W32(f, 0);
    return off;
}

static long BeginList(FILE* f, const char* type) {
    WFourCC(f, "LIST");
    long off = ftell(f);
    W32(f, 0);
    WFourCC(f, type);
    return off;
}

static void EndChunk(FILE* f, long sizeOff) {
    long cur = ftell(f);
    fseek(f, sizeOff, SEEK_SET);
    W32(f, (uint32_t)(cur - sizeOff - 4));
    fseek(f, cur, SEEK_SET);
}

static void EndChunkAt(FILE* f, long sizeOff, long endPos) {
    long cur = ftell(f);
    fseek(f, sizeOff, SEEK_SET);
    W32(f, (uint32_t)(endPos - sizeOff - 4));
    fseek(f, cur, SEEK_SET);
}

// Lifecycle

VideoRecorder::VideoRecorder() {}
VideoRecorder::~VideoRecorder() { Stop(); }

double VideoRecorder::GetDuration() const {
    const uint32_t frames = mFrameCount.load();
    if (!frames) return 0.0;
    if (mFramePeriodUs > 0)
        return (double)frames * (double)mFramePeriodUs / 1000000.0;
    if (mRecording && mStartTicks)
        return (double)(SDL_GetTicks() - mStartTicks) / 1000.0;
    return (double)frames / (double)mFps;
}

bool VideoRecorder::Start(const std::string& path, int width, int height, int fps) {
    if (mRecording) return true;

    mPath   = path;
    mWidth  = width;
    mHeight = height;
    mFps    = fps > 0 ? fps : 30;
    mError.clear();
    mFrameCount.store(0);
    mAudioSamples.store(0);
    mBytesWritten.store(0);
    mMaxFrameSize = 0;
    mIndex.clear();
    mStopRequested.store(false);
    mStartTicks = SDL_GetTicks();
    mHasAudio = (mMic && mMic->IsOpen());

    for (int i = 0; i < STAGING_SLOTS; i++) {
        if (!mStaging[i])
            mStaging[i] = (uint8_t*)malloc((size_t)width * height * 4);
        if (!mStaging[i]) {
            mError = "Could not allocate frame staging memory";
            return false;
        }
        mStagingState[i].store(0);
    }
    if (!mRgbRows) mRgbRows = (uint8_t*)malloc((size_t)width * 3);
    if (!mRgbRows) {
        mError = "Could not allocate the JPEG row buffer";
        return false;
    }

    mFile = fopen(path.c_str(), "wb");
    if (!mFile) {
        mError = "Could not open " + path + " for writing";
        return false;
    }

    setvbuf(mFile, nullptr, _IOFBF, 256 * 1024);

    if (!WriteHeader(width, height, mFps, mHasAudio)) {
        fclose(mFile);
        mFile = nullptr;
        if (mError.empty()) mError = "Could not write the AVI header";
        return false;
    }

    mRecording = true;
    mThread = std::thread(&VideoRecorder::RecorderThread, this);
    ALBUM_LOG("[REC] started %s (%dx%d @%d, audio=%d)",
              path.c_str(), width, height, mFps, (int)mHasAudio);
    return true;
}

void VideoRecorder::Stop() {
    if (!mRecording && !mThread.joinable()) {
        if (mFile) { fclose(mFile); mFile = nullptr; }
        return;
    }

    const Uint32 t0 = SDL_GetTicks();
    ALBUM_LOG("[REC] stop: begin (frames so far %u)", mFrameCount.load());

    mStopRequested.store(true);
    if (mThread.joinable()) mThread.join();
    ALBUM_LOG("[REC] stop: thread joined after %u ms", SDL_GetTicks() - t0);

    PatchSizes();
    ALBUM_LOG("[REC] stop: sizes patched after %u ms", SDL_GetTicks() - t0);

    if (mFile) {
        fclose(mFile);
        mFile = nullptr;
    }
    ALBUM_LOG("[REC] stop: file closed after %u ms", SDL_GetTicks() - t0);

    // Tear down the JPEG encoder.
    if (mJpegComp) {
        jpeg_destroy_compress((jpeg_compress_struct*)mJpegComp);
        free(mJpegComp);
        mJpegComp = nullptr;
    }
    if (mJpegErr) { free(mJpegErr); mJpegErr = nullptr; }
    if (mJpegBuf) { free(mJpegBuf); mJpegBuf = nullptr; }
    if (mRgbRows)  { free(mRgbRows);  mRgbRows = nullptr; }
    for (int i = 0; i < STAGING_SLOTS; i++) {
        if (mStaging[i]) { free(mStaging[i]); mStaging[i] = nullptr; }
        mStagingState[i].store(0);
    }
    mRecording = false;

    ALBUM_LOG("[REC] stopped %s frames=%u audioSamples=%u duration=%.1fs bytes=%u (total %u ms)",
              mPath.c_str(), mFrameCount.load(), mAudioSamples.load(),
              GetDuration(), mBytesWritten.load(), SDL_GetTicks() - t0);
    if (!mError.empty()) ALBUM_LOG("[REC] error: %s", mError.c_str());
}

// Header

bool VideoRecorder::WriteHeader(int width, int height, int fps, bool hasAudio) {
    FILE* f = mFile;
    uint32_t framePeriodUs = (uint32_t)(1000000u / (fps > 0 ? fps : 30));
    if (framePeriodUs < 1) framePeriodUs = 1;

    WFourCC(f, "RIFF");
    mRiffSizeOff = ftell(f);
    W32(f, 0);
    WFourCC(f, "AVI ");

    long hdrlSz = BeginList(f, "hdrl");
    {
        long sz = BeginChunk(f, "avih");
        mFramePeriodOff = ftell(f);
        W32(f, framePeriodUs);
        W32(f, (uint32_t)(width * height * 3 * fps));
        W32(f, 0);
        W32(f, 0x10);
        mTotalFramesOff = ftell(f);
        W32(f, 0);
        W32(f, 0);
        W32(f, hasAudio ? 2u : 1u);
        W32(f, (uint32_t)(width * height));
        W32(f, (uint32_t)width);
        W32(f, (uint32_t)height);
        W32(f, 0); W32(f, 0); W32(f, 0); W32(f, 0);
        EndChunk(f, sz);
    }
    {
        // Video stream: MJPG.
        long strlSz = BeginList(f, "strl");
        {
            long sz = BeginChunk(f, "strh");
            WFourCC(f, "vids"); WFourCC(f, "MJPG");
            W32(f, 0);
            W16(f, 0); W16(f, 0);
            W32(f, 0);

            mVideoScaleOff = ftell(f);
            W32(f, framePeriodUs);
            W32(f, 1000000u);
            W32(f, 0);
            mVideoLengthOff = ftell(f);
            W32(f, 0);
            W32(f, 0);
            W32(f, (uint32_t)-1);
            W32(f, 0);
            W16(f, 0); W16(f, 0);
            W16(f, (uint16_t)width); W16(f, (uint16_t)height);
            EndChunk(f, sz);
        }
        {
            long sz = BeginChunk(f, "strf");
            W32(f, 40);
            WI32(f, (int32_t)width);
            WI32(f, (int32_t)height);
            W16(f, 1);
            W16(f, 24);
            WFourCC(f, "MJPG");
            W32(f, (uint32_t)(width * height * 3));
            WI32(f, 0); WI32(f, 0);
            W32(f, 0); W32(f, 0);
            EndChunk(f, sz);
        }
        EndChunk(f, strlSz);
    }
    if (hasAudio) {
        const uint16_t channels   = MicCapture::CHANNELS;
        const uint16_t blockAlign = channels * 16 / 8;
        long strlSz = BeginList(f, "strl");
        {
            long sz = BeginChunk(f, "strh");
            WFourCC(f, "auds");
            W32(f, 0);
            W32(f, 0);
            W16(f, 0); W16(f, 0);
            W32(f, 0);
            W32(f, 1);
            mAudioStrhRateOff = ftell(f);
            W32(f, (uint32_t)MicCapture::SAMPLE_RATE);
            W32(f, 0);

            mAudioLengthOff = ftell(f);
            W32(f, 0);
            W32(f, 0);
            W32(f, 0);
            W32(f, blockAlign);
            W16(f, 0); W16(f, 0); W16(f, 0); W16(f, 0);
            EndChunk(f, sz);
        }
        {
            long sz = BeginChunk(f, "strf");
            W16(f, 0x0001);
            W16(f, channels);
            mAudioRateOff = ftell(f);
            W32(f, (uint32_t)MicCapture::SAMPLE_RATE);
            W32(f, (uint32_t)MicCapture::SAMPLE_RATE * blockAlign);
            W16(f, blockAlign);
            W16(f, 16);
            W16(f, 0);
            EndChunk(f, sz);
        }
        EndChunk(f, strlSz);
    }
    EndChunk(f, hdrlSz);

    mMoviSizeOff = BeginList(f, "movi");
    mMoviDataStart = mMoviSizeOff + 4 + 4;

    return true;
}

void VideoRecorder::PadAudioToVideo(uint32_t haveSamples, uint32_t wantSamples) {
    if (wantSamples <= haveSamples) return;

    const uint32_t pad = wantSamples - haveSamples;
    ALBUM_LOG("[REC] padding audio with %u samples of silence (%u -> %u)",
              pad, haveSamples, wantSamples);

    IndexEntry e;
    FILE* f = mFile;
    memcpy(e.id, "01wb", 4);
    e.flags  = 0x10;
    e.offset = (uint32_t)(ftell(f) - mMoviDataStart);

    // One chunk per 4096 samples, matching the size written in real time.
    static uint8_t zeros[4096 * 2];
    memset(zeros, 0, sizeof(zeros));
    uint32_t remaining = pad;
    const uint32_t chunkSamples = sizeof(zeros) / 2;

    while (remaining > 0) {
        uint32_t n = remaining < chunkSamples ? remaining : chunkSamples;
        WFourCC(f, "01wb");
        W32(f, n * 2);
        fwrite(zeros, 1, n * 2, f);
        mIndex.push_back(e);
        remaining -= n;
        e.offset += 8 + n * 2;
    }
}

void VideoRecorder::PatchSizes() {
    if (!mFile) return;

    const uint32_t frames = mFrameCount.load();

    uint32_t videoChunks = 0;
    for (const auto& e : mIndex) if (memcmp(e.id, "00dc", 4) == 0) videoChunks++;
    if (videoChunks != frames)
        ALBUM_ERROR("[REC] frame count mismatch: %u written, %u declared",
                    videoChunks, frames);

    const uint32_t storedSamples = mAudioSamples.load();
    uint32_t audioLength = storedSamples;

    const double elapsedSecs = mStartTicks
        ? (double)(SDL_GetTicks() - mStartTicks) / 1000.0 : 0.0;

    double videoSecs = (elapsedSecs > 0.05 && frames > 0)
        ? elapsedSecs
        : (mFps ? (double)frames / (double)mFps : 0.0);

    const uint32_t framePeriodUs =
        (uint32_t)(videoSecs * 1000000.0 / (double)frames + 0.5);
    const double actualFps = framePeriodUs ? (1000000.0 / framePeriodUs) : 0.0;
    mFramePeriodUs = framePeriodUs;

    uint32_t audioRate = (uint32_t)MicCapture::SAMPLE_RATE;
    if (mHasAudio && elapsedSecs > 0.05 && storedSamples > 0) {
        const double measured =
            (double)storedSamples / elapsedSecs / MicCapture::CHANNELS;
        if (measured >= 8000.0) audioRate = (uint32_t)(measured + 0.5);
    }

    if (mHasAudio) {
        const uint32_t wantSamples =
            (uint32_t)(videoSecs * (double)audioRate * MicCapture::CHANNELS + 0.5);
        if (wantSamples > audioLength) {
            PadAudioToVideo(audioLength, wantSamples);
            audioLength = wantSamples;
        }
    }

EndChunk(mFile, mMoviSizeOff);

    long idxSz = BeginChunk(mFile, "idx1");
    for (const auto& e : mIndex) {
        fwrite(e.id, 1, 4, mFile);
        W32(mFile, e.flags);
        W32(mFile, e.offset);
        W32(mFile, e.length);
    }
    EndChunk(mFile, idxSz);
    mBytesWritten.fetch_add((uint32_t)mIndex.size() * 16 + 8);

    const long fileEnd = ftell(mFile);

    fseek(mFile, mTotalFramesOff, SEEK_SET);
    W32(mFile, frames);

    if (mFramePeriodOff > 0) {
        fseek(mFile, mFramePeriodOff, SEEK_SET);
        W32(mFile, framePeriodUs);
    }
    if (mVideoScaleOff > 0) {
        fseek(mFile, mVideoScaleOff, SEEK_SET);
        W32(mFile, framePeriodUs);
    }

    if (mVideoLengthOff > 0) {
        fseek(mFile, mVideoLengthOff, SEEK_SET);
        W32(mFile, frames);
    }

    if (mHasAudio && audioLength > 0 && mAudioLengthOff > 0) {
        fseek(mFile, mAudioLengthOff, SEEK_SET);
        W32(mFile, audioLength);
    }

    if (mHasAudio && mAudioRateOff > 0) {
        fseek(mFile, mAudioRateOff, SEEK_SET);
        W32(mFile, audioRate);
    }
    if (mHasAudio && mAudioStrhRateOff > 0) {
        fseek(mFile, mAudioStrhRateOff, SEEK_SET);
        W32(mFile, audioRate);
    }

    EndChunkAt(mFile, mRiffSizeOff, fileEnd);
    fseek(mFile, fileEnd, SEEK_SET);

    const double audioSecs = mHasAudio
        ? (double)audioLength / ((double)audioRate * MicCapture::CHANNELS) : 0.0;
    ALBUM_LOG("[REC] layout: frames=%u videoChunks=%u idxEntries=%u moviDataStart=%ld "
              "framePeriod=%uus (%.2f fps actual, %d requested)",
              frames, videoChunks, (unsigned)mIndex.size(), mMoviDataStart,
              framePeriodUs, actualFps, mFps);
    ALBUM_LOG("[REC] track lengths: video=%.2fs (%u frames) audio=%.2fs "
              "(%u samples, %d ch @%u Hz)", videoSecs, frames, audioSecs,
              audioLength, MicCapture::CHANNELS, audioRate);
    if (mHasAudio && audioSecs < videoSecs - 0.2)
        ALBUM_ERROR("[MIC] audio is %.2fs short of the video; "
                    "the microphone is not keeping up", videoSecs - audioSecs);
    if (mHasAudio && mAudioStatCount) {
        const double mean = (double)mAudioSum / (double)mAudioStatCount;
        ALBUM_LOG("[MIC] raw samples: min=%d max=%d mean=%.1f (dc=%d) "
                  "over %u samples", mAudioMin, mAudioMax, mean, mAudioDc,
                  mAudioStatCount);
        if (mean > 8000.0 || mean < -8000.0)
            ALBUM_ERROR("[MIC] large DC offset %.0f removed; the hardware is "
                        "delivering offset-binary samples", mean);
        if (mAudioMin <= -32768 || mAudioMax >= 32767)
            ALBUM_ERROR("[MIC] input is clipping at full scale");
    }
}

bool VideoRecorder::WriteVideoChunk(const uint8_t* jpeg, size_t size) {
    FILE* f = mFile;
    const uint32_t chunkOffset = (uint32_t)(ftell(f) - mMoviDataStart);

    WFourCC(f, "00dc");
    W32(f, (uint32_t)size);
    fwrite(jpeg, 1, size, f);
    if (size & 1) { uint8_t pad = 0; fwrite(&pad, 1, 1, f); }

    IndexEntry e;
    memcpy(e.id, "00dc", 4);
    e.flags  = 0x10;
    e.offset = chunkOffset;
    e.length = (uint32_t)size;
    mIndex.push_back(e);

    if (size > mMaxFrameSize) mMaxFrameSize = (uint32_t)size;
    mBytesWritten.fetch_add((uint32_t)size + 8);
    return true;
}

bool VideoRecorder::WriteAudioChunk(const int16_t* samples, int count) {
    if (count <= 0) return true;

    FILE* f = mFile;
    const uint32_t bytes = (uint32_t)count * 2;
    const uint32_t chunkOffset = (uint32_t)(ftell(f) - mMoviDataStart);

    WFourCC(f, "01wb");
    W32(f, bytes);

    static uint8_t leBuf[2048];
    uint32_t remaining = bytes;
    static int16_t dcBuf[1024];
    const int16_t* src = samples;
    while (remaining > 0) {
        uint32_t chunk = remaining < sizeof(leBuf) ? remaining
                                                    : (uint32_t)sizeof(leBuf);
        uint32_t n = chunk / 2;
        for (uint32_t i = 0; i < n; i++) {
            const int32_t x = src[i];
            mAudioDc += (x - mAudioDc) >> 12;
            int32_t y = x - mAudioDc;
            if (y >  32767) y =  32767;
            if (y < -32768) y = -32768;
            dcBuf[i] = (int16_t)y;
            const uint16_t v = (uint16_t)dcBuf[i];
            leBuf[i * 2 + 0] = (uint8_t)(v & 0xFF);
            leBuf[i * 2 + 1] = (uint8_t)(v >> 8);
        }
        for (uint32_t i = 0; i < n; i++) {
            const int32_t x = src[i];
            if (x < mAudioMin) mAudioMin = x;
            if (x > mAudioMax) mAudioMax = x;
            mAudioSum += x;
        }
        mAudioStatCount += n;
        fwrite(leBuf, 1, chunk, f);
        src       += n;
        remaining -= chunk;
    }
    if (bytes & 1) { uint8_t pad = 0; fwrite(&pad, 1, 1, f); }

    IndexEntry e;
    memcpy(e.id, "01wb", 4);
    e.flags  = 0x10;
    e.offset = chunkOffset;
    e.length = bytes;
    mIndex.push_back(e);

    mBytesWritten.fetch_add(bytes + 8);
    mAudioSamples.fetch_add((uint32_t)count);
    return true;
}

bool VideoRecorder::EncodeJpeg(const uint8_t* rgba, int pitch) {
    if (!mJpegComp) {
        mJpegComp = malloc(sizeof(jpeg_compress_struct));
        mJpegErr  = malloc(sizeof(jpeg_error_mgr));
        if (!mJpegComp || !mJpegErr) {
            mError = "Could not allocate the JPEG encoder";
            return false;
        }
        jpeg_compress_struct* cinfo = (jpeg_compress_struct*)mJpegComp;
        jpeg_error_mgr*        jerr  = (jpeg_error_mgr*)mJpegErr;
        cinfo->err = jpeg_std_error(jerr);
        jpeg_create_compress(cinfo);
    }

    jpeg_compress_struct* cinfo = (jpeg_compress_struct*)mJpegComp;

    cinfo->image_width      = mWidth;
    cinfo->image_height     = mHeight;
    cinfo->input_components = 3;
    cinfo->in_color_space   = JCS_RGB;

    jpeg_set_defaults(cinfo);
    jpeg_set_quality(cinfo, mQuality, TRUE);
    cinfo->dct_method = JDCT_ISLOW;

    jpeg_mem_dest(cinfo, &mJpegBuf, &mJpegSize);

    cinfo->next_scanline = 0;
    jpeg_start_compress(cinfo, TRUE);

    for (int row = 0; row < mHeight; row++) {
        const uint8_t* src = rgba + (size_t)row * pitch;
        uint8_t*       dst = mRgbRows;
        for (int col = 0; col < mWidth; col++) {
            *dst++ = src[col * 4 + 0];
            *dst++ = src[col * 4 + 1];
            *dst++ = src[col * 4 + 2];
        }
        JSAMPROW rowPtr = (JSAMPROW)mRgbRows;
        jpeg_write_scanlines(cinfo, &rowPtr, 1);
    }

    jpeg_finish_compress(cinfo);
    return mJpegSize > 0;
}

void BrightenRgba(uint8_t* pixels, int w, int h, int stride) {
    if (!pixels || w <= 0 || h <= 0) return;
    const int GAIN_NUM = 5, GAIN_DEN = 4;
    const int LIFT = 24;

    for (int y = 0; y < h; y++) {
        uint8_t* p = pixels + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++, p += 4) {
            for (int c = 0; c < 3; c++) {
                int v = (p[c] * GAIN_NUM) / GAIN_DEN + LIFT;
                p[c] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    }
}

void VideoRecorder::PushFrame(const uint8_t* rgba, int pitch, int srcW, int srcH) {
    if (!mRecording || !rgba || srcW <= 0 || srcH <= 0) return;
    if (mWidth <= 0 || mHeight <= 0) return;

    if ((int)mXMap.size() != mWidth) {
        mXMap.resize(mWidth);
        for (int col = 0; col < mWidth; col++) mXMap[col] = col * srcW / mWidth;
    }
    const bool sameSize = (srcW == mWidth && srcH == mHeight);

    for (int i = 0; i < STAGING_SLOTS; i++) {
        if (mStagingState[i].load() != 0 || !mStaging[i]) continue;

        if (sameSize) {
            for (int row = 0; row < mHeight; row++)
                memcpy(mStaging[i] + (size_t)row * mWidth * 4,
                       rgba + (size_t)row * pitch,
                       (size_t)mWidth * 4);
        } else {
            for (int row = 0; row < mHeight; row++) {
                int sy = row * srcH / mHeight;
                if (sy >= srcH) sy = srcH - 1;
                const uint8_t* s = rgba + (size_t)sy * pitch;
                uint8_t* d = mStaging[i] + (size_t)row * mWidth * 4;
                for (int col = 0; col < mWidth; col++)
                    memcpy(d + (size_t)col * 4, s + (size_t)mXMap[col] * 4, 4);
            }
        }
        mStagingState[i].store(1);
        return;
    }
}

void VideoRecorder::RecorderThread() {
    ALBUM_LOG("[REC] encoder thread running");

    while (!mStopRequested.load()) {
        bool didWork = false;

        for (int i = 0; i < STAGING_SLOTS; i++) {
            if (mStagingState[i].load() != 1 || !mStaging[i]) continue;
            mStagingState[i].store(2);
            if (mBrighten)
                BrightenRgba(mStaging[i], mWidth, mHeight, mWidth * 4);
            if (EncodeJpeg(mStaging[i], mWidth * 4) &&
                WriteVideoChunk(mJpegBuf, (size_t)mJpegSize))
                mFrameCount.fetch_add(1);
            mStagingState[i].store(0);
            didWork = true;
        }

        if (mMic && mMic->IsOpen() && mFrameCount.load() > 0) {
            int got = mMic->Read(mMicBuf, MIC_BUF_SAMPLES);
            if (got > 0) {
                WriteAudioChunk(mMicBuf, got);
                didWork = true;
            }
        }

        SDL_Delay(didWork ? 1 : 8);
    }

    ALBUM_LOG("[REC] encoder thread stopping (frames=%u)", mFrameCount.load());

    for (int i = 0; i < STAGING_SLOTS; i++) {
        if (mStagingState[i].load() == 1 && mStaging[i]) {
            if (mBrighten)
                BrightenRgba(mStaging[i], mWidth, mHeight, mWidth * 4);
            if (EncodeJpeg(mStaging[i], mWidth * 4) &&
                WriteVideoChunk(mJpegBuf, (size_t)mJpegSize))
                mFrameCount.fetch_add(1);
            mStagingState[i].store(0);
        }
    }
    ALBUM_LOG("[REC] encoder thread exit");
}

void VideoRecorder::ValidateFile(const std::string& path) {
    const Uint32 t0 = SDL_GetTicks();

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        ALBUM_LOG("[REC][validate] could not reopen '%s'", path.c_str());
        return;
    }

    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) failures++;
        ALBUM_LOG("[REC][validate] %-34s %s", what, ok ? "OK" : "*** FAIL ***");
    };

    static const size_t HEAD = 4096;
    static uint8_t head[HEAD];
    fseek(f, 0, SEEK_SET);
    const size_t headLen = fread(head, 1, HEAD, f);

    auto fourcc = [&](size_t p, char* out) {
        if (p + 4 > headLen) return false;
        memcpy(out, head + p, 4);
        out[4] = '\0';
        return true;
    };
    auto le32 = [&](size_t p) -> uint32_t {
        if (p + 4 > headLen) return 0;
        return (uint32_t)head[p] | ((uint32_t)head[p + 1] << 8) |
               ((uint32_t)head[p + 2] << 16) | ((uint32_t)head[p + 3] << 24);
    };
    auto le16 = [&](size_t p) -> uint16_t {
        if (p + 2 > headLen) return 0;
        return (uint16_t)((uint32_t)head[p] | ((uint32_t)head[p + 1] << 8));
    };

    fseek(f, 0, SEEK_END);
    const long fileSize = ftell(f);

    char cc[8];
    check(fourcc(0, cc) && strcmp(cc, "RIFF") == 0, "signature is RIFF");
    check(fourcc(8, cc) && strcmp(cc, "AVI ") == 0, "form type is 'AVI '");
    const uint32_t riffSize = le32(4);
    check((long)riffSize + 8 == fileSize, "RIFF size matches file length");
    ALBUM_LOG("[REC][validate] file %ld bytes, riff declares %u",
              fileSize, riffSize);

    check(fourcc(12, cc) && strcmp(cc, "LIST") == 0, "hdrl LIST tag");
    check(fourcc(20, cc) && strcmp(cc, "hdrl") == 0, "hdrl list type");
    const uint32_t hdrlSize = le32(16);
    const long hdrlEnd = 20 + (long)hdrlSize;
    check(hdrlEnd <= fileSize, "hdrl size fits in file");

    bool sawAvih = false, sawVideoStrh = false, sawStrh = false;
    int  strhCount = 0, strlCount = 0;
    size_t p = 24;
    while (p + 8 <= (size_t)hdrlEnd) {
        char tag[8];
        if (!fourcc(p, tag)) break;
        const uint32_t csz = le32(p + 4);

        if (strcmp(tag, "avih") == 0) {
            sawAvih = true;
            check(csz == 56, "avih size is 56");
            const uint32_t frames  = le32(p + 8 + 16);
            const uint32_t streams = le32(p + 8 + 24);
            const uint32_t w       = le32(p + 8 + 32);
            const uint32_t h       = le32(p + 8 + 36);
            check(frames == mFrameCount.load(), "avih dwTotalFrames matches");
            ALBUM_LOG("[REC][validate] avih frames=%u streams=%u %ux%u",
                      frames, streams, w, h);
        } else if (strcmp(tag, "strh") == 0) {
            sawStrh = true;
            strhCount++;
            check(csz == 56, "strh size is 56");
            char type[8], handler[8];
            fourcc(p + 8, type);
            fourcc(p + 12, handler);
            const uint32_t scale = le32(p + 8 + 20);
            const uint32_t rate  = le32(p + 8 + 24);
            ALBUM_LOG("[REC][validate] strh[%d] type=%.4s handler=%.4s scale=%u rate=%u (%.1f fps)",
                      strhCount - 1, type, handler, scale, rate,
                      scale ? (double)rate / (double)scale : 0.0);
            if (strcmp(type, "vids") == 0) sawVideoStrh = true;
        } else if (strcmp(tag, "strf") == 0) {
            check(csz == 40 || csz == 18, "strf size is 40 or 18");
            if (csz == 40) {
                char comp[8];
                fourcc(p + 24, comp);
                check(le32(p + 12) > 0 && le32(p + 16) > 0,
                      "video strf dimensions positive");
                check(strcmp(comp, "MJPG") == 0, "video strf compression is MJPG");
            }
        } else if (strcmp(tag, "LIST") == 0) {
            const uint32_t sub = le32(p + 4);
            strlCount++;
            char stype[8];
            fourcc(p + 8, stype);
            check(strcmp(stype, "strl") == 0, "stream list type is strl");
            ALBUM_LOG("[REC][validate] strl[%d] size=%u", strlCount - 1, sub);
            size_t inner = p + 12;
            const size_t innerEnd = p + 8 + (size_t)sub;
            while (inner + 8 <= innerEnd) {
                char t2[8];
                if (!fourcc(inner, t2)) break;
                const uint32_t s2 = le32(inner + 4);
                if (strcmp(t2, "strh") == 0) {
                    sawStrh = true;
                    strhCount++;
                    check(s2 == 56, "strh size is 56");
                    char type[8], handler[8];
                    fourcc(inner + 8, type);
                    fourcc(inner + 12, handler);
                    const uint32_t scale = le32(inner + 8 + 20);
                    const uint32_t rate  = le32(inner + 8 + 24);
                    ALBUM_LOG("[REC][validate] strh[%d] type=%.4s handler=%.4s "
                              "scale=%u rate=%u (%.2f fps)",
                              strhCount - 1, type, handler, scale, rate,
                              scale ? (double)rate / (double)scale : 0.0);
                    if (strcmp(type, "vids") == 0) sawVideoStrh = true;
                } else if (strcmp(t2, "strf") == 0) {
                    check(s2 == 40 || s2 == 18, "strf size is 40 or 18");
                    if (s2 == 40) {
                        char comp[8];
                        fourcc(inner + 24, comp);
                        check(le32(inner + 12) > 0 && le32(inner + 16) > 0,
                              "video strf dimensions positive");
                        check(strcmp(comp, "MJPG") == 0,
                              "video strf compression is MJPG");
                    } else {
                        ALBUM_LOG("[REC][validate] audio strf: %u Hz, %u ch, %u bytes/sec",
                                  le32(inner + 8 + 4), le16(inner + 8 + 2),
                                  le32(inner + 8 + 6));
                    }
                }
                inner += 8 + (size_t)s2 + ((s2 & 1) ? 1 : 0);
            }
            check(inner == innerEnd, "strl chunks tile exactly");
            p += 8 + (size_t)sub + ((sub & 1) ? 1 : 0);
            continue;
        }
        p += 8 + (size_t)csz + ((csz & 1) ? 1 : 0);
    }
    check(sawAvih, "avih chunk present");
    check(sawVideoStrh, "video strh present");
    check(sawStrh, "at least one strh present");
    check(p == (size_t)hdrlEnd, "hdrl chunks tile exactly");
    ALBUM_LOG("[REC][validate] streams=%d index entries written=%u", strlCount, (unsigned)mIndex.size());

    const uint32_t moviSize = le32((size_t)hdrlEnd + 4);
    check(fourcc((size_t)hdrlEnd, cc) && strcmp(cc, "LIST") == 0, "movi LIST tag");
    check(fourcc((size_t)hdrlEnd + 8, cc) && strcmp(cc, "movi") == 0,
          "movi list type");
    const long moviEnd = hdrlEnd + 8 + (long)moviSize;
    check(moviEnd <= fileSize, "movi size fits in file");
    ALBUM_LOG("[REC][validate] movi data starts %ld, ends %ld (%u bytes)",
              hdrlEnd + 12, moviEnd, moviSize);

    fseek(f, moviEnd, SEEK_SET);
    if (fread(cc, 1, 4, f) != 4) cc[0] = '\0';
    cc[4] = '\0';
    check(strcmp(cc, "idx1") == 0, "idx1 tag");
    uint32_t idxSize = 0;
    {
        uint8_t b[4];
        if (fread(b, 1, 4, f) == 4)
            idxSize = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                      ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    }
    check(idxSize % 16 == 0, "idx1 size is a multiple of 16");
    check((long)idxSize == (long)mIndex.size() * 16, "idx1 entry count matches");
    check(moviEnd + 8 + (long)idxSize == fileSize, "idx1 runs to end of file");
    ALBUM_LOG("[REC][validate] idx1 size=%u entries=%u written=%u",
              idxSize, (unsigned)(idxSize / 16), (unsigned)mIndex.size());

    {
        fseek(f, moviEnd + 8 + 8, SEEK_SET);
        const uint32_t firstOffset = le32(ftell(f));
        check(firstOffset == 0, "first idx1 offset is 0 (chunk-relative)");
    }

    {
        uint8_t probe[16];
        fseek(f, hdrlEnd + 12, SEEK_SET);
        if (fread(probe, 1, sizeof(probe), f) == sizeof(probe)) {
            char vt[8];
            memcpy(vt, probe, 4);
            vt[4] = '\0';
            check(strcmp(vt, "00dc") == 0, "first movi chunk is 00dc");
            check(probe[8] == 0xFF && probe[9] == 0xD8,
                  "first video chunk is a JPEG (SOI)");
        } else {
            check(false, "could read the first movi chunk");
        }
    }
    {
        int badId = 0, badLen = 0, outOfRange = 0;
        uint32_t videoEntries = 0, audioEntries = 0;
        uint32_t audioNonZero = 0, audioTotal = 0;
        char firstBad[80] = "";
        const long moviDataStart = hdrlEnd + 12;

        for (size_t i = 0; i < mIndex.size(); i++) {
            const IndexEntry& e = mIndex[i];
            const bool isAudio = memcmp(e.id, "01wb", 4) == 0;
            if (isAudio) audioEntries++; else videoEntries++;

            const long pos = moviDataStart + (long)e.offset;
            if (pos < moviDataStart || pos + 8 > moviEnd) {
                outOfRange++;
                continue;
            }
            char got[8];
            fseek(f, pos, SEEK_SET);
            if (fread(got, 1, 4, f) != 4) { badId++; continue; }
            got[4] = '\0';
            if (memcmp(got, e.id, 4) != 0) {
                badId++;
                if (!firstBad[0])
                    snprintf(firstBad, sizeof(firstBad),
                             "entry %u wants '%s' but bytes there are '%s'",
                             (unsigned)i, e.id, got);
            }
            const long next = (i + 1 < mIndex.size())
                ? (long)mIndex[i + 1].offset
                : (moviEnd - moviDataStart);
            const long gap = next - ((long)e.offset + 8);
            if (gap != (long)e.length && gap != (long)e.length + 1) {
                badLen++;
                if (!firstBad[0])
                    snprintf(firstBad, sizeof(firstBad),
                             "entry %u length %u but gap to next is %ld",
                             (unsigned)i, e.length, gap);
            }
            if (isAudio && audioTotal < 32768) {
                static uint8_t buf[1024];
                const uint32_t n = e.length < sizeof(buf)
                    ? e.length : (uint32_t)sizeof(buf);
                fseek(f, pos + 8, SEEK_SET);
                if (fread(buf, 1, n, f) == n) {
                    audioTotal += n;
                    for (uint32_t k = 0; k < n; k++)
                        if (buf[k]) audioNonZero++;
                }
            }
        }
        ALBUM_LOG("[REC][validate] idx1 walk: %u video + %u audio entries, "
                  "%d bad id, %d bad length, %d out of range",
                  videoEntries, audioEntries, badId, badLen, outOfRange);
        if (firstBad[0]) ALBUM_LOG("[REC][validate] first problem: %s", firstBad);
        if (audioTotal)
            ALBUM_LOG("[REC][validate] audio payload: %u/%u non-zero bytes (%.1f%%)",
                      audioNonZero, audioTotal,
                      100.0 * (double)audioNonZero / (double)audioTotal);
        check(outOfRange == 0, "every idx1 offset is inside movi");
        check(badId == 0, "every idx1 offset lands on its chunk id");
        check(badLen == 0, "every chunk length reaches the next chunk");
        check(audioTotal == 0 || audioNonZero * 100 > audioTotal,
              "audio payload is not all zero");
    }

    fclose(f);
    ALBUM_LOG("[REC][validate] %s: %d failure(s) in %u ms",
              path.c_str(), failures, SDL_GetTicks() - t0);
    if (failures) mError = "The recorded file failed structural validation";
}
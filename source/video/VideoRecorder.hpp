#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

class MicCapture;

void BrightenRgba(uint8_t* pixels, int w, int h, int stride);

class VideoRecorder {
public:
    VideoRecorder();
    ~VideoRecorder();

    bool Start(const std::string& path, int width, int height, int fps);
    void Stop();
    bool IsRecording() const { return mRecording; }

    void ValidateFile(const std::string& path);

    void PushFrame(const uint8_t* rgba, int pitch, int srcW, int srcH);

    void SetMic(MicCapture* mic) { mMic = mic; }

    bool HasAudio() const { return mHasAudio; }

    void SetBrighten(bool on) { mBrighten = on; }

    uint32_t       GetFrameCount() const { return mFrameCount.load(); }
    uint32_t       GetBytesWritten() const { return mBytesWritten.load(); }
    double         GetDuration() const;
    const std::string& GetPath() const { return mPath; }
    const std::string& GetError() const { return mError; }

private:
    bool mBrighten = false;

    struct IndexEntry {
        char     id[4];
        uint32_t flags;
        uint32_t offset;
        uint32_t length;
    };

    bool WriteHeader(int width, int height, int fps, bool hasAudio);
    bool WriteVideoChunk(const uint8_t* jpeg, size_t size);
    bool WriteAudioChunk(const int16_t* samples, int count);
    bool EncodeJpeg(const uint8_t* rgba, int pitch);
    void RecorderThread();
    void PatchSizes();
    void PadAudioToVideo(uint32_t haveSamples, uint32_t wantSamples);

    FILE*  mFile = nullptr;
    std::string mPath;
    std::string mError;
    MicCapture* mMic = nullptr;

    int  mWidth  = 0;
    int  mHeight = 0;
    int  mFps    = 0;
    bool mHasAudio = false;
    bool mRecording = false;
    int  mQuality = 78;

    long mRiffSizeOff = -1;
    long mMoviSizeOff = -1;
    long mMoviDataStart = -1;
    long mTotalFramesOff = -1;
    long mVideoLengthOff = -1;
    long mAudioLengthOff = -1;
    long mFramePeriodOff = -1;
    long mVideoScaleOff = -1;
    long mAudioRateOff = -1;
    long mAudioStrhRateOff = -1;

    int32_t  mAudioDc = 0;
    int32_t  mAudioMin = 0;
    int32_t  mAudioMax = 0;
    int64_t  mAudioSum = 0;
    uint32_t mAudioStatCount = 0;
    uint32_t mStartTicks = 0;
    uint32_t mFramePeriodUs = 0;

    std::atomic<uint32_t> mFrameCount{0};
    std::atomic<uint32_t> mAudioSamples{0};
    std::atomic<uint32_t> mBytesWritten{0};
    uint32_t mMaxFrameSize = 0;

    std::vector<IndexEntry> mIndex;

    static constexpr int STAGING_SLOTS = 2;
    uint8_t* mStaging[STAGING_SLOTS] = {nullptr, nullptr};
    std::vector<int> mXMap;
    std::atomic<int> mStagingState[STAGING_SLOTS];

    void* mJpegComp = nullptr;
    void* mJpegErr  = nullptr;
    unsigned char* mJpegBuf  = nullptr;
    unsigned long  mJpegSize = 0;
    uint8_t* mRgbRows = nullptr;
    static constexpr int MIC_BUF_SAMPLES = 2048;
    int16_t mMicBuf[MIC_BUF_SAMPLES];

    std::atomic<bool> mStopRequested{false};
    std::thread mThread;
};
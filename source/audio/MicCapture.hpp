#pragma once

#include <cstdint>
#include <string>

class MicCapture {
public:
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int CHANNELS   = 1;

    static constexpr size_t MIN_SAMPLE_MAX = 0x2800;
    static constexpr size_t SAMPLE_MAX     = 0x4000;
    static constexpr size_t BUFFER_ALIGN   = 0x40;

    MicCapture();
    ~MicCapture();

    // instance: 0 = GamePad microphone, 1 = USB microphone.
    bool Open(int instance = 0);
    void Close();
    bool IsOpen() const { return mHandle >= 0; }

    int Read(int16_t* dest, int maxSamples);

    const char* GetError() const { return mError; }

private:
    int    mHandle   = -1;
    int    mInstance = 0;
    void*  mBuffer   = nullptr;
    size_t mSampleMaxCount = 0;
    int    mPollCount = 0;
    char   mError[128] = {0};
};
#include "MicCapture.hpp"
#include "../ui/Log.hpp"

#include <mic/mic.h>

#include <cstdlib>
#include <cstring>

MicCapture::MicCapture() {}

MicCapture::~MicCapture() {
    Close();
}

bool MicCapture::Open(int instance) {
    if (mHandle >= 0) return true;

    mSampleMaxCount = SAMPLE_MAX;

    size_t bytes = mSampleMaxCount * 2;
    size_t aligned = (bytes + (BUFFER_ALIGN - 1)) & ~(size_t)(BUFFER_ALIGN - 1);
    mBuffer = aligned_alloc(BUFFER_ALIGN, aligned);
    if (!mBuffer) {
        snprintf(mError, sizeof(mError), "Could not allocate the mic buffer");
        return false;
    }
    memset(mBuffer, 0, aligned);

    MICWorkMemory work;
    memset(&work, 0, sizeof(work));
    work.sampleMaxCount = mSampleMaxCount;
    work.sampleBuffer   = mBuffer;

    MICError err = MIC_ERROR_OK;
    mHandle = MICInit((MICInstance)instance, 0, &work, &err);
    if (mHandle < 0) {
        mHandle = -1;
        switch (err) {
            case MIC_ERROR_INIT:
                snprintf(mError, sizeof(mError),
                         "The microphone failed to initialise");
                break;
            case MIC_ERROR_INVALID_INSTANCE:
                snprintf(mError, sizeof(mError),
                         "No microphone on instance %d", instance);
                break;
            default:
                snprintf(mError, sizeof(mError),
                         "The microphone could not be opened (error %d)", (int)err);
                break;
        }
        free(mBuffer);
        mBuffer = nullptr;
        return false;
    }

    if (MICOpen(mHandle) != MIC_ERROR_OK) {
        snprintf(mError, sizeof(mError), "The microphone refused to open");
        MICUninit(mHandle);
        mHandle = -1;
        free(mBuffer);
        mBuffer = nullptr;
        return false;
    }

    mInstance  = instance;
    mPollCount = 0;
    mError[0]  = '\0';
    ALBUM_LOG("[MIC] opened instance=%d handle=%d sampleMax=%d",
                 instance, (int)mHandle, (int)mSampleMaxCount);
    return true;
}

void MicCapture::Close() {
    if (mHandle >= 0) {
        MICClose(mHandle);
        MICUninit(mHandle);
        mHandle = -1;
        ALBUM_LOG("[MIC] closed");
    }
    if (mBuffer) {
        free(mBuffer);
        mBuffer = nullptr;
    }
}

int MicCapture::Read(int16_t* dest, int maxSamples) {
    if (mHandle < 0 || !dest || maxSamples <= 0) return 0;

    MICStatus status;
    memset(&status, 0, sizeof(status));
    if (MICGetStatus(mHandle, &status) != MIC_ERROR_OK) return 0;

    if (mPollCount < 4) {
        ALBUM_LOG("[MIC] poll %d: state=0x%x availableData=%d bufferPos=%d",
                  mPollCount, status.state, status.availableData, status.bufferPos);
        mPollCount++;
    }

    if (status.availableData <= 0) return 0;

    const int16_t* src = (const int16_t*)mBuffer;

    int total = 0;
    for (;;) {
        MICStatus status;
        memset(&status, 0, sizeof(status));
        if (MICGetStatus(mHandle, &status) != MIC_ERROR_OK) break;

        int avail = status.availableData;
        int pos   = status.bufferPos;
        if (avail <= 0) break;
        if (pos < 0) pos = 0;
        if (pos + avail > (int)mSampleMaxCount) avail = (int)mSampleMaxCount - pos;
        if (avail <= 0) break;

        int n = avail;
        if (n > maxSamples - total) n = maxSamples - total;
        if (n <= 0) break;

        memcpy(dest + total, src + pos, (size_t)n * sizeof(int16_t));
        MICSetDataConsumed(mHandle, n);
        total += n;

        if (total >= maxSamples) break;
    }

    return total;
}
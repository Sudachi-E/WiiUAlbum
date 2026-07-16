#pragma once

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

class ClipEncoder {
public:
    ClipEncoder();
    ~ClipEncoder();

    bool Open(const std::string& inputPath, const std::string& outputPath,
              double startTime, double endTime);
    bool Process();
    double GetProgress() const;
    void Close();
    bool IsOpen() const { return mOutputCtx != nullptr; }
    const std::string& GetOutputPath() const { return mOutputPath; }

private:
    AVFormatContext* mInputCtx;
    AVFormatContext* mOutputCtx;
    int* mStreamMap;
    int  mNumStreams;
    double mStartTime;
    double mEndTime;
    int64_t mStartPts;
    int64_t mEndPts;
    int64_t mTotalDuration;
    int64_t mProcessedDuration;
    int mVideoStreamIndex;
    std::string mOutputPath;
    std::string mOutputFormat;
};

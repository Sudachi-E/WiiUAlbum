#include "ClipEncoder.hpp"
#include <cstring>
#include <whb/log.h>
#include <coreinit/debug.h>

ClipEncoder::ClipEncoder()
    : mInputCtx(nullptr), mOutputCtx(nullptr), mStreamMap(nullptr),
      mNumStreams(0), mStartTime(0.0), mEndTime(0.0),
      mStartPts(0), mEndPts(0), mTotalDuration(0), mProcessedDuration(0),
      mVideoStreamIndex(-1) {}

ClipEncoder::~ClipEncoder() {
    Close();
}

bool ClipEncoder::Open(const std::string& inputPath, const std::string& outputPath,
                       double startTime, double endTime) {
    mStartTime = startTime;
    mEndTime   = endTime;
    mOutputPath = outputPath;

    int ret = avformat_open_input(&mInputCtx, inputPath.c_str(), nullptr, nullptr);
    if (ret < 0) {
        OSReport("[CLIP] avformat_open_input failed: %d\n", ret);
        WHBLogPrintf("[CLIP] avformat_open_input failed: %d", ret);
        return false;
    }

    ret = avformat_find_stream_info(mInputCtx, nullptr);
    if (ret < 0) {
        OSReport("[CLIP] avformat_find_stream_info failed: %d\n", ret);
        WHBLogPrintf("[CLIP] avformat_find_stream_info failed: %d", ret);
        Close();
        return false;
    }

    struct { const char* name; const char* ext; } fmtTable[] = {
        {"mov", "mov"}, {"matroska", "mkv"},
        {"avi", "avi"}, {"flv", "flv"}, {nullptr, nullptr}
    };
    for (int i = 0; fmtTable[i].name; i++) {
        AVOutputFormat* fmt = av_guess_format(fmtTable[i].name, nullptr, nullptr);
        if (!fmt) continue;
        std::string tryPath = outputPath;
        size_t dot = tryPath.rfind('.');
        if (dot != std::string::npos)
            tryPath = tryPath.substr(0, dot + 1) + fmtTable[i].ext;
        else
            tryPath += "." + std::string(fmtTable[i].ext);

        AVFormatContext* tryCtx = nullptr;
        avformat_alloc_output_context2(&tryCtx, fmt, nullptr, tryPath.c_str());
        if (!tryCtx) continue;

        bool ok = true;
        for (int si = 0; si < (int)mInputCtx->nb_streams; si++) {
            AVStream* inSt = mInputCtx->streams[si];
            AVStream* outSt = avformat_new_stream(tryCtx, nullptr);
            if (!outSt) { ok = false; break; }
            if (avcodec_parameters_copy(outSt->codecpar, inSt->codecpar) < 0) { ok = false; break; }
            outSt->codecpar->codec_tag = 0;
            outSt->time_base = inSt->time_base;
            if (inSt->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                inSt->sample_aspect_ratio.num > 0)
                outSt->sample_aspect_ratio = inSt->sample_aspect_ratio;
        }
        if (!ok) { avformat_free_context(tryCtx); continue; }

        AVIOContext* pb = nullptr;
        if (avio_open(&pb, tryPath.c_str(), AVIO_FLAG_WRITE) < 0) {
            avformat_free_context(tryCtx);
            continue;
        }
        tryCtx->pb = pb;
        if (avformat_write_header(tryCtx, nullptr) >= 0) {
            av_write_trailer(tryCtx);
            avio_closep(&pb);
            avformat_free_context(tryCtx);
            mOutputFormat = fmtTable[i].name;
            break;
        }
        avio_closep(&pb);
        avformat_free_context(tryCtx);
    }
    if (mOutputFormat.empty()) {
        OSReport("[CLIP] no suitable output muxer found\n");
        WHBLogPrintf("[CLIP] no suitable output muxer found");
        Close();
        return false;
    }

    {
        std::string tryPath = outputPath;
        size_t dot = tryPath.rfind('.');
        for (int i = 0; fmtTable[i].name; i++) {
            if (mOutputFormat == fmtTable[i].name) {
                if (dot != std::string::npos)
                    tryPath = tryPath.substr(0, dot + 1) + fmtTable[i].ext;
                else
                    tryPath += "." + std::string(fmtTable[i].ext);
                break;
            }
        }
        mOutputPath = tryPath;
    }

    {
        AVOutputFormat* fmt = av_guess_format(mOutputFormat.c_str(), nullptr, nullptr);
        if (!fmt) { Close(); return false; }
        int r = avformat_alloc_output_context2(&mOutputCtx, fmt, nullptr, mOutputPath.c_str());
        if (r < 0 || !mOutputCtx) { Close(); return false; }
    }
    OSReport("[CLIP] using muxer %s -> %s\n", mOutputFormat.c_str(), mOutputPath.c_str());
    WHBLogPrintf("[CLIP] using muxer %s -> %s", mOutputFormat.c_str(), mOutputPath.c_str());

    mNumStreams = mInputCtx->nb_streams;
    mStreamMap  = new int[mNumStreams];
    for (int i = 0; i < mNumStreams; i++) mStreamMap[i] = -1;

    for (int i = 0; i < mNumStreams; i++) {
        AVStream* inStream = mInputCtx->streams[i];
        AVStream* outStream = avformat_new_stream(mOutputCtx, nullptr);
        if (!outStream) {
            OSReport("[CLIP] avformat_new_stream failed for stream %d\n", i);
            WHBLogPrintf("[CLIP] avformat_new_stream failed for stream %d", i);
            Close();
            return false;
        }

        ret = avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
        if (ret < 0) {
            OSReport("[CLIP] avcodec_parameters_copy failed: %d\n", ret);
            WHBLogPrintf("[CLIP] avcodec_parameters_copy failed: %d", ret);
            Close();
            return false;
        }

        outStream->codecpar->codec_tag = 0;
        outStream->time_base = inStream->time_base;

        // Set expected duration right on the output stream
        double durSec = mEndTime - mStartTime;
        outStream->duration = (int64_t)(durSec * outStream->time_base.den / outStream->time_base.num + 0.5);

        if (inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            inStream->sample_aspect_ratio.num > 0)
            outStream->sample_aspect_ratio = inStream->sample_aspect_ratio;

        if (inStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            mVideoStreamIndex = i;
        }

        mStreamMap[i] = i;
    }

    ret = avio_open(&mOutputCtx->pb, mOutputPath.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        OSReport("[CLIP] avio_open failed for output: %d\n", ret);
        WHBLogPrintf("[CLIP] avio_open failed for output: %d", ret);
        Close();
        return false;
    }

    for (int i = 0; i < (int)mNumStreams; i++) {
        AVStream* os = mOutputCtx->streams[i];
        OSReport("[CLIP]  stream %d: codec=%d tb=%d/%d\n", i,
               os->codecpar->codec_id,
               os->time_base.num, os->time_base.den);
    }

    ret = avformat_write_header(mOutputCtx, nullptr);
    if (ret < 0) {
        OSReport("[CLIP] avformat_write_header failed: %d\n", ret);
        WHBLogPrintf("[CLIP] avformat_write_header failed: %d", ret);
        Close();
        return false;
    }

    return true;
}

bool ClipEncoder::Process() {
    if (!mInputCtx || !mOutputCtx) return false;

    // Seek to keyframe before start time
    int streamIndex = (mVideoStreamIndex >= 0) ? mVideoStreamIndex : 0;
    int64_t seekTs = (int64_t)(mStartTime / av_q2d(mInputCtx->streams[streamIndex]->time_base));
    av_seek_frame(mInputCtx, streamIndex, seekTs, AVSEEK_FLAG_BACKWARD);

    // Track first PTS per stream for offset calculation
    bool* firstPacket = new bool[mNumStreams]();
    int64_t* firstPts = new int64_t[mNumStreams]();

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = nullptr;
    pkt.size = 0;

    while (av_read_frame(mInputCtx, &pkt) >= 0) {
        if (pkt.stream_index >= mNumStreams || mStreamMap[pkt.stream_index] < 0) {
            av_packet_unref(&pkt);
            continue;
        }

        AVStream* inStream  = mInputCtx->streams[pkt.stream_index];

        // Convert PTS to seconds for range check
        double ptsSec = 0.0;
        if (pkt.pts != AV_NOPTS_VALUE) {
            ptsSec = pkt.pts * av_q2d(inStream->time_base);
        } else if (pkt.dts != AV_NOPTS_VALUE) {
            ptsSec = pkt.dts * av_q2d(inStream->time_base);
        }

        // Stop if passed the end time
        if (ptsSec > mEndTime + 0.5) {
            av_packet_unref(&pkt);
            break;
        }

        // Skip packets before start time
        if (ptsSec < mStartTime - 0.04) {
            av_packet_unref(&pkt);
            continue;
        }

        AVStream* outStream = mOutputCtx->streams[pkt.stream_index];

        // Store first PTS per stream for offset
        int si = pkt.stream_index;
        if (!firstPacket[si]) {
            firstPacket[si] = true;
            firstPts[si] = (pkt.pts != AV_NOPTS_VALUE) ? pkt.pts : pkt.dts;
        }

        // Remap timestamps to start from 0 for this stream
        int64_t offset = firstPts[si];
        if (pkt.pts != AV_NOPTS_VALUE)
            pkt.pts -= offset;
        if (pkt.dts != AV_NOPTS_VALUE)
            pkt.dts -= offset;

        pkt.pos = -1;
        av_packet_rescale_ts(&pkt, inStream->time_base, outStream->time_base);

        int ret = av_interleaved_write_frame(mOutputCtx, &pkt);
        if (ret < 0) {
            OSReport("[CLIP] av_interleaved_write_frame error: %d\n", ret);
            WHBLogPrintf("[CLIP] av_interleaved_write_frame error: %d", ret);
        }

        av_packet_unref(&pkt);
    }

    delete[] firstPacket;
    delete[] firstPts;

    av_write_trailer(mOutputCtx);

    OSReport("[CLIP] Clip encoding complete\n");
    WHBLogPrintf("[CLIP] Clip encoding complete");
    return true;
}

double ClipEncoder::GetProgress() const {
    return 0.0;
}

void ClipEncoder::Close() {
    if (mOutputCtx) {
        avio_closep(&mOutputCtx->pb);
        avformat_free_context(mOutputCtx);
        mOutputCtx = nullptr;
    }
    if (mInputCtx) {
        avformat_close_input(&mInputCtx);
    }
    delete[] mStreamMap;
    mStreamMap = nullptr;
    mNumStreams = 0;
    mVideoStreamIndex = -1;
}

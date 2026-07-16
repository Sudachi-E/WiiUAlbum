#include "VideoDecoder.hpp"
#include <whb/log.h>

VideoDecoder::VideoDecoder()
    : mFormatCtx(nullptr), mVideoCodecCtx(nullptr), mAudioCodecCtx(nullptr),
      mSwsCtx(nullptr), mSwrCtx(nullptr), mAvioCtx(nullptr), mFrame(nullptr),
      mFrameRGB(nullptr), mAudioFrame(nullptr), mPacket(nullptr),
      mVideoStreamIndex(-1), mAudioStreamIndex(-1), mWidth(0), mHeight(0),
      mDuration(0.0), mCurrentTime(0.0), mAudioTime(0.0), mBuffer(nullptr),
      mAvioBuffer(nullptr), mAudioBuffer(nullptr), mAudioBufferSize(0),
      mAudioBufferIndex(0), mAudioDevice(0), mPacketMutex(nullptr),
      mAudioPacket(nullptr), mPacketReaderThread(nullptr), mFile(nullptr) {
    mPacketMutex = SDL_CreateMutex();
    SDL_AtomicSet(&mReaderThreadRunning, 0);
}

VideoDecoder::~VideoDecoder() {
    Close();
    if (mPacketMutex) {
        SDL_DestroyMutex(mPacketMutex);
        mPacketMutex = nullptr;
    }
}

int VideoDecoder::ReadPacket(void* opaque, uint8_t* buf, int buf_size) {
    FILE* file = static_cast<FILE*>(opaque);
    return fread(buf, 1, buf_size, file);
}

int64_t VideoDecoder::Seek(void* opaque, int64_t offset, int whence) {
    FILE* file = static_cast<FILE*>(opaque);

    if (whence == AVSEEK_SIZE) {
        long pos = ftell(file);
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        fseek(file, pos, SEEK_SET);
        return size;
    }

    if (fseek(file, offset, whence) != 0) {
        return -1;
    }

    return ftell(file);
}

bool VideoDecoder::Open(const std::string& path) {
    mCurrentTime = 0.0;
    mAudioTime = 0.0;

    WHBLogPrintf("VideoDecoder::Open: Starting to open: %s", path.c_str());

    static bool ffmpegInitialized = false;
    if (!ffmpegInitialized) {
        WHBLogPrintf("VideoDecoder: FFmpeg initialized");
        ffmpegInitialized = true;
    }

    FILE* testFile = fopen(path.c_str(), "rb");
    if (testFile) {
        unsigned char header[16];
        size_t bytesRead = fread(header, 1, 16, testFile);
        fclose(testFile);
        if (bytesRead >= 4) {
            if (header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F')
                WHBLogPrintf("VideoDecoder: AVI/RIFF container detected");
            else if (bytesRead >= 8 && header[4]=='f'&&header[5]=='t'&&header[6]=='y'&&header[7]=='p')
                WHBLogPrintf("VideoDecoder: MP4 container detected");
        }
    } else {
        WHBLogPrintf("VideoDecoder: Could not open file for inspection");
    }

    mFormatCtx = nullptr;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "2000000", 0);
    av_dict_set(&opts, "analyzeduration", "500000", 0);

    int ret = avformat_open_input(&mFormatCtx, path.c_str(), nullptr, &opts);
    av_dict_free(&opts);

    if (ret != 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        WHBLogPrintf("VideoDecoder: Direct open failed (%d: %s), trying custom I/O", ret, errbuf);

        mFile = fopen(path.c_str(), "rb");
        if (!mFile) {
            WHBLogPrintf("VideoDecoder: fopen returned NULL");
            mWidth = -999;
            mHeight = 1;
            return false;
        }

        const int avio_buffer_size = 262144;
        mAvioBuffer = (uint8_t*)av_malloc(avio_buffer_size);
        if (!mAvioBuffer) {
            fclose(mFile);
            mFile = nullptr;
            mWidth = -998;
            mHeight = 1;
            return false;
        }

        mAvioCtx = avio_alloc_context(mAvioBuffer, avio_buffer_size, 0, mFile,
                                       &VideoDecoder::ReadPacket, nullptr, &VideoDecoder::Seek);
        if (!mAvioCtx) {
            av_free(mAvioBuffer);
            fclose(mFile);
            mFile = nullptr;
            mWidth = -997;
            mHeight = 1;
            return false;
        }

        mFormatCtx = avformat_alloc_context();
        if (!mFormatCtx) {
            mWidth = -996;
            mHeight = 1;
            Close();
            return false;
        }

        mFormatCtx->pb = mAvioCtx;
        mFormatCtx->probesize = 2000000;
        mFormatCtx->max_analyze_duration = 500000;

        ret = avformat_open_input(&mFormatCtx, nullptr, nullptr, nullptr);
        if (ret != 0) {
            mWidth = ret;
            mHeight = 1;
            Close();
            return false;
        }
    }

    if (avformat_find_stream_info(mFormatCtx, nullptr) < 0) {
        mWidth = -2;
        mHeight = 2;
        Close();
        return false;
    }

    for (unsigned i = 0; i < mFormatCtx->nb_streams; i++) {
        AVMediaType type = mFormatCtx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && mVideoStreamIndex < 0) {
            mVideoStreamIndex = i;
        }
        if (type == AVMEDIA_TYPE_AUDIO && mAudioStreamIndex < 0) {
            mAudioStreamIndex = i;
        }
    }

    if (mVideoStreamIndex == -1 && mAudioStreamIndex == -1) {
        mWidth = -3;
        mHeight = 3;
        Close();
        return false;
    }

    if (mVideoStreamIndex != -1) {
        AVCodecParameters* codecParams = mFormatCtx->streams[mVideoStreamIndex]->codecpar;

        const AVCodec* codec = nullptr;

        if (codecParams->codec_id == AV_CODEC_ID_H264) {
            codec = avcodec_find_decoder_by_name("h264_wiiu");
            if (!codec) {
                codec = avcodec_find_decoder(codecParams->codec_id);
            }
        } else {
            codec = avcodec_find_decoder(codecParams->codec_id);
        }

        if (!codec) {
            mWidth = codecParams->codec_id;
            mHeight = 4;
            mFailedCodecName = avcodec_get_name(codecParams->codec_id);
            Close();
            return false;
        }

        mVideoCodecCtx = avcodec_alloc_context3(codec);
        if (!mVideoCodecCtx) {
            mWidth = -5;
            mHeight = 5;
            Close();
            return false;
        }

        if (avcodec_parameters_to_context(mVideoCodecCtx, codecParams) < 0) {
            mWidth = -6;
            mHeight = 6;
            Close();
            return false;
        }

        mVideoCodecCtx->thread_count = 2;
        mVideoCodecCtx->thread_type = FF_THREAD_SLICE;
        if (codecParams->codec_id == AV_CODEC_ID_H264) {
            mVideoCodecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
            mVideoCodecCtx->skip_loop_filter = AVDISCARD_NONREF;
        }

        if (avcodec_open2(mVideoCodecCtx, codec, nullptr) < 0) {
            mWidth = -7;
            mHeight = 7;
            Close();
            return false;
        }

        mWidth = mVideoCodecCtx->width;
        mHeight = mVideoCodecCtx->height;
    } else {
        mWidth = 1;
        mHeight = 1;
    }

    if (mAudioStreamIndex != -1) {
        AVCodecParameters* audioCodecParams = mFormatCtx->streams[mAudioStreamIndex]->codecpar;

        int audioChannels = audioCodecParams->channels;
        int64_t audioChannelLayout = audioCodecParams->channel_layout;
        int audioSampleRate = audioCodecParams->sample_rate;
        if (audioChannels <= 0) audioChannels = 2;
        if (!audioChannelLayout) audioChannelLayout = av_get_default_channel_layout(audioChannels);

        const AVCodec* audioCodec = avcodec_find_decoder(audioCodecParams->codec_id);
        if (!audioCodec) {
            audioCodec = avcodec_find_decoder_by_name("mp3");
        }
        if (!audioCodec) {
            audioCodec = avcodec_find_decoder_by_name("mp3float");
        }

        if (audioCodec) {
            mAudioCodecCtx = avcodec_alloc_context3(audioCodec);
            if (mAudioCodecCtx) {
                if (avcodec_parameters_to_context(mAudioCodecCtx, audioCodecParams) < 0) {
                    avcodec_free_context(&mAudioCodecCtx);
                } else if (avcodec_open2(mAudioCodecCtx, audioCodec, nullptr) < 0) {
                    avcodec_free_context(&mAudioCodecCtx);
                } else {
                    mSwrCtx = swr_alloc();
                    if (mSwrCtx) {
                        av_opt_set_int(mSwrCtx, "in_channel_layout", audioChannelLayout, 0);
                        av_opt_set_int(mSwrCtx, "out_channel_layout", audioChannelLayout, 0);
                        av_opt_set_int(mSwrCtx, "in_sample_rate", audioSampleRate, 0);
                        av_opt_set_int(mSwrCtx, "out_sample_rate", audioSampleRate, 0);
                        av_opt_set_sample_fmt(mSwrCtx, "in_sample_fmt", mAudioCodecCtx->sample_fmt, 0);
                        av_opt_set_sample_fmt(mSwrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

                        swr_init(mSwrCtx);
                    }
                }
            }
        }
    }

    if (mFormatCtx->duration != AV_NOPTS_VALUE) {
        mDuration = mFormatCtx->duration / (double)AV_TIME_BASE;
    }

    mFrame = av_frame_alloc();
    mPacket = av_packet_alloc();
    mAudioPacket = av_packet_alloc();

    if (!mFrame || !mPacket || !mAudioPacket) {
        mWidth = -8;
        mHeight = 8;
        Close();
        return false;
    }

    if (mVideoStreamIndex != -1) {
        mFrameRGB = av_frame_alloc();
        if (!mFrameRGB) {
            mWidth = -8;
            mHeight = 8;
            Close();
            return false;
        }

        int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, mWidth, mHeight, 1);
        mBuffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
        av_image_fill_arrays(mFrameRGB->data, mFrameRGB->linesize, mBuffer,
                            AV_PIX_FMT_RGBA, mWidth, mHeight, 1);

        if (mVideoCodecCtx->pix_fmt == AV_PIX_FMT_NONE) {
            mWidth = -10;
            mHeight = 10;
            Close();
            return false;
        }

        int swsFlags = SWS_FAST_BILINEAR;
        if (mVideoCodecCtx->codec_id == AV_CODEC_ID_RAWVIDEO) {
            swsFlags = SWS_POINT;
        }

        mSwsCtx = sws_getContext(mWidth, mHeight, mVideoCodecCtx->pix_fmt,
                                mWidth, mHeight, AV_PIX_FMT_RGBA,
                                swsFlags, nullptr, nullptr, nullptr);

        if (!mSwsCtx) {
            mWidth = -9;
            mHeight = 9;
            Close();
            return false;
        }

        int srcRange = 0;
        int dstRange = 1;

        if (mVideoCodecCtx->pix_fmt == AV_PIX_FMT_YUVJ420P ||
            mVideoCodecCtx->pix_fmt == AV_PIX_FMT_YUVJ422P ||
            mVideoCodecCtx->pix_fmt == AV_PIX_FMT_YUVJ444P ||
            mVideoCodecCtx->pix_fmt == AV_PIX_FMT_YUVJ440P) {
            srcRange = 1;
        }

        int *inv_table, *table;
        int brightness, contrast, saturation;
        sws_getColorspaceDetails(mSwsCtx, &inv_table, &srcRange, &table, &dstRange,
                                 &brightness, &contrast, &saturation);
        sws_setColorspaceDetails(mSwsCtx, inv_table, srcRange, table, dstRange,
                                 brightness, contrast, saturation);
    }

    SDL_AtomicSet(&mReaderThreadRunning, 1);
    mPacketReaderThread = SDL_CreateThread(PacketReaderThreadFunc, "PacketReader", this);
    if (!mPacketReaderThread) {
        WHBLogPrintf("VideoDecoder: Failed to create packet reader thread");
    }

    return true;
}

void VideoDecoder::Close() {
    if (mPacketReaderThread) {
        SDL_AtomicSet(&mReaderThreadRunning, 0);
        SDL_WaitThread(mPacketReaderThread, nullptr);
        mPacketReaderThread = nullptr;
    }

    StopAudio();

    SDL_LockMutex(mPacketMutex);
    while (!mAudioPacketQueue.empty()) {
        AVPacket* pkt = mAudioPacketQueue.front();
        mAudioPacketQueue.pop_front();
        av_packet_free(&pkt);
    }
    while (!mVideoPacketQueue.empty()) {
        AVPacket* pkt = mVideoPacketQueue.front();
        mVideoPacketQueue.pop_front();
        av_packet_free(&pkt);
    }
    SDL_UnlockMutex(mPacketMutex);

    if (mSwsCtx) {
        sws_freeContext(mSwsCtx);
        mSwsCtx = nullptr;
    }

    if (mSwrCtx) {
        swr_free(&mSwrCtx);
        mSwrCtx = nullptr;
    }

    if (mBuffer) {
        av_free(mBuffer);
        mBuffer = nullptr;
    }

    if (mAudioBuffer) {
        av_free(mAudioBuffer);
        mAudioBuffer = nullptr;
        mAudioBufferSize = 0;
        mAudioBufferIndex = 0;
    }

    if (mFrameRGB) {
        av_frame_free(&mFrameRGB);
    }

    if (mAudioFrame) {
        av_frame_free(&mAudioFrame);
    }

    if (mFrame) {
        av_frame_free(&mFrame);
    }

    if (mPacket) {
        av_packet_free(&mPacket);
    }

    if (mAudioPacket) {
        av_packet_free(&mAudioPacket);
    }

    if (mVideoCodecCtx) {
        avcodec_free_context(&mVideoCodecCtx);
    }

    if (mAudioCodecCtx) {
        avcodec_free_context(&mAudioCodecCtx);
    }

    if (mFormatCtx) {
        avformat_close_input(&mFormatCtx);
    }

    if (mAvioCtx) {
        av_freep(&mAvioCtx->buffer);
        avio_context_free(&mAvioCtx);
    }

    if (mFile) {
        fclose(mFile);
        mFile = nullptr;
    }
    mAvioBuffer = nullptr;

    mVideoStreamIndex = -1;
    mAudioStreamIndex = -1;
}

bool VideoDecoder::ReadFrame(SDL_Texture* texture) {
    if (!mFormatCtx) {
        return false;
    }

    static int frameCount = 0;
    static Uint32 lastLogTime = 0;
    static int framesProcessed = 0;

    frameCount++;

    SDL_LockMutex(mPacketMutex);

    if (mVideoPacketQueue.empty()) {
        SDL_UnlockMutex(mPacketMutex);
        Uint32 now = SDL_GetTicks();
        if ((now - lastLogTime) > 5000) {
            WHBLogPrintf("[VIDEO] No video packets available");
            lastLogTime = now;
        }
        return true;
    }

    AVPacket* pkt = mVideoPacketQueue.front();
    mVideoPacketQueue.pop_front();
    int audioQueueSize = mAudioPacketQueue.size();
    int videoQueueSize = mVideoPacketQueue.size();

    SDL_UnlockMutex(mPacketMutex);

    if (avcodec_send_packet(mVideoCodecCtx, pkt) < 0) {
        av_packet_free(&pkt);
        return true;
    }

    int receiveResult = avcodec_receive_frame(mVideoCodecCtx, mFrame);

    if (receiveResult == 0) {
        if (mFrame->pts != AV_NOPTS_VALUE) {
            mCurrentTime = mFrame->pts * av_q2d(mFormatCtx->streams[mVideoStreamIndex]->time_base);
        } else {
            double frameRate = GetFrameRate();
            if (frameRate > 0) {
                mCurrentTime += 1.0 / frameRate;
            }
        }

        framesProcessed++;

        if (texture) {
            int result = sws_scale(mSwsCtx,
                                  (const uint8_t* const*)mFrame->data,
                                  mFrame->linesize,
                                  0, mHeight,
                                  mFrameRGB->data,
                                  mFrameRGB->linesize);

            if (result > 0) {
                SDL_UpdateTexture(texture, nullptr, mFrameRGB->data[0], mFrameRGB->linesize[0]);
            }
        }

        Uint32 now = SDL_GetTicks();
        if ((now - lastLogTime) > 5000) {
            WHBLogPrintf("[VIDEO] Frame #%d vPTS=%.2f aPTS=%.2f vQ=%d aQ=%d",
                         frameCount, mCurrentTime, mAudioTime, videoQueueSize, audioQueueSize);
            lastLogTime = now;
        }
    }

    av_packet_free(&pkt);
    return true;
}

bool VideoDecoder::Seek(double seconds) {
    if (!mFormatCtx) {
        return false;
    }

    // Stop the packet reader thread to prevent concurrent av_read_frame
    // on the AVFormatContext during av_seek_frame.
    if (mPacketReaderThread) {
        SDL_AtomicSet(&mReaderThreadRunning, 0);
        SDL_WaitThread(mPacketReaderThread, nullptr);
        mPacketReaderThread = nullptr;
    }

    // Clear packet queues (audio callback is blocked by mutex while we hold it)
    SDL_LockMutex(mPacketMutex);
    while (!mAudioPacketQueue.empty()) {
        AVPacket* pkt = mAudioPacketQueue.front();
        mAudioPacketQueue.pop_front();
        av_packet_free(&pkt);
    }
    while (!mVideoPacketQueue.empty()) {
        AVPacket* pkt = mVideoPacketQueue.front();
        mVideoPacketQueue.pop_front();
        av_packet_free(&pkt);
    }
    SDL_UnlockMutex(mPacketMutex);

    int streamIndex = (mVideoStreamIndex >= 0) ? mVideoStreamIndex : mAudioStreamIndex;
    if (streamIndex < 0) {
        SDL_AtomicSet(&mReaderThreadRunning, 1);
        mPacketReaderThread = SDL_CreateThread(PacketReaderThreadFunc, "PacketReader", this);
        return false;
    }

    int64_t timestamp = (int64_t)(seconds / av_q2d(mFormatCtx->streams[streamIndex]->time_base));

    // Safe: reader thread is stopped, audio callback is blocked by mutex
    if (av_seek_frame(mFormatCtx, streamIndex, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        SDL_AtomicSet(&mReaderThreadRunning, 1);
        mPacketReaderThread = SDL_CreateThread(PacketReaderThreadFunc, "PacketReader", this);
        return false;
    }

    if (mVideoCodecCtx) {
        avcodec_flush_buffers(mVideoCodecCtx);
    }
    if (mAudioCodecCtx) {
        avcodec_flush_buffers(mAudioCodecCtx);
    }

    mAudioBufferIndex = 0;
    mAudioBufferSize = 0;

    mCurrentTime = seconds;
    mAudioTime = seconds;

    // Restart the packet reader thread to resume filling queues from new position
    SDL_AtomicSet(&mReaderThreadRunning, 1);
    mPacketReaderThread = SDL_CreateThread(PacketReaderThreadFunc, "PacketReader", this);

    return true;
}

double VideoDecoder::GetFrameRate() const {
    if (!mFormatCtx || mVideoStreamIndex < 0) {
        return 30.0;
    }

    AVStream* stream = mFormatCtx->streams[mVideoStreamIndex];
    AVRational frameRate = stream->avg_frame_rate;

    if (frameRate.den > 0 && frameRate.num > 0) {
        return (double)frameRate.num / (double)frameRate.den;
    }

    frameRate = stream->r_frame_rate;
    if (frameRate.den > 0 && frameRate.num > 0) {
        return (double)frameRate.num / (double)frameRate.den;
    }

    return 30.0;
}

void AudioCallback(void* userdata, Uint8* stream, int len) {
    VideoDecoder* decoder = static_cast<VideoDecoder*>(userdata);

    SDL_memset(stream, 0, len);

    if (!decoder->mAudioCodecCtx || !decoder->mFormatCtx) {
        return;
    }

    int bytesWritten = 0;

    while (bytesWritten < len) {
        if (decoder->mAudioBufferIndex < decoder->mAudioBufferSize) {
            int bytesToCopy = decoder->mAudioBufferSize - decoder->mAudioBufferIndex;
            if (bytesToCopy > len - bytesWritten) {
                bytesToCopy = len - bytesWritten;
            }

            SDL_memcpy(stream + bytesWritten,
                      decoder->mAudioBuffer + decoder->mAudioBufferIndex,
                      bytesToCopy);

            bytesWritten += bytesToCopy;
            decoder->mAudioBufferIndex += bytesToCopy;
        } else {
            if (!decoder->mAudioFrame) {
                decoder->mAudioFrame = av_frame_alloc();
            }

            SDL_LockMutex(decoder->mPacketMutex);

            bool gotAudio = false;

            while (!gotAudio && !decoder->mAudioPacketQueue.empty()) {
                AVPacket* audioPkt = decoder->mAudioPacketQueue.front();
                decoder->mAudioPacketQueue.pop_front();

                int sendRet = avcodec_send_packet(decoder->mAudioCodecCtx, audioPkt);
                if (sendRet >= 0) {
                    int recvRet = avcodec_receive_frame(decoder->mAudioCodecCtx, decoder->mAudioFrame);
                    if (recvRet == 0) {
                        gotAudio = true;

                        if (decoder->mAudioFrame->pts != AV_NOPTS_VALUE) {
                            decoder->mAudioTime = decoder->mAudioFrame->pts *
                                av_q2d(decoder->mFormatCtx->streams[decoder->mAudioStreamIndex]->time_base);
                        }

                        int out_samples = (int)swr_get_delay(decoder->mSwrCtx, decoder->mAudioCodecCtx->sample_rate)
                                        + (int)av_rescale_rnd(decoder->mAudioFrame->nb_samples,
                                                               decoder->mAudioCodecCtx->sample_rate,
                                                               decoder->mAudioCodecCtx->sample_rate,
                                                               AV_ROUND_UP);

                        int out_size = av_samples_get_buffer_size(
                            nullptr,
                            decoder->mAudioCodecCtx->channels,
                            out_samples,
                            AV_SAMPLE_FMT_S16,
                            0
                        );

                        if (out_size > decoder->mAudioBufferSize) {
                            av_free(decoder->mAudioBuffer);
                            decoder->mAudioBuffer = (uint8_t*)av_malloc(out_size);
                            decoder->mAudioBufferSize = out_size;
                        }

                        uint8_t* out_buf = decoder->mAudioBuffer;
                        int converted_samples = swr_convert(
                            decoder->mSwrCtx,
                            &out_buf,
                            out_samples,
                            (const uint8_t**)decoder->mAudioFrame->data,
                            decoder->mAudioFrame->nb_samples
                        );

                        if (converted_samples > 0) {
                            decoder->mAudioBufferSize = av_samples_get_buffer_size(
                                nullptr,
                                decoder->mAudioCodecCtx->channels,
                                converted_samples,
                                AV_SAMPLE_FMT_S16,
                                0
                            );
                            decoder->mAudioBufferIndex = 0;
                        }
                    }
                }

                av_packet_free(&audioPkt);
            }

            SDL_UnlockMutex(decoder->mPacketMutex);

            if (!gotAudio) {
                break;
            }
        }
    }
}

SDL_Surface* VideoDecoder::GetCurrentFrameAsSurface() {
    if (!mFrameRGB || !mBuffer || mWidth <= 0 || mHeight <= 0)
        return nullptr;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
        0, mWidth, mHeight, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return nullptr;
    for (int y = 0; y < mHeight; y++) {
        memcpy((uint8_t*)surface->pixels + y * surface->pitch,
               mFrameRGB->data[0] + y * mFrameRGB->linesize[0],
               mWidth * 4);
    }
    return surface;
}

int VideoDecoder::GetVideoQueueSize() {
    SDL_LockMutex(mPacketMutex);
    int size = mVideoPacketQueue.size();
    SDL_UnlockMutex(mPacketMutex);
    return size;
}

void VideoDecoder::StartAudio() {
    if (!mAudioCodecCtx) {
        return;
    }
    if (mAudioDevice > 0) {
        return;
    }

    SDL_AudioSpec wanted_spec, obtained_spec;
    SDL_zero(wanted_spec);

    wanted_spec.freq = mAudioCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = mAudioCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = 1024;
    wanted_spec.callback = AudioCallback;
    wanted_spec.userdata = this;

    mAudioDevice = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &obtained_spec, 0);
    if (mAudioDevice == 0) {
        return;
    }

    SDL_PauseAudioDevice(mAudioDevice, 0);
}

void VideoDecoder::StopAudio() {
    if (mAudioDevice > 0) {
        SDL_CloseAudioDevice(mAudioDevice);
        mAudioDevice = 0;
    }
}

void VideoDecoder::PauseAudio(bool pause) {
    if (mAudioDevice > 0) {
        SDL_PauseAudioDevice(mAudioDevice, pause ? 1 : 0);
    }
}

int VideoDecoder::PacketReaderThreadFunc(void* data) {
    VideoDecoder* decoder = static_cast<VideoDecoder*>(data);
    decoder->PacketReaderLoop();
    return 0;
}

void VideoDecoder::PacketReaderLoop() {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        return;
    }

    while (SDL_AtomicGet(&mReaderThreadRunning)) {
        // Check queue sizes with a quick lock/unlock
        {
            SDL_LockMutex(mPacketMutex);
            bool full = mVideoPacketQueue.size() > 30 && mAudioPacketQueue.size() > 30;
            SDL_UnlockMutex(mPacketMutex);
            if (full) {
                SDL_Delay(10);
                continue;
            }
        }

        // Read frame WITHOUT holding the mutex (I/O is slow)
        int ret = av_read_frame(mFormatCtx, pkt);

        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                SDL_Delay(50);
                continue;
            } else {
                break;
            }
        }

        // Only lock for the queue push (fast)
        SDL_LockMutex(mPacketMutex);
        if (pkt->stream_index == mVideoStreamIndex) {
            AVPacket* videoPkt = av_packet_alloc();
            av_packet_ref(videoPkt, pkt);
            mVideoPacketQueue.push_back(videoPkt);
        } else if (pkt->stream_index == mAudioStreamIndex) {
            AVPacket* audioPkt = av_packet_alloc();
            av_packet_ref(audioPkt, pkt);
            mAudioPacketQueue.push_back(audioPkt);
        }
        av_packet_unref(pkt);
        SDL_UnlockMutex(mPacketMutex);
    }

    av_packet_free(&pkt);
}

SDL_Surface* VideoDecoder::ExtractThumbnail(const std::string& path, uint32_t* outDurationSec) {
    AVFormatContext* fmtCtx = nullptr;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "2000000", 0);
    av_dict_set(&opts, "analyzeduration", "500000", 0);
    if (avformat_open_input(&fmtCtx, path.c_str(), nullptr, &opts) != 0) {
        av_dict_free(&opts);
        return nullptr;
    }
    av_dict_free(&opts);
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return nullptr;
    }

    if (outDurationSec && fmtCtx->duration > 0) {
        *outDurationSec = (uint32_t)(fmtCtx->duration / AV_TIME_BASE);
    }

    int videoStream = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            break;
        }
    }
    if (videoStream < 0) {
        avformat_close_input(&fmtCtx);
        return nullptr;
    }

    const AVCodec* dec = avcodec_find_decoder(fmtCtx->streams[videoStream]->codecpar->codec_id);
    if (!dec) {
        avformat_close_input(&fmtCtx);
        return nullptr;
    }

    AVCodecContext* decCtx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(decCtx, fmtCtx->streams[videoStream]->codecpar);
    if (avcodec_open2(decCtx, dec, nullptr) < 0) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return nullptr;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();

    uint8_t* rgbBuffer = (uint8_t*)av_malloc(
        av_image_get_buffer_size(AV_PIX_FMT_RGB24, decCtx->width, decCtx->height, 1));
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer,
                         AV_PIX_FMT_RGB24, decCtx->width, decCtx->height, 1);

    struct SwsContext* swsCtx = sws_getContext(
        decCtx->width, decCtx->height, decCtx->pix_fmt,
        decCtx->width, decCtx->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    SDL_Surface* surface = nullptr;
    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == videoStream) {
            avcodec_send_packet(decCtx, pkt);
            int ret = avcodec_receive_frame(decCtx, frame);
            if (ret == 0) {
                sws_scale(swsCtx, frame->data, frame->linesize, 0, decCtx->height,
                          rgbFrame->data, rgbFrame->linesize);

                surface = SDL_CreateRGBSurfaceWithFormat(
                    0, decCtx->width, decCtx->height, 24, SDL_PIXELFORMAT_RGB24);
                if (surface) {
                    for (int y = 0; y < decCtx->height; y++) {
                        memcpy((uint8_t*)surface->pixels + y * surface->pitch,
                               rgbFrame->data[0] + y * rgbFrame->linesize[0],
                               decCtx->width * 3);
                    }
                }
                break;
            }
            if (ret == AVERROR(EAGAIN))
                continue;
            break;
        }
        av_packet_unref(pkt);
    }

    sws_freeContext(swsCtx);
    av_free(rgbBuffer);
    av_frame_free(&rgbFrame);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);
    return surface;
}

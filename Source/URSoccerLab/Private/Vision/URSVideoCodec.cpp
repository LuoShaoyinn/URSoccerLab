#include "URSVideoCodec.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <unistd.h>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
namespace URSoccerLab::Media
{
namespace
{
void Check(int Result, const char *Operation)
{
    if (Result >= 0)
        return;
    char Error[256];
    av_strerror(Result, Error, sizeof(Error));
    throw std::runtime_error(std::string(Operation) + ": " + Error);
}
std::string EncoderName(const Backend &B)
{
    return (B.Codec == "h265" ? "hevc" : B.Codec) + "_" + B.Api;
}
struct Decoder
{
    AVCodecContext *Context = nullptr;
    ~Decoder()
    {
        avcodec_free_context(&Context);
    }
};
Backend Validate(const Backend &B, Profile P)
{
    // Two GOPs verify both independent pictures and their dependent frames.
    P.Gop = 4;
    Codec Encode(B, P);
    const AVCodec *DecoderCodec = avcodec_find_decoder_by_name(B.Codec == "av1"    ? "libdav1d"
                                                               : B.Codec == "h265" ? "hevc"
                                                                                   : "h264");
    if (!DecoderCodec)
        throw std::runtime_error("validation decoder is unavailable");
    Decoder Decode;
    Decode.Context = avcodec_alloc_context3(DecoderCodec);
    if (!Decode.Context)
        throw std::runtime_error("allocate validation decoder");
    Decode.Context->thread_count = 1;
    Decode.Context->thread_type = FF_THREAD_SLICE;
    Check(avcodec_open2(Decode.Context, DecoderCodec, nullptr), "open validation decoder");
    int Decoded = 0, Packets = 0;
    Backend Result = B;
    auto Drain = [&]
    {
        Packet Encoded;
        while (Encode.Receive(Encoded))
        {
            ++Packets;
            if (!Decode.Context->extradata && !Encoded.Config.empty())
            {
                Decode.Context->extradata =
                    static_cast<uint8_t *>(av_mallocz(Encoded.Config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
                if (!Decode.Context->extradata)
                    throw std::runtime_error("allocate validation config");
                std::memcpy(Decode.Context->extradata, Encoded.Config.data(), Encoded.Config.size());
                Decode.Context->extradata_size = int(Encoded.Config.size());
            }
            AVPacket *Input = av_packet_alloc();
            if (!Input)
                throw std::runtime_error("allocate validation packet");
            int R = av_new_packet(Input, int(Encoded.Bytes.size()));
            if (R >= 0)
            {
                std::memcpy(Input->data, Encoded.Bytes.data(), Encoded.Bytes.size());
                R = avcodec_send_packet(Decode.Context, Input);
            }
            av_packet_free(&Input);
            Check(R, "decode calibration packet");
            AVFrame *Frame = av_frame_alloc();
            if (!Frame)
                throw std::runtime_error("allocate validation frame");
            while ((R = avcodec_receive_frame(Decode.Context, Frame)) >= 0)
            {
                bool Valid = Frame->width >= P.Width && Frame->height >= P.Height && Frame->width <= 65535 &&
                             Frame->height <= 65535;
                Result.DecodedWidth = std::max(Result.DecodedWidth, Frame->width);
                Result.DecodedHeight = std::max(Result.DecodedHeight, Frame->height);
                ++Decoded;
                av_frame_unref(Frame);
                if (!Valid)
                {
                    av_frame_free(&Frame);
                    throw std::runtime_error("validation frame is smaller than capture");
                }
            }
            av_frame_free(&Frame);
            if (R != AVERROR(EAGAIN) && R != AVERROR_EOF)
                Check(R, "receive calibration frame");
        }
    };
    for (int I = 0; I < 8; ++I)
    {
        Encode.Send(nullptr, I);
        Drain();
    }
    Encode.Flush();
    Drain();
    // No B frames or decoder frame threading: all eight must be immediately usable.
    if (Packets != 8 || Decoded != 8)
        throw std::runtime_error("calibration did not decode all eight frames");
    return Result;
}
bool SameSize(const Profile &A, const Profile &B)
{
    return A.Width == B.Width && A.Height == B.Height && A.Rate == B.Rate && A.Bitrate == B.Bitrate;
}
} // namespace
struct Codec::Impl
{
    Backend BackendConfig;
    Profile Capture;
    AVCodecContext *Encoder = nullptr;
    AVBufferRef *Device = nullptr;
    AVFrame *Cpu = nullptr;
    SwsContext *Converter = nullptr;
    AVPacket *Output = nullptr;
    ~Impl()
    {
        av_packet_free(&Output);
        av_frame_free(&Cpu);
        avcodec_free_context(&Encoder);
        av_buffer_unref(&Device);
        sws_freeContext(Converter);
    }
};
Codec::Codec(const Backend &B, const Profile &P) : State(new Impl)
{
    if (P.Width < 2 || P.Height < 2 || P.Width > 65535 || P.Height > 65535 || P.Width % 2 || P.Height % 2 ||
        !std::isfinite(P.Rate) || P.Rate <= 0 || P.Bitrate <= 0 || P.Gop <= 0)
        throw std::runtime_error(
            "Invalid video profile: NV12 requires positive even dimensions, rate, bitrate and GOP");
    auto &S = *State;
    S.BackendConfig = B;
    S.Capture = P;
    const std::string Name = EncoderName(B);
    const AVCodec *E = avcodec_find_encoder_by_name(Name.c_str());
    if (!E)
        throw std::runtime_error(Name + " is not included in this build");
    S.Encoder = avcodec_alloc_context3(E);
    if (!S.Encoder)
        throw std::runtime_error("allocate video encoder");
    auto *C = S.Encoder;
    C->width = P.Width;
    C->height = P.Height;
    C->time_base = av_d2q(1.0 / P.Rate, 1000000);
    C->framerate = av_d2q(P.Rate, 1000000);
    C->bit_rate = P.Bitrate;
    C->gop_size = P.Gop;
    C->max_b_frames = 0;
    AVDictionary *Options = nullptr;
    if (B.Api == "nvenc")
    {
        C->pix_fmt = AV_PIX_FMT_NV12;
        av_dict_set(&Options, "preset", "p1", 0);
        av_dict_set(&Options, "tune", "ll", 0);
        av_dict_set(&Options, "zerolatency", "1", 0);
        av_dict_set(&Options, "delay", "0", 0);
        if (!B.Device.empty())
            av_dict_set(&Options, "gpu", B.Device.c_str(), 0);
    }
    else
    {
        const AVHWDeviceType Type = B.Api == "vulkan" ? AV_HWDEVICE_TYPE_VULKAN
                                    : B.Api == "qsv"  ? AV_HWDEVICE_TYPE_QSV
                                                      : AV_HWDEVICE_TYPE_VAAPI;
        AVDictionary *DeviceOptions = nullptr;
        if (B.Api == "qsv" && !B.Device.empty())
            av_dict_set(&DeviceOptions, "child_device", B.Device.c_str(), 0);
        int R = av_hwdevice_ctx_create(&S.Device, Type, B.Api == "qsv" || B.Device.empty() ? nullptr : B.Device.c_str(),
                                       DeviceOptions, 0);
        av_dict_free(&DeviceOptions);
        Check(R, "create encoder device");
        AVBufferRef *Frames = av_hwframe_ctx_alloc(S.Device);
        if (!Frames)
            throw std::runtime_error("allocate hardware frames");
        auto *F = reinterpret_cast<AVHWFramesContext *>(Frames->data);
        C->pix_fmt = B.Api == "vulkan" ? AV_PIX_FMT_VULKAN : B.Api == "qsv" ? AV_PIX_FMT_QSV : AV_PIX_FMT_VAAPI;
        F->format = C->pix_fmt;
        F->sw_format = AV_PIX_FMT_NV12;
        F->width = P.Width;
        F->height = P.Height;
        F->initial_pool_size = B.Api == "qsv" ? 16 : 0;
        R = av_hwframe_ctx_init(Frames);
        if (R < 0)
        {
            av_buffer_unref(&Frames);
            Check(R, "initialize hardware frames");
        }
        C->hw_frames_ctx = Frames;
        av_dict_set(&Options, "async_depth", "1", 0);
        if (B.Api == "vulkan")
        {
            av_dict_set(&Options, "tune", "ll", 0);
            av_dict_set(&Options, "usage", "stream", 0);
        }
        if (B.Api == "vaapi")
            av_dict_set(&Options, "low_power", B.LowPower ? "1" : "0", 0);
    }
    int R = avcodec_open2(C, E, &Options);
    av_dict_free(&Options);
    Check(R, "open video encoder");
    S.Cpu = av_frame_alloc();
    if (!S.Cpu)
        throw std::runtime_error("allocate video staging frame");
    S.Cpu->format = AV_PIX_FMT_NV12;
    S.Cpu->width = P.Width;
    S.Cpu->height = P.Height;
    Check(av_frame_get_buffer(S.Cpu, 32), "allocate video staging buffer");
    S.Converter = sws_getContext(P.Width, P.Height, AV_PIX_FMT_BGRA, P.Width, P.Height, AV_PIX_FMT_NV12,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!S.Converter)
        throw std::runtime_error("create video pixel converter");
    S.Output = av_packet_alloc();
    if (!S.Output)
        throw std::runtime_error("allocate video output packet");
}
Codec::~Codec() = default;
void Codec::Send(const uint8_t *Pixels, int64_t Pts)
{
    auto &S = *State;
    Check(av_frame_make_writable(S.Cpu), "make staging writable");
    const int W = S.Capture.Width, H = S.Capture.Height;
    if (Pixels)
    {
        const uint8_t *Sources[] = {Pixels, nullptr, nullptr, nullptr};
        int Strides[] = {W * 4, 0, 0, 0};
        if (sws_scale(S.Converter, Sources, Strides, 0, H, S.Cpu->data, S.Cpu->linesize) != H)
            throw std::runtime_error("convert video frame");
    }
    else
    {
        for (int Y = 0; Y < H; ++Y)
            for (int X = 0; X < W; ++X)
                S.Cpu->data[0][Y * S.Cpu->linesize[0] + X] = uint8_t(16 + (X + Y + Pts * 9) % 220);
        for (int Y = 0; Y < H / 2; ++Y)
            std::memset(S.Cpu->data[1] + Y * S.Cpu->linesize[1], 128, W);
    }
    S.Cpu->pts = Pts;
    if (S.Encoder->hw_frames_ctx)
    {
        AVFrame *Gpu = av_frame_alloc();
        if (!Gpu)
            throw std::runtime_error("allocate hardware frame");
        int R = av_hwframe_get_buffer(S.Encoder->hw_frames_ctx, Gpu, 0);
        if (R >= 0)
        {
            Gpu->width = W;
            Gpu->height = H;
            R = av_hwframe_transfer_data(Gpu, S.Cpu, 0);
        }
        if (R >= 0)
        {
            Gpu->pts = Pts;
            R = avcodec_send_frame(S.Encoder, Gpu);
        }
        av_frame_free(&Gpu);
        Check(R, "upload/send video frame");
    }
    else
        Check(avcodec_send_frame(S.Encoder, S.Cpu), "send video frame");
}
bool Codec::Receive(Packet &P)
{
    auto &S = *State;
    int R = avcodec_receive_packet(S.Encoder, S.Output);
    if (R == AVERROR(EAGAIN) || R == AVERROR_EOF)
        return false;
    Check(R, "receive video packet");
    P.Bytes.assign(S.Output->data, S.Output->data + S.Output->size);
    if (S.Encoder->extradata_size)
        P.Config.assign(S.Encoder->extradata, S.Encoder->extradata + S.Encoder->extradata_size);
    else
        P.Config.clear();
    P.Pts = S.Output->pts;
    P.Key = (S.Output->flags & AV_PKT_FLAG_KEY) != 0;
    // Hardware encoders may omit padded dimensions from AVCodecContext.
    // Use sizes observed by the validation decoder as well as encoder metadata.
    P.Width = std::max({S.Encoder->coded_width, S.Capture.Width, S.BackendConfig.DecodedWidth});
    P.Height = std::max({S.Encoder->coded_height, S.Capture.Height, S.BackendConfig.DecodedHeight});
    av_packet_unref(S.Output);
    return true;
}
void Codec::Flush()
{
    Check(avcodec_send_frame(State->Encoder, nullptr), "flush video encoder");
}
Selection::Selection(Policy P) : Settings(std::move(P))
{
}
void Selection::AddProfile(const Profile &P)
{
    std::lock_guard<std::mutex> Lock(Mutex);
    // Later scene registrations are validated against the same selected backend.
    if (std::any_of(Settings.Profiles.begin(), Settings.Profiles.end(),
                    [&](const Profile &Existing) { return SameSize(Existing, P); }))
        return;
    Settings.Profiles.push_back(P);
}
Backend Selection::Get(const Profile &Actual)
{
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Failed)
        throw std::runtime_error(Failure);
    if (Chosen)
    {
        for (const auto &Profile : Checked)
            if (SameSize(Profile.Capture, Actual))
                return Profile.Result;
        Backend Validated = Validate(Selected, Actual);
        Checked.push_back({Actual, Validated});
        return Validated;
    }
    auto Profiles = Settings.Profiles;
    if (std::none_of(Profiles.begin(), Profiles.end(), [&](const Profile &P) { return SameSize(P, Actual); }))
        Profiles.push_back(Actual);
    std::vector<std::string> Codecs{Settings.Codec};
    if (!Settings.Fallback.empty() && Settings.Fallback != Settings.Codec)
        Codecs.push_back(Settings.Fallback);
    std::vector<std::string> Nodes;
    for (int I = 128; I < 192; ++I)
    {
        auto P = std::string("/dev/dri/renderD") + std::to_string(I);
        if (access(P.c_str(), R_OK | W_OK) == 0)
            Nodes.push_back(P);
    }
    for (const auto &Video : Codecs)
    {
        std::vector<Backend> Candidates;
        for (const std::string &Api : Settings.Backend == "auto"
                                          ? std::vector<std::string>{"nvenc", "qsv", "vaapi", "vulkan"}
                                          : std::vector<std::string>{Settings.Backend})
        {
            if (!Settings.Device.empty())
            {
                Candidates.push_back({Video, Api, Settings.Device, false});
                if (Api == "vaapi")
                    Candidates.push_back({Video, Api, Settings.Device, true});
            }
            else if (Api == "vaapi" || Api == "qsv")
            {
                for (const auto &Node : Nodes)
                {
                    Candidates.push_back({Video, Api, Node, false});
                    if (Api == "vaapi")
                        Candidates.push_back({Video, Api, Node, true});
                }
            }
            else
                Candidates.push_back({Video, Api, Api == "vulkan" ? "0" : "", false});
        }
        for (const auto &B : Candidates)
        {
            try
            {
                std::vector<ValidatedProfile> Validated;
                for (const auto &P : Profiles)
                    Validated.push_back({P, Validate(B, P)});
                Selected = B;
                Chosen = true;
                Checked = std::move(Validated);
                std::fprintf(stderr,
                             "[URS Encoder] selected codec=%s backend=%s device=%s (shared robot/guest policy; "
                             "encode/decode validated)\n",
                             B.Codec.c_str(), B.Api.c_str(), B.Device.empty() ? "auto" : B.Device.c_str());
                for (const auto &Profile : Checked)
                    if (SameSize(Profile.Capture, Actual))
                        return Profile.Result;
                throw std::runtime_error("Calibration profile was not registered");
            }
            catch (const std::exception &E)
            {
                std::fprintf(stderr, "[URS Encoder] rejected codec=%s backend=%s device=%s: %s\n", B.Codec.c_str(),
                             B.Api.c_str(), B.Device.c_str(), E.what());
            }
        }
    }
    Failure = "No validated video encoder for codec=" + Settings.Codec + ", backend=" + Settings.Backend +
              ", fallback=" + (Settings.Fallback.empty() ? "disabled" : Settings.Fallback);
    Failed = true;
    throw std::runtime_error(Failure);
}
} // namespace URSoccerLab::Media

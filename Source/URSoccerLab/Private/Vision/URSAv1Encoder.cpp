#include "Vision/URSAv1Encoder.h"
#include <atomic>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
namespace URSoccerLab
{
namespace { std::atomic<uint64> NextEpoch{1}; }
struct FAv1Encoder::FImpl
{
 FURSRgbStreamConfig Settings;
 AVCodecContext* Codec = nullptr;
 AVBufferRef* Device = nullptr;
 AVFrame* Cpu = nullptr;
 SwsContext* Convert = nullptr;
 int32 Width = 0, Height = 0;
 int64 NextPts = 0;
 uint64 Epoch = NextEpoch.fetch_add(1);
 bool Failed = false;
 TMap<int64, FEncodedCameraFrame> Pending;
 explicit FImpl(const FURSRgbStreamConfig& In) : Settings(In) {}
 ~FImpl() { avcodec_free_context(&Codec); av_frame_free(&Cpu); av_buffer_unref(&Device); sws_freeContext(Convert); }
 bool Error(int Code, const TCHAR* Operation)
 {
  char Buffer[256]; av_strerror(Code, Buffer, sizeof(Buffer));
  UE_LOG(LogTemp, Error, TEXT("[URS AV1] %s: %s"), Operation, UTF8_TO_TCHAR(Buffer));
  Failed = true; return false;
 }
 bool Open(int32 W, int32 H)
 {
  Width = W; Height = H;
  const AVCodec* Encoder = avcodec_find_encoder_by_name("av1_vulkan");
  if (!Encoder) return Error(AVERROR_ENCODER_NOT_FOUND, TEXT("FFmpeg av1_vulkan unavailable"));
  FTCHARToUTF8 DeviceName(*Settings.VulkanDevice);
  int Ret = av_hwdevice_ctx_create(&Device, AV_HWDEVICE_TYPE_VULKAN,
    Settings.VulkanDevice.IsEmpty() ? nullptr : DeviceName.Get(), nullptr, 0);
  if (Ret < 0) return Error(Ret, TEXT("create Vulkan encoder device"));
  AVBufferRef* Frames = av_hwframe_ctx_alloc(Device);
  if (!Frames) return Error(AVERROR(ENOMEM), TEXT("allocate Vulkan frames"));
  auto* Context = reinterpret_cast<AVHWFramesContext*>(Frames->data);
  Context->format = AV_PIX_FMT_VULKAN; Context->sw_format = AV_PIX_FMT_NV12;
  Context->width = W; Context->height = H;
  Ret = av_hwframe_ctx_init(Frames);
  if (Ret < 0) { av_buffer_unref(&Frames); return Error(Ret, TEXT("initialize Vulkan frames")); }
  Codec = avcodec_alloc_context3(Encoder);
  if (!Codec) { av_buffer_unref(&Frames); return Error(AVERROR(ENOMEM), TEXT("allocate AV1 context")); }
  Codec->width = W; Codec->height = H; Codec->pix_fmt = AV_PIX_FMT_VULKAN;
  Codec->time_base = av_d2q(1.0 / Settings.RateHz, 1000000);
  Codec->framerate = av_d2q(Settings.RateHz, 1000000);
  Codec->bit_rate = int64(Settings.BitrateKbps) * 1000;
  Codec->gop_size = FMath::Max(1, FMath::RoundToInt(Settings.RateHz * Settings.KeyframeIntervalSeconds));
  Codec->max_b_frames = 0;
  Codec->hw_frames_ctx = Frames;
  AVDictionary* Options = nullptr;
  av_dict_set(&Options, "tune", "ull", 0);
  av_dict_set(&Options, "async_depth", "1", 0);
  av_dict_set(&Options, "usage", "stream", 0);
  Ret = avcodec_open2(Codec, Encoder, &Options);
  av_dict_free(&Options);
  if (Ret < 0) return Error(Ret, TEXT("open Vulkan AV1 encoder (driver must support AV1 encode)"));
  Cpu = av_frame_alloc();
  if (!Cpu) return Error(AVERROR(ENOMEM), TEXT("allocate CPU staging frame"));
  Cpu->format = AV_PIX_FMT_NV12; Cpu->width = W; Cpu->height = H;
  Ret = av_frame_get_buffer(Cpu, 32);
  if (Ret < 0) return Error(Ret, TEXT("allocate NV12 staging buffer"));
  Convert = sws_getContext(W, H, AV_PIX_FMT_BGRA, W, H, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (!Convert) return Error(AVERROR(EINVAL), TEXT("create BGRA to NV12 converter"));
  UE_LOG(LogTemp, Display, TEXT("[URS AV1] Vulkan %dx%d %.1f Hz, %d kbps, keyframe interval %d frames, epoch %llu."),
    W, H, Settings.RateHz, Settings.BitrateKbps, Codec->gop_size, Epoch);
  return true;
 }
};
FAv1Encoder::FAv1Encoder(const FURSRgbStreamConfig& Settings) : Impl(MakeUnique<FImpl>(Settings)) {}
FAv1Encoder::~FAv1Encoder() = default;
bool FAv1Encoder::Encode(const TArray<FRawCameraImage>& Images, FEncodedCameraFrame& Frame)
{
 auto& S = *Impl;
 if (S.Failed || Images.IsEmpty() || Images.Num() > 2) return false;
 const auto& Left = Images[0];
 const int32 W = Left.Width * Images.Num(), H = Left.Height;
 for (const auto& Image : Images)
  if (Image.Width != Left.Width || Image.Height != H || Image.Pixels.Num() != Left.Width * H) return false;
 if (!S.Codec && !S.Open(W, H)) return false;
 if (S.Width != W || S.Height != H) return S.Error(AVERROR(EINVAL), TEXT("AV1 dimensions changed; recreate stream"));
 TArray<FColor> Packed;
 const FColor* Pixels = Left.Pixels.GetData();
 if (Images.Num() == 2)
 {
  Packed.SetNumUninitialized(W * H);
  for (int32 Y = 0; Y < H; ++Y)
   for (int32 Eye = 0; Eye < 2; ++Eye)
    FMemory::Memcpy(Packed.GetData() + Y*W + Eye*Left.Width,
                    Images[Eye].Pixels.GetData() + Y*Left.Width, Left.Width*sizeof(FColor));
  Pixels = Packed.GetData();
 }
 int Ret = av_frame_make_writable(S.Cpu);
 if (Ret < 0) return S.Error(Ret, TEXT("make staging writable"));
 const uint8* Sources[4] = {reinterpret_cast<const uint8*>(Pixels), nullptr, nullptr, nullptr};
 int Strides[4] = {W*4, 0, 0, 0};
 sws_scale(S.Convert, Sources, Strides, 0, H, S.Cpu->data, S.Cpu->linesize);
 AVFrame* GPU = av_frame_alloc();
 if (!GPU) return S.Error(AVERROR(ENOMEM), TEXT("allocate GPU frame"));
 Ret = av_hwframe_get_buffer(S.Codec->hw_frames_ctx, GPU, 0);
 // Hardware pools may align the allocation beyond the requested view size.
 // The AVFrame dimensions describe the visible image, not that allocation.
 if (Ret >= 0) { GPU->width = W; GPU->height = H; Ret = av_hwframe_transfer_data(GPU, S.Cpu, 0); }
 if (Ret < 0) { av_frame_free(&GPU); return S.Error(Ret, TEXT("upload Vulkan frame")); }
 GPU->pts = S.NextPts++;
 S.Pending.Add(GPU->pts, Frame);
 Ret = avcodec_send_frame(S.Codec, GPU);
 av_frame_free(&GPU);
 if (Ret < 0) return S.Error(Ret, TEXT("send AV1 frame"));
 AVPacket* Packet = av_packet_alloc();
 if (!Packet) return S.Error(AVERROR(ENOMEM), TEXT("allocate AV1 packet"));
 Ret = avcodec_receive_packet(S.Codec, Packet);
 if (Ret == AVERROR(EAGAIN)) { av_packet_free(&Packet); Frame.Images.Empty(); return true; }
 if (Ret < 0) { av_packet_free(&Packet); return S.Error(Ret, TEXT("receive AV1 packet")); }
 if (const auto* Metadata = S.Pending.Find(Packet->pts)) Frame = *Metadata;
 else { av_packet_free(&Packet); return S.Error(AVERROR(EINVAL), TEXT("AV1 packet has no matching capture metadata")); }
 S.Pending.Remove(Packet->pts);
 if (S.Pending.Num() > 8) { av_packet_free(&Packet); return S.Error(AVERROR(EINVAL), TEXT("encoder exceeded low-latency pending budget")); }
 Frame.Sequence = uint32(Packet->pts);
 Frame.bAv1 = true; Frame.bKeyFrame = (Packet->flags & AV_PKT_FLAG_KEY) != 0; Frame.VideoEpoch = S.Epoch;
 FEncodedCameraImage Image;
 Image.Name = Left.Name; Image.Width = W; Image.Height = H;
 Image.Codec = 3; Image.Flags = Frame.bKeyFrame ? 1 : 0; Image.RawLength = W*H*4;
 // AV1 payload v1: version/layout, epoch LE64, coded dimensions LE16 each, codec-config length LE32,
 // second-eye name length/name, codec config, then one AV1 packet.
 Image.Data.Add(1); Image.Data.Add(Images.Num() == 2 ? 1 : 0);
 Image.Data.Append(reinterpret_cast<const uint8*>(&S.Epoch), 8);
 const uint16 CodedWidth = S.Codec->coded_width, CodedHeight = S.Codec->coded_height;
 Image.Data.Append(reinterpret_cast<const uint8*>(&CodedWidth), 2);
 Image.Data.Append(reinterpret_cast<const uint8*>(&CodedHeight), 2);
 const uint32 ConfigLength = S.Codec->extradata_size;
 Image.Data.Append(reinterpret_cast<const uint8*>(&ConfigLength), 4);
 FTCHARToUTF8 RightName(Images.Num() == 2 ? *Images[1].Name : TEXT(""));
 Image.Data.Add(RightName.Length());
 Image.Data.Append(reinterpret_cast<const uint8*>(RightName.Get()), RightName.Length());
 if (ConfigLength) Image.Data.Append(S.Codec->extradata, ConfigLength);
 Image.Data.Append(Packet->data, Packet->size);
 av_packet_free(&Packet);
 Frame.Images.Add(MoveTemp(Image));
 return true;
}
}

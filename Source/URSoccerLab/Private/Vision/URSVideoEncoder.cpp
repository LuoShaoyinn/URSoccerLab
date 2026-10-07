#include "Vision/URSVideoEncoder.h"
#include "URSVideoCodec.h"
#include <atomic>
#include <cstdio>
#include <exception>
namespace URSoccerLab
{
namespace
{
std::atomic<uint64> NextEpoch{1};
}
struct FVideoEncoder::FImpl
{
    FURSRgbStreamConfig Settings;
    std::unique_ptr<Media::Codec> Codec;
    Media::Backend Backend;
    int32 Width = 0, Height = 0;
    int64 NextPts = 0;
    uint64 Epoch = NextEpoch.fetch_add(1);
    bool Failed = false;
    TMap<int64, FEncodedCameraFrame> Pending;
    explicit FImpl(const FURSRgbStreamConfig &In) : Settings(In)
    {
    }
    bool Error(const char *Message)
    {
        std::fprintf(stderr, "[URS Encoder] stream failed: %s\n", Message);
        Failed = true;
        return false;
    }
    bool Open(int32 W, int32 H)
    {
        Width = W;
        Height = H;
        try
        {
            if (!Settings.EncoderSelection)
                return Error("Missing shared encoder selection");
            Media::Profile Profile{
                W, H, Settings.RateHz, Settings.BitrateKbps * 1000,
                FMath::Max(1, FMath::RoundToInt(Settings.RateHz * Settings.KeyframeIntervalSeconds))};
            Backend = Settings.EncoderSelection->Get(Profile);
            Codec = std::make_unique<Media::Codec>(Backend, Profile);
            return true;
        }
        catch (const std::exception &E)
        {
            return Error(E.what());
        }
    }
};
FVideoEncoder::FVideoEncoder(const FURSRgbStreamConfig &Settings) : Impl(MakeUnique<FImpl>(Settings))
{
}
FVideoEncoder::~FVideoEncoder() = default;
bool FVideoEncoder::Encode(const TArray<FRawCameraImage> &Images, FEncodedCameraFrame &Frame)
{
    auto &S = *Impl;
    if (S.Failed || Images.IsEmpty() || Images.Num() > 2)
        return false;
    const auto &Left = Images[0];
    const int32 W = Left.Width * Images.Num(), H = Left.Height;
    for (const auto &Image : Images)
        if (Image.Width != Left.Width || Image.Height != H || Image.Pixels.Num() != Left.Width * H)
            return false;
    if (!S.Codec && !S.Open(W, H))
        return false;
    if (S.Width != W || S.Height != H)
        return S.Error("Video dimensions changed; recreate stream");
    TArray<FColor> Packed;
    const FColor *Pixels = Left.Pixels.GetData();
    if (Images.Num() == 2)
    {
        Packed.SetNumUninitialized(W * H);
        for (int32 Y = 0; Y < H; ++Y)
            for (int32 Eye = 0; Eye < 2; ++Eye)
                FMemory::Memcpy(Packed.GetData() + Y * W + Eye * Left.Width,
                                Images[Eye].Pixels.GetData() + Y * Left.Width, Left.Width * sizeof(FColor));
        Pixels = Packed.GetData();
    }
    Media::Packet Packet;
    try
    {
        const int64 Pts = S.NextPts++;
        S.Pending.Add(Pts, Frame);
        S.Codec->Send(reinterpret_cast<const uint8 *>(Pixels), Pts);
        if (!S.Codec->Receive(Packet))
        {
            if (S.Pending.Num() > 8)
                return S.Error("Encoder exceeded low-latency pending budget");
            Frame.Images.Empty();
            return true;
        }
    }
    catch (const std::exception &Error)
    {
        return S.Error(Error.what());
    }
    if (const auto *Metadata = S.Pending.Find(Packet.Pts))
        Frame = *Metadata;
    else
    {
        return S.Error("Video packet has no matching capture metadata");
    }
    S.Pending.Remove(Packet.Pts);
    if (S.Pending.Num() > 8)
    {
        return S.Error("Encoder exceeded low-latency pending budget");
    }
    Frame.Sequence = uint32(Packet.Pts);
    Frame.bVideo = true;
    Frame.bKeyFrame = (Packet.Key) != 0;
    Frame.VideoEpoch = S.Epoch;
    FEncodedCameraImage Image;
    Image.Name = Left.Name;
    Image.Width = W;
    Image.Height = H;
    Image.Codec = S.Backend.Codec == "av1" ? 3 : S.Backend.Codec == "h264" ? 4 : 5;
    Image.Flags = Frame.bKeyFrame ? 1 : 0;
    Image.RawLength = W * H * 4;
    // Video payload v1: version/layout, epoch LE64, coded dimensions LE16 each, codec-config length LE32,
    // second-eye name length/name, codec config, then one encoded packet.
    Image.Data.Add(1);
    Image.Data.Add(Images.Num() == 2 ? 1 : 0);
    Image.Data.Append(reinterpret_cast<const uint8 *>(&S.Epoch), 8);
    const uint16 CodedWidth = Packet.Width, CodedHeight = Packet.Height;
    Image.Data.Append(reinterpret_cast<const uint8 *>(&CodedWidth), 2);
    Image.Data.Append(reinterpret_cast<const uint8 *>(&CodedHeight), 2);
    const uint32 ConfigLength = Packet.Config.size();
    Image.Data.Append(reinterpret_cast<const uint8 *>(&ConfigLength), 4);
    FTCHARToUTF8 RightName(Images.Num() == 2 ? *Images[1].Name : TEXT(""));
    Image.Data.Add(RightName.Length());
    Image.Data.Append(reinterpret_cast<const uint8 *>(RightName.Get()), RightName.Length());
    if (ConfigLength)
        Image.Data.Append(Packet.Config.data(), ConfigLength);
    Image.Data.Append(Packet.Bytes.data(), int32(Packet.Bytes.size()));
    Frame.Images.Add(MoveTemp(Image));
    return true;
}
} // namespace URSoccerLab

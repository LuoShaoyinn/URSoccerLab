#pragma once
#include "CoreMinimal.h"
class IImageWrapperModule;
namespace URSoccerLab
{
struct FRawCameraImage
{
	TArray<FColor> Pixels;
	int32 Width = 0, Height = 0;
	FString Name;
};
struct FEncodedCameraImage
{
	FString Name;
	uint16 Width = 0, Height = 0;
	uint8 Codec = 0, PixelFormat = 0, Flags = 0;
	uint32 RawLength = 0;
	TArray<uint8> Data;
};
struct FEncodedCameraFrame
{
	FString ActorId;
	uint32 Sequence = 0;
	double SimTime = 0;
	TArray<FEncodedCameraImage> Images;
	bool bAv1 = false, bKeyFrame = false;
	uint64 VideoEpoch = 0;
	uint8 MessageType = 1;
};
class URSOCCERLAB_API FImageEncoder
{
public:
	static bool Encode(const FRawCameraImage& Image, bool bJpeg, int32 Quality, IImageWrapperModule& Module,
	                   FEncodedCameraImage& Out);
};
} // namespace URSoccerLab

#include "Vision/URSImageEncoder.h"
#include "ImageCore.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
namespace URSoccerLab
{
bool FImageEncoder::Encode(const FRawCameraImage& Image, bool bJpeg, int32 Quality, IImageWrapperModule& Module,
                           FEncodedCameraImage& Out)
{
	Out = FEncodedCameraImage{};
	if (int64(Image.Pixels.Num()) * sizeof(FColor) > MAX_int32)
		return false;
	if (Image.Width <= 0 || Image.Height <= 0 || Image.Width > 65535 || Image.Height > 65535 ||
	    int64(Image.Width) * Image.Height != Image.Pixels.Num())
		return false;
	FTCHARToUTF8 Name(*Image.Name);
	if (Name.Length() > 255)
		return false;
	Out.Name = Image.Name;
	Out.Width = Image.Width;
	Out.Height = Image.Height;
	Out.RawLength = Image.Pixels.Num() * sizeof(FColor);
	if (bJpeg)
	{
		TArray64<uint8> Jpeg;
		FImageView View(Image.Pixels.GetData(), Image.Width, Image.Height);
		if (Module.CompressImage(Jpeg, EImageFormat::JPEG, View, Quality) && Jpeg.Num() > 2 && Jpeg[0] == 0xff &&
		    Jpeg[1] == 0xd8 && Jpeg.Num() <= MAX_int32)
		{
			Out.Codec = 1;
			Out.Data.Append(Jpeg.GetData(), int32(Jpeg.Num()));
			return true;
		}
	}
	Out.Codec = 0;
	Out.Data.Append(reinterpret_cast<const uint8*>(Image.Pixels.GetData()), Out.RawLength);
	return true;
}
} // namespace URSoccerLab

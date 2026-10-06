#include "Scene/URSFieldTextures.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TextureResource.h"

namespace URSoccerLab
{
namespace
{
bool ReadImage(const FString& Path, const FString& Directory, bool Srgb, FImage& Out, FString& Error)
{
	const FString Absolute = FPaths::IsRelative(Path) ? FPaths::Combine(Directory, Path) : Path;
	TArray<uint8> Bytes;
	FImage Decoded;
	if (!FFileHelper::LoadFileToArray(Bytes, *Absolute) || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Decoded))
	{
		Error = FString::Printf(TEXT("cannot decode external texture: %s"), *Absolute);
		return false;
	}
	if (Decoded.NumSlices != 1 || Decoded.SizeX < 1 || Decoded.SizeY < 1 || Decoded.SizeX > 8192 || Decoded.SizeY > 8192)
	{
		Error = FString::Printf(TEXT("texture must be a 2D image at most 8192x8192: %s"), *Absolute);
		return false;
	}
	// Image decoders often tag PNG data as sRGB. Data maps carry numerical values;
	// override the tag before conversion so 128 means 128, not gamma-decoded 55.
	if (!Srgb) Decoded.GammaSpace = EGammaSpace::Linear;
	Out.Init(Decoded.SizeX, Decoded.SizeY, ERawImageFormat::BGRA8, Srgb ? EGammaSpace::sRGB : EGammaSpace::Linear);
	FImageCore::CopyImage(Decoded, Out);
	return true;
}
UTexture2D* MakeTexture(FImage Image, bool Srgb, bool Normal, bool Clamp)
{
	UTexture2D* Texture = UTexture2D::CreateTransient(Image.SizeX, Image.SizeY, PF_B8G8R8A8);
	if (!Texture) return nullptr;
	Texture->SRGB = Srgb;
	Texture->CompressionSettings = Normal ? TC_Normalmap : TC_Default;
	Texture->NeverStream = true;
	Texture->Filter = TF_Trilinear;
	Texture->AddressX = Texture->AddressY = Clamp ? TA_Clamp : TA_Wrap;
	Texture->bNotOfflineProcessed = true;
	auto* Platform = Texture->GetPlatformData();
	Platform->Mips.Empty();
	for (;;)
	{
		auto* Mip = new FTexture2DMipMap();
		Mip->SizeX = Image.SizeX; Mip->SizeY = Image.SizeY; Mip->SizeZ = 1;
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		void* Data = Mip->BulkData.Realloc(Image.RawData.Num());
		FMemory::Memcpy(Data, Image.RawData.GetData(), Image.RawData.Num());
		Mip->BulkData.Unlock();
		Platform->Mips.Add(Mip);
		if (Image.SizeX == 1 && Image.SizeY == 1) break;
		FImage Smaller;
		FImageCore::ResizeImageAllocDest(Image, Smaller, FMath::Max(1, Image.SizeX / 2), FMath::Max(1, Image.SizeY / 2),
			FImageCore::EResizeImageFilter::Box);
		Image = MoveTemp(Smaller);
	}
	Texture->UpdateResource();
	return Texture;
}
FImage Solid(FColor Color)
{
	FImage Image(1, 1, ERawImageFormat::BGRA8, EGammaSpace::Linear);
	Image.AsBGRA8()[0] = Color;
	return Image;
}
}

bool FFieldTextures::Load(const FURSPBRVisualConfig& V, const FString& Directory, FFieldTextures& Out, FString& Error, bool bClampBaseColor)
{
	check(IsInGameThread());
	FImage Base, Normal, Roughness, Metallic, Ao;
	if (!ReadImage(V.BaseColorMap, Directory, true, Base, Error)) return false;
	Normal = Solid(FColor(128, 128, 255));
	Ao = Solid(FColor::White);
	if (!V.NormalMap.IsEmpty() && !ReadImage(V.NormalMap, Directory, false, Normal, Error)) return false;
	if (!V.RoughnessMap.IsEmpty() && !ReadImage(V.RoughnessMap, Directory, false, Roughness, Error)) return false;
	if (!V.MetallicMap.IsEmpty() && !ReadImage(V.MetallicMap, Directory, false, Metallic, Error)) return false;
	if (!V.AoMap.IsEmpty() && !ReadImage(V.AoMap, Directory, false, Ao, Error)) return false;
	if (V.bNormalOpenGL)
		for (FColor& Pixel : Normal.AsBGRA8()) Pixel.G = 255 - Pixel.G;

	// The original glTF shader reads roughness from G and metallic from B.
	// Accept separate external maps, using their red channel, and pack internally.
	const int32 W = FMath::Max(1, FMath::Max(Roughness.SizeX, Metallic.SizeX));
	const int32 H = FMath::Max(1, FMath::Max(Roughness.SizeY, Metallic.SizeY));
	FImage Packed(W, H, ERawImageFormat::BGRA8, EGammaSpace::Linear);
	FImage RoughResized, MetalResized;
	if (!V.RoughnessMap.IsEmpty()) FImageCore::ResizeImageAllocDest(Roughness, RoughResized, W, H);
	if (!V.MetallicMap.IsEmpty()) FImageCore::ResizeImageAllocDest(Metallic, MetalResized, W, H);
	for (int64 I = 0; I < Packed.AsBGRA8().Num(); ++I)
		Packed.AsBGRA8()[I] = FColor(255, V.RoughnessMap.IsEmpty() ? 255 : RoughResized.AsBGRA8()[I].R,
			V.MetallicMap.IsEmpty() ? 255 : MetalResized.AsBGRA8()[I].R, 255);
	Out.BaseColor = MakeTexture(MoveTemp(Base), true, false, bClampBaseColor);
	Out.Normal = MakeTexture(MoveTemp(Normal), false, true, false);
	Out.MetallicRoughness = MakeTexture(MoveTemp(Packed), false, false, false);
	Out.Ao = MakeTexture(MoveTemp(Ao), false, false, false);
	if (!Out.BaseColor || !Out.Normal || !Out.MetallicRoughness || !Out.Ao)
	{
		Error = TEXT("could not allocate runtime textures");
		return false;
	}
	return true;
}
void FFieldTextures::Apply(UMaterialInstanceDynamic* Material, const FURSPBRVisualConfig& V,
    const FLinearColor& DetailTransform) const
{
    check(Material);
    Material->SetTextureParameterValue(TEXT("BaseColorTexture"), BaseColor);
    Material->SetTextureParameterValue(TEXT("NormalTexture"), Normal);
    Material->SetTextureParameterValue(TEXT("MetallicRoughnessTexture"), MetallicRoughness);
    Material->SetTextureParameterValue(TEXT("OcclusionTexture"), Ao);
    Material->SetScalarParameterValue(TEXT("NormalScale"), V.NormalMap.IsEmpty() ? 0 : V.NormalStrength);
    Material->SetScalarParameterValue(TEXT("RoughnessFactor"), V.RoughnessMap.IsEmpty() ? V.Roughness : 1);
    Material->SetScalarParameterValue(TEXT("MetallicFactor"), V.MetallicMap.IsEmpty() ? V.Metallic : 1);
    Material->SetVectorParameterValue(TEXT("BaseColorTexture_OffsetScale"), FLinearColor(0, 0, 1, 1));
    for (const TCHAR* Name : {TEXT("NormalTexture_OffsetScale"), TEXT("MetallicRoughnessTexture_OffsetScale"), TEXT("OcclusionTexture_OffsetScale")})
        Material->SetVectorParameterValue(Name, DetailTransform);
}

}

#if WITH_DEV_AUTOMATION_TESTS
#include "Scene/URSFieldTextures.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

using namespace URSoccerLab;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSFieldTextureTest, "URSoccerLab.Scene.FieldPBR.ExternalTextures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSFieldTextureTest::RunTest(const FString& Parameters)
{
	const FString Directory = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("FieldPBR"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const auto Save = [&](const TCHAR* Name, int32 W, int32 H, FColor Color)
	{
		TArray<FColor> Pixels; Pixels.Init(Color, W * H);
		TArray64<uint8> Bytes;
		FImageUtils::PNGCompressImageArray(W, H, Pixels, Bytes);
		return FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(Directory, Name));
	};
	TestTrue(TEXT("base image written"), Save(TEXT("base.png"), 16, 8, FColor(180, 80, 30)));
	TestTrue(TEXT("normal image written"), Save(TEXT("normal.png"), 4, 4, FColor(128, 160, 250)));
	TestTrue(TEXT("roughness image written"), Save(TEXT("rough.png"), 8, 4, FColor(128, 0, 0)));
	TestTrue(TEXT("metal image written"), Save(TEXT("metal.png"), 4, 8, FColor(64, 0, 0)));
	TestTrue(TEXT("AO image written"), Save(TEXT("ao.png"), 4, 4, FColor(192, 0, 0)));
	FURSFieldVisualConfig Visual;
	Visual.BaseColorMap = TEXT("base.png"); Visual.NormalMap = TEXT("normal.png");
	Visual.RoughnessMap = TEXT("rough.png"); Visual.MetallicMap = TEXT("metal.png"); Visual.AoMap = TEXT("ao.png");
	Visual.bNormalOpenGL = true;
	FFieldTextures Textures;
	FString Error;
	if (TestTrue(TEXT("all external maps load: ") + Error, FFieldTextures::Load(Visual, Directory, Textures, Error)))
	{
		TestTrue(TEXT("base color is sRGB"), Textures.BaseColor->SRGB);
		TestFalse(TEXT("normal is linear"), Textures.Normal->SRGB);
		TestFalse(TEXT("packed data is linear"), Textures.MetallicRoughness->SRGB);
		TestFalse(TEXT("AO is linear"), Textures.Ao->SRGB);
		TestEqual(TEXT("normal sampler classification"), Textures.Normal->CompressionSettings.GetValue(), TC_Normalmap);
		TestEqual(TEXT("full base mip chain"), Textures.BaseColor->GetPlatformData()->Mips.Num(), 5);
		TestEqual(TEXT("packed map matches largest input width"), Textures.MetallicRoughness->GetSizeX(), 8);
		TestEqual(TEXT("packed map matches largest input height"), Textures.MetallicRoughness->GetSizeY(), 8);
		auto& Packed = Textures.MetallicRoughness->GetPlatformData()->Mips[0].BulkData;
		const FColor Pixel = reinterpret_cast<const FColor*>(Packed.LockReadOnly())[0]; Packed.Unlock();
		TestEqual(TEXT("roughness uses R without gamma conversion, stored in G"), Pixel.G, uint8(128));
		TestEqual(TEXT("metallic uses R without gamma conversion, stored in B"), Pixel.B, uint8(64));
		auto& Normal = Textures.Normal->GetPlatformData()->Mips[0].BulkData;
		const FColor NormalPixel = reinterpret_cast<const FColor*>(Normal.LockReadOnly())[0]; Normal.Unlock();
		TestEqual(TEXT("OpenGL normal green flipped"), NormalPixel.G, uint8(95));
		TestEqual(TEXT("normal R is not gamma converted"), NormalPixel.R, uint8(128));
	}
	Visual.NormalMap.Empty(); Visual.RoughnessMap.Empty(); Visual.MetallicMap.Empty(); Visual.AoMap.Empty();
	if (TestTrue(TEXT("base-only config loads"), FFieldTextures::Load(Visual, Directory, Textures, Error)))
	{
		TestEqual(TEXT("optional maps get neutral 1x1 textures"), Textures.Normal->GetSizeX(), 1);
		TestEqual(TEXT("neutral MR has one mip"), Textures.MetallicRoughness->GetPlatformData()->Mips.Num(), 1);
	}
	Visual.NormalMap = TEXT("missing.png");
	TestFalse(TEXT("specified missing optional map fails"), FFieldTextures::Load(Visual, Directory, Textures, Error));
	TestTrue(TEXT("missing map error identifies path"), Error.Contains(TEXT("missing.png")));
	IFileManager::Get().DeleteDirectory(*Directory, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSFieldPBRConfigTest, "URSoccerLab.Scene.FieldPBR.ConfigValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSFieldPBRConfigTest::RunTest(const FString& Parameters)
{
	FURSObjectTypeRegistry::Get().RegisterDefaultTypes(); FURSRobotTypeRegistry::Get().RegisterDefaultTypes();
	auto Config = FURSSceneConfigIo::MakeDefault();
	Config.Field.Visual.NormalMap = TEXT("n.png"); Config.Field.Visual.RoughnessMap = TEXT("r.png");
	Config.Field.Visual.MetallicMap = TEXT("m.png"); Config.Field.Visual.AoMap = TEXT("a.png");
	Config.Field.Visual.bNormalOpenGL = true; Config.Field.Visual.DetailTileSizeM = 0.25;
	Config.Field.Physics.Friction = {0.4f, 0.02f, 0.003f}; Config.Field.Physics.Condim = 6;
	Config.Field.Physics.Solimp = {0.8f, 0.9f, 0.002f, 0.4f, 3.f};
	const FString File = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("FieldPBR"), TEXT(".json"));
	FString Error; FURSSceneConfig Loaded;
	TestTrue(TEXT("PBR config writes"), FURSSceneConfigIo::WriteToFile(File, Config, Error));
	TestTrue(TEXT("PBR config reads"), FURSSceneConfigIo::LoadFromFile(File, Loaded, Error));
	TestTrue(TEXT("PBR config validates"), FURSSceneConfigIo::Validate(Loaded).bOk);
	TestEqual(TEXT("normal path roundtrip"), Loaded.Field.Visual.NormalMap, Config.Field.Visual.NormalMap);
	TestEqual(TEXT("AO path roundtrip"), Loaded.Field.Visual.AoMap, Config.Field.Visual.AoMap);
	TestEqual(TEXT("detail size roundtrip"), Loaded.Field.Visual.DetailTileSizeM, 0.25);
	TestTrue(TEXT("normal convention roundtrip"), Loaded.Field.Visual.bNormalOpenGL);
	TestEqual(TEXT("ground friction roundtrip"), Loaded.Field.Physics.Friction[0], 0.4f);
	TestEqual(TEXT("ground condim roundtrip"), Loaded.Field.Physics.Condim, 6);
	TestEqual(TEXT("ground solimp roundtrip"), Loaded.Field.Physics.Solimp[4], 3.f);
	Loaded.Field.Visual.DetailTileSizeM = 0;
	TestFalse(TEXT("zero detail size rejected"), FURSSceneConfigIo::Validate(Loaded).bOk);
	Loaded = Config; Loaded.Field.Visual.Roughness = 1.1;
	TestFalse(TEXT("roughness outside unit range rejected"), FURSSceneConfigIo::Validate(Loaded).bOk);
	Loaded = Config; Loaded.Field.Physics.Condim = 2;
	TestFalse(TEXT("unsupported contact dimensions rejected"), FURSSceneConfigIo::Validate(Loaded).bOk);
	Loaded = Config; Loaded.Field.Physics.Friction[0] = -1;
	TestFalse(TEXT("negative friction rejected"), FURSSceneConfigIo::Validate(Loaded).bOk);
	Loaded = Config; Loaded.Field.Physics.Solref = {0.02f, -1.f};
	TestFalse(TEXT("mixed solref formats rejected"), FURSSceneConfigIo::Validate(Loaded).bOk);
	Loaded = Config; Loaded.Field.Physics.Solimp[3] = 1.5;
	TestFalse(TEXT("invalid impedance midpoint rejected"), FURSSceneConfigIo::Validate(Loaded).bOk);
	// Test parser types independently from native validation.
	FString Json; FFileHelper::LoadFileToString(Json, *File);
	const auto Reject = [&](const FString& Invalid, const TCHAR* Label)
	{
		FFileHelper::SaveStringToFile(Invalid, *File);
		TestFalse(Label, FURSSceneConfigIo::LoadFromFile(File, Loaded, Error));
	};
	Reject(Json.Replace(TEXT("\"condim\":6"), TEXT("\"condim\":3.5")), TEXT("fractional condim rejected"));
	Reject(Json.Replace(TEXT("\"normal_map\":\"n.png\""), TEXT("\"normal_map\":5")), TEXT("numeric map path rejected"));
	const FString SolrefPrefix = TEXT("\"solref\":[");
	const int32 ValueStart = Json.Find(SolrefPrefix) + SolrefPrefix.Len();
	const int32 ValueEnd = Json.Find(TEXT(","), ESearchCase::CaseSensitive, ESearchDir::FromStart, ValueStart);
	Reject(Json.Left(ValueStart) + TEXT("\"bad\"") + Json.Mid(ValueEnd), TEXT("non-number solver array rejected"));
	IFileManager::Get().Delete(*File);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSBallPBRConfigTest, "URSoccerLab.Scene.BallPBR.ConfigValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSBallPBRConfigTest::RunTest(const FString& Parameters)
{
    FURSObjectTypeRegistry::Get().RegisterDefaultTypes();
    FURSRobotTypeRegistry::Get().RegisterDefaultTypes();
    auto Config = FURSSceneConfigIo::MakeDefault();
    FURSPBRVisualConfig V;
    V.BaseColorMap = TEXT("ball.png"); V.NormalMap = TEXT("ball_normal.png");
    V.AoMap = TEXT("ball_ao.png"); V.RoughnessMap = TEXT("ball_rough.png");
    V.MetallicMap = TEXT("ball_metal.png"); V.bNormalOpenGL = true;
    V.NormalStrength = 0.6; V.Roughness = 0.4; V.Metallic = 0.2;
    Config.Objects[0].Visual = V;
    const FString File = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("BallPBR"), TEXT(".json"));
    FString Error; FURSSceneConfig Loaded;
    TestTrue(TEXT("ball PBR writes"), FURSSceneConfigIo::WriteToFile(File, Config, Error));
    TestTrue(TEXT("ball PBR reads"), FURSSceneConfigIo::LoadFromFile(File, Loaded, Error));
    TestTrue(TEXT("ball PBR validates"), FURSSceneConfigIo::Validate(Loaded).bOk);
    if (TestTrue(TEXT("ball visual survives roundtrip"), Loaded.Objects[0].Visual.IsSet()))
    {
        const auto& Read = Loaded.Objects[0].Visual.GetValue();
        TestEqual(TEXT("base path"), Read.BaseColorMap, V.BaseColorMap);
        TestEqual(TEXT("normal path"), Read.NormalMap, V.NormalMap);
        TestEqual(TEXT("AO path"), Read.AoMap, V.AoMap);
        TestEqual(TEXT("roughness path"), Read.RoughnessMap, V.RoughnessMap);
        TestEqual(TEXT("metallic path"), Read.MetallicMap, V.MetallicMap);
        TestTrue(TEXT("OpenGL convention"), Read.bNormalOpenGL);
        TestEqual(TEXT("normal strength"), Read.NormalStrength, V.NormalStrength);
    }
    Loaded = Config; Loaded.Objects[0].Visual.GetValue().Roughness = -0.1;
    TestFalse(TEXT("invalid ball roughness rejected"), FURSSceneConfigIo::Validate(Loaded).bOk);
    Loaded = Config; Loaded.Objects[0].Visual.GetValue().BaseColorMap.Empty();
    TestFalse(TEXT("ball base map required when visual specified"), FURSSceneConfigIo::Validate(Loaded).bOk);
    FString Json; FFileHelper::LoadFileToString(Json, *File);
    FFileHelper::SaveStringToFile(Json.Replace(TEXT("\"base_color_map\":\"ball.png\""), TEXT("\"base_color_map\":5")), *File);
    TestFalse(TEXT("numeric ball texture path rejected"), FURSSceneConfigIo::LoadFromFile(File, Loaded, Error));
    Config.Objects[0].Visual.Reset();
    TestTrue(TEXT("old ball configs still write"), FURSSceneConfigIo::WriteToFile(File, Config, Error));
    TestTrue(TEXT("old ball configs still read"), FURSSceneConfigIo::LoadFromFile(File, Loaded, Error));
    TestFalse(TEXT("omitted visual remains omitted"), Loaded.Objects[0].Visual.IsSet());
    IFileManager::Get().Delete(*File);
    return true;
}
#endif

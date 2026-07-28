#if WITH_DEV_AUTOMATION_TESTS

#include "Scene/URSRobotTypeRegistry.h"
#include "Scene/URSSceneConfig.h"

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

using namespace URSoccerLab;

namespace
{
FString SceneTempDir()
{
	FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tmp"));
	IFileManager::Get().MakeDirectory(*Dir, true);
	return Dir;
}

FString WriteTempConfig(const FURSSceneConfig& Config)
{
	const FString Path = FPaths::CreateTempFilename(*SceneTempDir(), TEXT("URSScene"), TEXT(".json"));
	FString Error;
	if (!FURSSceneConfigIo::WriteToFile(Path, Config, Error))
	{
		return FString();
	}
	return Path;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSSceneConfigDefaultTest,
	"URSoccerLab.Scene.Config.DefaultLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSSceneConfigDefaultTest::RunTest(const FString& Parameters)
{
	FURSRobotTypeRegistry::Get().RegisterDefaultTypes();

	const FURSSceneConfig Config = FURSSceneConfigIo::MakeDefault();
	TestEqual(TEXT("default has one robot"), Config.Robots.Num(), 1);
	TestEqual(TEXT("default actor_id"), Config.Robots[0].ActorId, FString(TEXT("robot_rp0")));
	TestEqual(TEXT("default type"), Config.Robots[0].Type, FString(TEXT("pi_plus")));
	TestTrue(TEXT("default translation set"), Config.Robots[0].TranslationMeters.IsSet());
	TestEqual(TEXT("default translation Z"), Config.Robots[0].TranslationMeters.GetValue().Z, 0.3762);
	TestTrue(TEXT("default rotation set"), Config.Robots[0].RotationQuatXyzw.IsSet());

	const FURSSceneConfigValidationResult Validation = FURSSceneConfigIo::Validate(Config);
	TestTrue(TEXT("default config valid"), Validation.bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSSceneConfigRoundTripTest,
	"URSoccerLab.Scene.Config.JsonRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSSceneConfigRoundTripTest::RunTest(const FString& Parameters)
{
	FURSRobotTypeRegistry::Get().RegisterDefaultTypes();

	const FURSSceneConfig Original = FURSSceneConfigIo::MakeDefault();
	const FString Path = WriteTempConfig(Original);
	TestTrue(TEXT("temp config written"), !Path.IsEmpty());

	FURSSceneConfig Loaded;
	FString Error;
	TestTrue(TEXT("load temp config"), FURSSceneConfigIo::LoadFromFile(Path, Loaded, Error));
	TestTrue(TEXT("load error empty"), Error.IsEmpty());

	TestEqual(TEXT("round-trip robot count"), Loaded.Robots.Num(), Original.Robots.Num());
	TestEqual(TEXT("round-trip actor_id"), Loaded.Robots[0].ActorId, Original.Robots[0].ActorId);
	TestEqual(TEXT("round-trip type"), Loaded.Robots[0].Type, Original.Robots[0].Type);
	TestTrue(TEXT("round-trip translation present"), Loaded.Robots[0].TranslationMeters.IsSet());
	TestEqual(TEXT("round-trip translation Z"), Loaded.Robots[0].TranslationMeters.GetValue().Z, 0.3762);
	TestTrue(TEXT("round-trip rotation present"), Loaded.Robots[0].RotationQuatXyzw.IsSet());

	IFileManager::Get().Delete(*Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSSceneConfigLoadRejectionTest,
	"URSoccerLab.Scene.Config.LoadRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSSceneConfigLoadRejectionTest::RunTest(const FString& Parameters)
{
	const FString TempDir = SceneTempDir();
	FString Error;
	FURSSceneConfig Out;

	auto WriteAndLoad = [&TempDir](const FString& JsonBody, FURSSceneConfig& OutCfg, FString& OutError) -> bool
	{
		const FString Path = FPaths::CreateTempFilename(*TempDir, TEXT("URSSceneReject"), TEXT(".json"));
		FFileHelper::SaveStringToFile(JsonBody, *Path);
		const bool bOk = FURSSceneConfigIo::LoadFromFile(Path, OutCfg, OutError);
		IFileManager::Get().Delete(*Path);
		return bOk;
	};

	TestFalse(TEXT("missing version rejected"),
		WriteAndLoad(TEXT("{\"robots\":[]}"), Out, Error));

	TestFalse(TEXT("unknown version rejected"),
		WriteAndLoad(TEXT("{\"version\":\"urs_scene_v999\",\"robots\":[]}"), Out, Error));

	TestFalse(TEXT("missing actor_id rejected"),
		WriteAndLoad(TEXT("{\"version\":\"urs_scene_v1\",\"robots\":[{\"type\":\"pi_plus\"}]}"), Out, Error));

	TestFalse(TEXT("empty actor_id rejected"),
		WriteAndLoad(TEXT("{\"version\":\"urs_scene_v1\",\"robots\":[{\"actor_id\":\"\",\"type\":\"pi_plus\"}]}"), Out, Error));

	TestFalse(TEXT("missing type rejected"),
		WriteAndLoad(TEXT("{\"version\":\"urs_scene_v1\",\"robots\":[{\"actor_id\":\"robot_rp0\"}]}"), Out, Error));

	TestFalse(TEXT("non-finite translation rejected"),
		WriteAndLoad(TEXT("{\"version\":\"urs_scene_v1\",\"robots\":[{\"actor_id\":\"r\",\"type\":\"pi_plus\",\"translation_m\":[1e999,0,0]}]}"), Out, Error));

	TestFalse(TEXT("missing file rejected"),
		FURSSceneConfigIo::LoadFromFile(FPaths::CreateTempFilename(*TempDir, TEXT("Nope"), TEXT(".json")), Out, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSSceneConfigValidateTest,
	"URSoccerLab.Scene.Config.ValidateFlagsErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSSceneConfigValidateTest::RunTest(const FString& Parameters)
{
	FURSRobotTypeRegistry::Get().RegisterDefaultTypes();

	FURSSceneConfig Config;
	FURSRobotSpawn& A = Config.Robots.	AddDefaulted_GetRef();
	A.ActorId = TEXT("robot_rp0");
	A.Type = TEXT("pi_plus");

	FURSRobotSpawn& B = Config.Robots.	AddDefaulted_GetRef();
	B.ActorId = TEXT("robot_rp0");
	B.Type = TEXT("pi_plus");

	FURSSceneConfigValidationResult Result = FURSSceneConfigIo::Validate(Config);
	TestFalse(TEXT("duplicate actor_id invalid"), Result.bOk);
	TestTrue(TEXT("duplicate actor_id reports error"),
		Result.Errors.ContainsByPredicate([](const FString& E) { return E.Contains(TEXT("duplicate")); }));

	Config.Robots.Reset();
	FURSRobotSpawn& UnknownType = Config.Robots.	AddDefaulted_GetRef();
	UnknownType.ActorId = TEXT("robot_rp0");
	UnknownType.Type = TEXT("does_not_exist");
	Result = FURSSceneConfigIo::Validate(Config);
	TestFalse(TEXT("unknown type invalid"), Result.bOk);
	TestTrue(TEXT("unknown type reports error"),
		Result.Errors.ContainsByPredicate([](const FString& E) { return E.Contains(TEXT("unknown type")); }));

	Config.Robots.Reset();
	FURSRobotSpawn& EmptyId = Config.Robots.	AddDefaulted_GetRef();
	EmptyId.Type = TEXT("pi_plus");
	Result = FURSSceneConfigIo::Validate(Config);
	TestFalse(TEXT("empty actor_id invalid"), Result.bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSRobotTypeRegistryTest,
	"URSoccerLab.Scene.Registry.DefaultTypesRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSRobotTypeRegistryTest::RunTest(const FString& Parameters)
{
	FURSRobotTypeRegistry& Reg = FURSRobotTypeRegistry::Get();
	Reg.RegisterDefaultTypes();

	const FURSRobotType* PiPlus = Reg.Find(TEXT("pi_plus"));
	TestNotNull(TEXT("pi_plus registered"), PiPlus);
	TestTrue(TEXT("pi_plus blueprint path set"), !PiPlus->BlueprintAssetPath.IsEmpty());
	TestEqual(TEXT("pi_plus default base height"), PiPlus->DefaultBaseHeightM, 0.3762);
	TestEqual(TEXT("pi_plus standing pose defines every hinge"), PiPlus->DefaultJointPositions.Num(), 22);
	const float* LeftCalf = PiPlus->DefaultJointPositions.Find(TEXT("l_calf_joint"));
	TestNotNull(TEXT("pi_plus standing pose has left calf"), LeftCalf);
	if (LeftCalf)
	{
		TestEqual(TEXT("pi_plus standing left calf angle"), *LeftCalf, 0.65f);
	}

	TestNull(TEXT("unknown type returns null"), Reg.Find(TEXT("nope")));

	Reg.RegisterDefaultTypes();
	const FURSRobotType* PiPlusAgain = Reg.Find(TEXT("pi_plus"));
	TestNotNull(TEXT("pi_plus still registered after re-register"), PiPlusAgain);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

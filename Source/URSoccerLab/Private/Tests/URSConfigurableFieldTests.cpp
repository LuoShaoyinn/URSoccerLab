#if WITH_DEV_AUTOMATION_TESTS
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "Scene/URSSceneConfigComponent.h"

using namespace URSoccerLab;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSRequiredFieldTest, "URSoccerLab.Scene.Config.RequiredField",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSRequiredFieldTest::RunTest(const FString &Parameters)
{
	FURSObjectTypeRegistry::Get().RegisterDefaultTypes();
	FURSRobotTypeRegistry::Get().RegisterDefaultTypes();
	const FString Path = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("URSRequired"), TEXT(".json"));
	FString Error;
	FURSSceneConfig Loaded;
	const auto Load = [&](const FString &Json)
	{
		FFileHelper::SaveStringToFile(Json, *Path);
		return FURSSceneConfigIo::LoadFromFile(Path, Loaded, Error);
	};
	TestFalse(TEXT("missing field rejected"), Load(TEXT(R"({"version":"urs_scene_v1","robots":[]})")));
	TestFalse(TEXT("missing dimensions rejected"),
			  Load(TEXT(R"({"version":"urs_scene_v1","field":{"map_image":"map.png"},"robots":[]})")));
	TestFalse(
		TEXT("string dimensions rejected"),
		Load(TEXT(
			R"({"version":"urs_scene_v1","field":{"length_m":"9","width_m":6,"map_image":"map.png"},"robots":[]})")));
	TestFalse(TEXT("wrong image type rejected"),
			  Load(TEXT(R"({"version":"urs_scene_v1","field":{"length_m":9,"width_m":6,"map_image":5},"robots":[]})")));
	TestTrue(
		TEXT("valid explicit field loads"),
		Load(TEXT(
			R"({"version":"urs_scene_v1","field":{"length_m":7,"width_m":4,"map_image":"map.png"},"robots":[]})")));
	TestTrue(TEXT("image resolved relative to JSON directory"),
			 Loaded.SourceDirectory == FPaths::GetPath(FPaths::ConvertRelativePathToFull(Path)));
	TestTrue(TEXT("valid config accepted"), FURSSceneConfigIo::Validate(Loaded).bOk);
	Loaded.Field.MapImage.Empty();
	TestFalse(TEXT("empty image rejected"), FURSSceneConfigIo::Validate(Loaded).bOk);
	auto Config = FURSSceneConfigIo::MakeDefault();
	Config.Field.LengthM = -1;
	TestFalse(TEXT("negative dimensions rejected"), FURSSceneConfigIo::Validate(Config).bOk);
	Config = FURSSceneConfigIo::MakeDefault();
	Config.Objects[0].Physics.bIsSet = true;
	Config.Objects[0].Physics.RadiusM = 0;
	TestFalse(TEXT("zero radius rejected"), FURSSceneConfigIo::Validate(Config).bOk);
	Config.Objects[0].Physics.RadiusM = 0.11;
	Config.Objects[0].Physics.MassKg = 0.43;
	Config.Objects[0].Physics.Friction = {0.6f, 0.005f, 0.001f};
	Config.Objects[0].Physics.Solref = {0.02f, 0.7f};
	TestTrue(TEXT("new settings serialize"), FURSSceneConfigIo::WriteToFile(Path, Config, Error));
	TestTrue(TEXT("new settings deserialize"), FURSSceneConfigIo::LoadFromFile(Path, Loaded, Error));
	TestEqual(TEXT("radius roundtrip"), Loaded.Objects[0].Physics.RadiusM, 0.11);
	TestEqual(TEXT("field width roundtrip"), Loaded.Field.WidthM, Config.Field.WidthM);
	TestEqual(TEXT("friction roundtrip"), Loaded.Objects[0].Physics.Friction[2], 0.001f);
	Config.Objects[0].Physics.Solref = {0.02f, -0.7f};
	TestFalse(TEXT("mixed solref formats rejected"), FURSSceneConfigIo::Validate(Config).bOk);
	IFileManager::Get().Delete(*Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSRuntimeFieldBallTest, "URSoccerLab.Scene.Config.RuntimeFieldBall",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSRuntimeFieldBallTest::RunTest(const FString &Parameters)
{
	FURSObjectTypeRegistry::Get().RegisterDefaultTypes();
	FURSRobotTypeRegistry::Get().RegisterDefaultTypes();
	UWorld *World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	AAMjManager *Manager = World->SpawnActor<AAMjManager>();
	UURSSceneConfigComponent *Scene = NewObject<UURSSceneConfigComponent>(Manager);
	Manager->AddInstanceComponent(Scene);
	Scene->RegisterComponent();
	auto Config = FURSSceneConfigIo::MakeDefault();
	Config.Robots.Reset();
	Config.Field.LengthM = 7;
	Config.Field.WidthM = 4;
	Config.Field.BorderXM = 0.5;
	Config.Field.BorderYM = 0.5;
	Config.Field.MapImage = TEXT("Assets/FieldMaps/example.png");
	Config.SourceDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Config.Objects[0].TranslationMeters.Reset();
	auto &P = Config.Objects[0].Physics;
	P.bIsSet = true;
	P.RadiusM = 0.11;
	P.MassKg = 0.43;
	P.Friction = {0.6f, 0.005f, 0.001f};
	P.Solref = {0.02f, 0.7f};
	FString Error;
	if (TestTrue(TEXT("runtime config applies: ") + Error, Scene->ApplyConfig(Config, Error)))
	{
		TArray<UStaticMeshComponent *> Meshes;
		Manager->GetComponents(Meshes);
		bool Found = false;
		for (auto *Mesh : Meshes)
			if (Mesh->GetName() == TEXT("URSRuntimeField"))
			{
				Found = true;
				TestTrue(TEXT("field bounds are 8 x 5 metres"),
						 Mesh->Bounds.BoxExtent.Equals(FVector(400, 250, 0), 0.1));
			}
		TestTrue(TEXT("runtime surface exists"), Found);
		for (TActorIterator<AMjArticulation> It(World); It; ++It)
			TestTrue(TEXT("default ball center follows radius"), FMath::IsNearlyEqual(It->GetActorLocation().Z, 11.0));
		Manager->Compile();
		mjModel *Model = Manager->PhysicsEngine->m_model;
		if (TestNotNull(TEXT("MuJoCo compiles configured ball"), Model))
		{
			TestEqual(TEXT("one sphere in compiled model"), Model->ngeom, 1);
			TestTrue(TEXT("compiled radius"), FMath::IsNearlyEqual(Model->geom_size[0], 0.11, 1e-6));
			const int Body = Model->geom_bodyid[0];
			TestTrue(TEXT("compiled mass"), FMath::IsNearlyEqual(Model->body_mass[Body], 0.43, 1e-6));
			TestTrue(TEXT("inertia recomputed"),
					 FMath::IsNearlyEqual(Model->body_inertia[3 * Body], 0.4 * 0.43 * 0.11 * 0.11, 1e-6));
			TestTrue(TEXT("compiled friction"), FMath::IsNearlyEqual(Model->geom_friction[2], 0.001, 1e-6));
			TestTrue(TEXT("compiled contact damping"), FMath::IsNearlyEqual(Model->geom_solref[1], 0.7, 1e-6));
		}
		Config.Field.MapImage = TEXT("does-not-exist.png");
		TestFalse(TEXT("missing external image fails"), Scene->ApplyConfig(Config, Error));
	}
	Manager->PhysicsEngine->bShouldStopTask = true;
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}
#endif

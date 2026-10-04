#if WITH_DEV_AUTOMATION_TESTS
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
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
			R"({"version":"urs_scene_v1","field":{"length_m":7,"width_m":4,"map_image":"map.png"},"goals":{"width_m":1.8,"height_m":1.2,"post_radius_m":0.05,"poses":[{"translation_m":[-4.5,0,0],"yaw_deg":0},{"translation_m":[4.5,0,0],"yaw_deg":180}]},"robots":[]})")));
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
	Config.Goals.Poses = {{FVector(-2, 1, 0), 90}, {FVector(2, -1, 0.2), 180}};
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
				TestEqual(TEXT("field retains static mobility"), Mesh->Mobility, EComponentMobility::Static);
				TestTrue(TEXT("field retains Nanite geometry"), Mesh->GetStaticMesh()->GetNaniteSettings().bEnabled);
				TestTrue(TEXT("field bounds are 8 x 5 metres"),
						 Mesh->Bounds.BoxExtent.Equals(FVector(400, 250, 0), 0.1));
			}
		TestTrue(TEXT("runtime surface exists"), Found);
		Config.Field.LengthM = 10;
		Config.Field.WidthM = 6;
		if (TestTrue(TEXT("static field can be resized on config reload"), Scene->ApplyConfig(Config, Error)))
		{
			Meshes.Reset();
			Manager->GetComponents(Meshes);
			int32 SurfaceCount = 0;
			for (auto *Mesh : Meshes)
				if (Mesh->GetName() == TEXT("URSRuntimeField"))
				{
					++SurfaceCount;
					TestTrue(TEXT("reloaded field bounds are 11 x 7 metres"),
						Mesh->Bounds.BoxExtent.Equals(FVector(550, 350, 0), 0.1));
					TestTrue(TEXT("reloaded static surface is registered"), Mesh->IsRegistered());
				}
			TestEqual(TEXT("reload reuses one field surface"), SurfaceCount, 1);
		}
		for (TActorIterator<AMjArticulation> It(World); It; ++It)
			if (It->ActorId == TEXT("ball"))
				TestTrue(TEXT("default ball center follows radius"),
						 FMath::IsNearlyEqual(It->GetActorLocation().Z, 11.0));
		Manager->Compile();
		mjModel *Model = Manager->PhysicsEngine->m_model;
		if (TestNotNull(TEXT("MuJoCo compiles configured ball"), Model))
		{
			TestEqual(TEXT("one sphere and six goal cylinders in compiled model"), Model->ngeom, 7);
			int BallGeom = -1;
			int FirstPost = -1;
			int FirstBar = -1;
			int CylinderCount = 0;
			for (int I = 0; I < Model->ngeom; ++I)
			{
				if (Model->geom_type[I] == mjGEOM_SPHERE)
					BallGeom = I;
				if (Model->geom_type[I] == mjGEOM_CYLINDER)
				{
					++CylinderCount;
					TestEqual(TEXT("goal cylinders are static"), Model->geom_bodyid[I], 0);
					TestTrue(TEXT("goal post radius"), FMath::IsNearlyEqual(Model->geom_size[3 * I], 0.05, 1e-6));
					const FString Name = UTF8_TO_TCHAR(mj_id2name(Model, mjOBJ_GEOM, I));
					if (Name.EndsWith(TEXT("goal_0_left_post")))
						FirstPost = I;
					if (Name.EndsWith(TEXT("goal_0_crossbar")))
						FirstBar = I;
				}
			}
			TestEqual(TEXT("exactly six goal cylinders"), CylinderCount, 6);
			if (!TestTrue(TEXT("compiled sphere exists"), BallGeom >= 0))
			{
				Manager->PhysicsEngine->bShouldStopTask = true;
				World->DestroyWorld(false);
				GEngine->DestroyWorldContext(World);
				return false;
			}
			if (TestTrue(TEXT("first rotated post exists"), FirstPost >= 0))
			{
				TestTrue(TEXT("goal yaw and translation reach physics"),
						 FVector(Model->geom_pos[3 * FirstPost], Model->geom_pos[3 * FirstPost + 1],
								 Model->geom_pos[3 * FirstPost + 2])
							 .Equals(FVector(-2.95, 1, 0.65), 1e-5));
				TestTrue(TEXT("post half length"),
						 FMath::IsNearlyEqual(Model->geom_size[3 * FirstPost + 1], 0.65, 1e-6));
				auto *Data = Manager->PhysicsEngine->m_data;
				Data->qpos[0] = -2.80;
				Data->qpos[1] = 1;
				Data->qpos[2] = 0.65;
				mj_forward(Model, Data);
				bool Contact = false;
				for (int C = 0; C < Data->ncon; ++C)
					Contact |= (Data->contact[C].geom[0] == BallGeom && Data->contact[C].geom[1] == FirstPost) ||
							   (Data->contact[C].geom[1] == BallGeom && Data->contact[C].geom[0] == FirstPost);
				TestTrue(TEXT("ball contacts goalpost"), Contact);
			}
			if (TestTrue(TEXT("crossbar exists"), FirstBar >= 0))
			{
				TestTrue(TEXT("crossbar clearance height"),
						 FMath::IsNearlyEqual(Model->geom_pos[3 * FirstBar + 2], 1.25, 1e-6));
				TestTrue(TEXT("crossbar half length"),
						 FMath::IsNearlyEqual(Model->geom_size[3 * FirstBar + 1], 1.0, 1e-6));
				mjtNum Axis[3], Z[3] = {0, 0, 1};
				mju_rotVecQuat(Axis, Z, Model->geom_quat + 4 * FirstBar);
				TestTrue(TEXT("crossbar follows rotated goal opening"),
						 FMath::IsNearlyEqual(FMath::Abs(Axis[0]), 1.0, 1e-6));
			}
			TestTrue(TEXT("compiled radius"), FMath::IsNearlyEqual(Model->geom_size[3 * BallGeom], 0.11, 1e-6));
			const int Body = Model->geom_bodyid[BallGeom];
			TestTrue(TEXT("compiled mass"), FMath::IsNearlyEqual(Model->body_mass[Body], 0.43, 1e-6));
			TestTrue(TEXT("inertia recomputed"),
					 FMath::IsNearlyEqual(Model->body_inertia[3 * Body], 0.4 * 0.43 * 0.11 * 0.11, 1e-6));
			TestTrue(TEXT("compiled friction"),
					 FMath::IsNearlyEqual(Model->geom_friction[3 * BallGeom + 2], 0.001, 1e-6));
			TestTrue(TEXT("compiled contact damping"),
					 FMath::IsNearlyEqual(Model->geom_solref[2 * BallGeom + 1], 0.7, 1e-6));
		}
		Config.Field.MapImage = TEXT("does-not-exist.png");
		TestFalse(TEXT("missing external image fails"), Scene->ApplyConfig(Config, Error));
	}
	Manager->PhysicsEngine->bShouldStopTask = true;
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSRequiredGoalsTest, "URSoccerLab.Scene.Config.RequiredGoals",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSRequiredGoalsTest::RunTest(const FString &Parameters)
{
	FURSObjectTypeRegistry::Get().RegisterDefaultTypes();
	FURSRobotTypeRegistry::Get().RegisterDefaultTypes();
	const FString Path = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("URSGoals"), TEXT(".json"));
	FURSSceneConfig Loaded;
	FString Error;
	const auto Load = [&](const FString &Goals)
	{
		const FString Json =
			TEXT(
				R"({"version":"urs_scene_v1","field":{"length_m":9,"width_m":6,"map_image":"field.png"},"robots":[])") +
			(Goals.IsEmpty() ? FString() : TEXT(",\"goals\":") + Goals) + TEXT("}");
		FFileHelper::SaveStringToFile(Json, *Path);
		return FURSSceneConfigIo::LoadFromFile(Path, Loaded, Error);
	};
	TestFalse(TEXT("missing goals rejected"), Load(TEXT("")));
	TestFalse(
		TEXT("one goal rejected"),
		Load(TEXT(
			R"({"width_m":1.8,"height_m":1.2,"post_radius_m":0.05,"poses":[{"translation_m":[-4.5,0,0],"yaw_deg":0}]})")));
	TestFalse(
		TEXT("missing dimensions rejected"),
		Load(
			TEXT(R"({"poses":[{"translation_m":[-4.5,0,0],"yaw_deg":0},{"translation_m":[4.5,0,0],"yaw_deg":180}]})")));
	TestFalse(
		TEXT("missing yaw rejected"),
		Load(TEXT(
			R"({"width_m":1.8,"height_m":1.2,"post_radius_m":0.05,"poses":[{"translation_m":[-4.5,0,0]},{"translation_m":[4.5,0,0],"yaw_deg":180}]})")));
	TestFalse(
		TEXT("string coordinate rejected"),
		Load(TEXT(
			R"({"width_m":1.8,"height_m":1.2,"post_radius_m":0.05,"poses":[{"translation_m":["-4.5",0,0],"yaw_deg":0},{"translation_m":[4.5,0,0],"yaw_deg":180}]})")));
	TestTrue(
		TEXT("two explicit goals accepted"),
		Load(TEXT(
			R"({"width_m":2,"height_m":1,"post_radius_m":0.04,"poses":[{"translation_m":[-2,1,0],"yaw_deg":90},{"translation_m":[2,-1,0.2],"yaw_deg":180}]})")));
	TestTrue(TEXT("valid goals validate"), FURSSceneConfigIo::Validate(Loaded).bOk);
	TestEqual(TEXT("yaw parsed"), Loaded.Goals.Poses[0].YawDeg, 90.0);
	TestTrue(TEXT("goals serialize"), FURSSceneConfigIo::WriteToFile(Path, Loaded, Error));
	FURSSceneConfig Roundtrip;
	TestTrue(TEXT("goals reload"), FURSSceneConfigIo::LoadFromFile(Path, Roundtrip, Error));
	TestEqual(TEXT("width roundtrip"), Roundtrip.Goals.WidthM, 2.0);
	TestEqual(TEXT("pose roundtrip"), Roundtrip.Goals.Poses[1].TranslationMeters.Z, 0.2);
	Loaded.Goals.PostRadiusM = 0;
	TestFalse(TEXT("zero radius rejected"), FURSSceneConfigIo::Validate(Loaded).bOk);
	Loaded.Goals.PostRadiusM = 0.04;
	Loaded.Goals.Poses.RemoveAt(1);
	TestFalse(TEXT("exactly two poses required by native validation"), FURSSceneConfigIo::Validate(Loaded).bOk);
	IFileManager::Get().Delete(*Path);
	return true;
}

#endif

#include "Scene/URSSceneConfigComponent.h"
#include "Scene/URSFieldTextures.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeParser.h"
#include "MuJoCo/Components/Geometry/Primitives/MjPlane.h"
#include "Vision/URSCameraStreamComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "SceneUtils.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "MuJoCo/Components/Geometry/Primitives/MjSphere.h"
#include "MuJoCo/Components/Geometry/Primitives/MjCylinder.h"
#include "MuJoCo/Components/Bodies/MjWorldBody.h"
#include "Misc/Paths.h"

#include "MuJoCo/Components/Geometry/MjGeom.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "MuJoCo/Core/AMjManager.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "MuJoCo/Utils/MjUtils.h"
#include "Transport/NetworkManager.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Scene/URSObjectTypeRegistry.h"
#include "Transport/URSTcpTransportComponent.h"

using namespace URSoccerLab;

UURSSceneConfigComponent::UURSSceneConfigComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(
		TEXT("/Game/URSoccerLab/Scenes/SoccerField/Runtime/SM_RuntimeField"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(
		TEXT("/Game/URSoccerLab/Scenes/SoccerField/Runtime/MI_RuntimeField"));
	RuntimeFieldMesh = Plane.Object;
	RuntimeFieldMaterial = Material.Object;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> White(TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
	GoalCylinderMesh = Cylinder.Object;
	GoalMaterial = White.Object;
}

void UURSSceneConfigComponent::BeginPlay()
{
	Super::BeginPlay();
	// Robot spawning is owned by AURSSoccerGameMode::InitGame, which runs
	// BEFORE BeginPlay to guarantee robots are in the compiled MuJoCo model.
}

bool UURSSceneConfigComponent::ReloadConfig(FString& OutError)
{
	const FString ConfigFilePath = FPaths::IsRelative(ConfigPath)
		? FPaths::Combine(FPaths::ProjectDir(), ConfigPath)
		: ConfigPath;
	const FString AbsPath = FPaths::ConvertRelativePathToFull(ConfigFilePath);
	if (!FURSSceneConfigIo::LoadFromFile(AbsPath, ActiveConfig, OutError))
	{
		return false;
	}
	const FURSSceneConfigValidationResult Validation = FURSSceneConfigIo::Validate(ActiveConfig);
	if (!Validation.bOk)
	{
		OutError = Validation.Errors.Num() > 0 ? Validation.Errors[0] : TEXT("scene config invalid");
		return false;
	}
	return true;
}

bool UURSSceneConfigComponent::ApplyConfig(FString& OutError)
{
	if (!ReloadConfig(OutError))
	{
		return false;
	}
	return ApplyConfig(ActiveConfig, OutError);
}

bool UURSSceneConfigComponent::ApplyConfig(const URSoccerLab::FURSSceneConfig& Config, FString& OutError)
{
	const URSoccerLab::FURSSceneConfigValidationResult Validation = URSoccerLab::FURSSceneConfigIo::Validate(Config);
	if (!Validation.bOk)
	{
		OutError = Validation.Errors.Num() > 0 ? Validation.Errors[0] : TEXT("scene config invalid");
		return false;
	}

    // Validate all packages before changing the active scene.
    TMap<FString,TSharedPtr<FExternalRobotPackage>> Packages;
    for (const auto& Spawn:Config.Robots)
    {
        if (Packages.Contains(Spawn.Type)) continue;
        const FString& Path=Config.RobotTypes.FindChecked(Spawn.Type);
        FString Absolute=FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path)?FPaths::Combine(Config.SourceDirectory,Path):Path);
        auto Package=FExternalRobotLoader::Load(Absolute,OutError);
        if (!Package) return false;
        if (Package->Id!=Spawn.Type) { OutError=TEXT("robot manifest id must match scene type: ")+Spawn.Type; return false; }
        for (const auto& Warning:Package->Warnings) UE_LOG(LogTemp,Warning,TEXT("Robot package %s: %s"),*Spawn.Type,*Warning);
        Packages.Add(Spawn.Type,Package);
    }
    RobotPackages=MoveTemp(Packages);
	ActiveConfig = Config;
	if (!ApplyFieldConfig(OutError))
		return false;

	AActor* Owner = GetOwner();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	AAMjManager* Manager = Cast<AAMjManager>(Owner);
	if (!Manager || !World)
	{
		OutError = TEXT("UURSSceneConfigComponent must be owned by an AAMjManager");
		return false;
	}

	// Camera pixels are delivered by URSoccerLab's consolidated, versioned
	// TCP transport. Keep URLab camera rendering/readback enabled, but prevent
	// its NetworkManager from starting one raw ZMQ socket and SHM mapping per
	// camera when the cameras register during BeginPlay.
	if (Manager->NetworkManager)
	{
		Manager->NetworkManager->bEnableCameraBroadcast = false;
	}

	if (!ApplyFieldPhysicsConfig(OutError))
		return false;
	if (!ApplyGoalsConfig(OutError))
		return false;
	DestroyConfiguredArticulations();

	TSet<FString> NewActorIds;
	NewActorIds.Reserve(ActiveConfig.Robots.Num() + ActiveConfig.Objects.Num());
	for (const URSoccerLab::FURSRobotSpawn& Spawn : ActiveConfig.Robots)
	{
		NewActorIds.Add(Spawn.ActorId);
	}
	for (const URSoccerLab::FURSObjectSpawn& Spawn : ActiveConfig.Objects)
	{
		NewActorIds.Add(Spawn.ActorId);
	}

	// Destroy any actor we previously spawned whose id was removed from the
	// current config. This is what makes ApplyConfig idempotent across
	// reloads even when ids disappear from the file.
	{
		TSet<FString> Stale = KnownActorIds.Difference(NewActorIds);
		if (Stale.Num() > 0)
		{
			DestroyActorsWithIds(Stale);
			for (const FString& Id : Stale)
			{
				KnownActorIds.Remove(Id);
				SpawnedRobots.Remove(Id);
				SpawnedObjects.Remove(Id);
			}
		}
	}

	TArray<FString> SpawnedInThisCall;
	for (const URSoccerLab::FURSRobotSpawn& Spawn : ActiveConfig.Robots)
	{
		if (!SpawnOneRobot(Manager, Spawn, OutError))
		{
			// Rollback: destroy everything we spawned in this call so the
			// scene is not left in a partial state.
			UE_LOG(LogTemp, Error, TEXT("URSoccerLab scene config: spawn failed for '%s', rolling back %d robot(s)."),
				*Spawn.ActorId, SpawnedInThisCall.Num());
			DestroyActorsWithIds(TSet<FString>(SpawnedInThisCall));
			for (const FString& Id : SpawnedInThisCall)
			{
				KnownActorIds.Remove(Id);
				SpawnedRobots.Remove(Id);
				SpawnedObjects.Remove(Id);
			}
			return false;
		}
		KnownActorIds.Add(Spawn.ActorId);
		SpawnedInThisCall.Add(Spawn.ActorId);
	}
	for (const URSoccerLab::FURSObjectSpawn& Spawn : ActiveConfig.Objects)
	{
		if (!SpawnOneObject(Manager, Spawn, OutError))
		{
			UE_LOG(LogTemp, Error,
				TEXT("URSoccerLab scene config: object spawn failed for '%s', rolling back %d articulation(s)."),
				*Spawn.ActorId, SpawnedInThisCall.Num());
			DestroyActorsWithIds(TSet<FString>(SpawnedInThisCall));
			for (const FString& Id : SpawnedInThisCall)
			{
				KnownActorIds.Remove(Id);
				SpawnedRobots.Remove(Id);
				SpawnedObjects.Remove(Id);
			}
			return false;
		}
		KnownActorIds.Add(Spawn.ActorId);
		SpawnedInThisCall.Add(Spawn.ActorId);
	}

	UE_LOG(LogTemp, Log, TEXT("URSoccerLab scene config applied: %d robot(s), %d object(s)."),
		SpawnedRobots.Num(), SpawnedObjects.Num());
	ApplyRenderConfig();
	ApplyLightingConfig();
	ApplyPhysicsConfig();
	OnSceneConfigApplied.Broadcast();
	return true;
}

void UURSSceneConfigComponent::DestroyConfiguredArticulations()
{
	TSet<FString> IdsToDestroy;
	for (const URSoccerLab::FURSRobotSpawn& Spawn : ActiveConfig.Robots)
	{
		IdsToDestroy.Add(Spawn.ActorId);
	}
	for (const URSoccerLab::FURSObjectSpawn& Spawn : ActiveConfig.Objects)
	{
		IdsToDestroy.Add(Spawn.ActorId);
	}
	if (IdsToDestroy.Num() == 0)
	{
		return;
	}
	DestroyActorsWithIds(IdsToDestroy);
}

bool UURSSceneConfigComponent::SpawnOneObject(
	AAMjManager* Manager,
	const URSoccerLab::FURSObjectSpawn& Spawn,
	FString& OutError)
{
	const URSoccerLab::FURSObjectType* Type =
		URSoccerLab::FURSObjectTypeRegistry::Get().Find(Spawn.Type);
	if (!Type)
	{
		OutError = FString::Printf(TEXT("unknown object type '%s'"), *Spawn.Type);
		return false;
	}

	const FString GeneratedClassPath = Type->BlueprintAssetPath + TEXT("_C");
	TSubclassOf<AActor> BlueprintClass = LoadClass<AActor>(nullptr, *GeneratedClassPath);
	if (!BlueprintClass)
	{
		OutError = FString::Printf(TEXT("failed to load object blueprint class %s"), *GeneratedClassPath);
		return false;
	}

	const FVector TranslationMeters = Spawn.TranslationMeters.Get(
		FVector(0.0, 0.0, Spawn.Physics.bIsSet ? Spawn.Physics.RadiusM : Type->DefaultBaseHeightM));
	const FQuat RotationXyzw = Spawn.RotationQuatXyzw.Get(FQuat::Identity);
	double MjPos[3] = {TranslationMeters.X, TranslationMeters.Y, TranslationMeters.Z};
	const FVector UELocation = MjUtils::MjToUEPosition(MjPos);
	double MjQuatWxyz[4] = {RotationXyzw.W, RotationXyzw.X, RotationXyzw.Y, RotationXyzw.Z};
	const FRotator UERotation = MjUtils::MjToUERotation(MjQuatWxyz).Rotator();

	FActorSpawnParameters Params;
	Params.Name = FName(*Spawn.ActorId);
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AMjArticulation* Articulation = Manager->GetWorld()->SpawnActor<AMjArticulation>(
		BlueprintClass, UELocation, UERotation, Params);
	if (!Articulation)
	{
		OutError = FString::Printf(TEXT("SpawnActor returned null for object '%s'"), *Spawn.ActorId);
		return false;
	}

	if (Spawn.Physics.bIsSet)
	{
		TArray<UMjSphere *> Spheres;
		Articulation->GetComponents(Spheres);
		if (Spheres.Num() != 1)
		{
			Articulation->Destroy();
			OutError = TEXT("soccer_ball requires exactly one sphere");
			return false;
		}
		auto *Sphere = Spheres[0];
		const auto &P = Spawn.Physics;
		Sphere->bOverride_size = true;
		Sphere->SetRelativeScale3D(FVector(2.0 * P.RadiusM));
		Sphere->mass = P.MassKg;
		Sphere->bOverride_mass = true;
		Sphere->friction = P.Friction;
		Sphere->bOverride_friction = true;
		Sphere->solref = P.Solref;
		Sphere->bOverride_solref = true;
		TArray<UStaticMeshComponent *> Meshes;
		Articulation->GetComponents(Meshes);
		for (auto *Mesh : Meshes)
			if (Mesh->GetStaticMesh() &&
				Mesh->GetStaticMesh()->GetPathName().StartsWith(TEXT("/Game/URSoccerLab/Objects/soccer_ball/")))
				Mesh->SetRelativeScale3D(Mesh->GetRelativeScale3D() * (P.RadiusM / 0.075));
		UE_LOG(LogTemp, Log, TEXT("URS ball '%s': radius=%g m mass=%g kg friction=%g,%g,%g solref=%g,%g"),
			   *Spawn.ActorId, P.RadiusM, P.MassKg, P.Friction[0], P.Friction[1], P.Friction[2], P.Solref[0],
			   P.Solref[1]);
	}
	if (Spawn.Visual.IsSet())
	{
		FFieldTextures Textures;
		if (!RuntimeFieldMaterial || !FFieldTextures::Load(Spawn.Visual.GetValue(), ActiveConfig.SourceDirectory, Textures, OutError, false))
		{
			if (!RuntimeFieldMaterial) OutError = TEXT("missing generic runtime PBR material");
			Articulation->Destroy();
			return false;
		}
		TArray<UStaticMeshComponent*> Meshes;
		Articulation->GetComponents(Meshes);
		int32 Applied = 0;
		for (auto* Mesh : Meshes)
		{
			if (!Mesh->GetStaticMesh() || !Mesh->GetStaticMesh()->GetPathName().StartsWith(TEXT("/Game/URSoccerLab/Objects/soccer_ball/"))) continue;
			for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
			{
				Mesh->SetMaterial(Slot, RuntimeFieldMaterial);
				auto* Material = Mesh->CreateDynamicMaterialInstance(Slot);
				Textures.Apply(Material, Spawn.Visual.GetValue());
				++Applied;
			}
		}
		if (!Applied)
		{
			OutError = TEXT("soccer_ball has no visual mesh material slots");
			Articulation->Destroy();
			return false;
		}
		UE_LOG(LogTemp, Log, TEXT("URS ball '%s': external PBR applied to %d material slots; base_color=%s"),
			*Spawn.ActorId, Applied, *Spawn.Visual.GetValue().BaseColorMap);
	}
	Articulation->ActorId = Spawn.ActorId;
#if WITH_EDITOR
	Articulation->SetActorLabel(Spawn.ActorId);
#endif

	FURSSpawnedObjectInfo Info;
	Info.ActorId = Spawn.ActorId;
	Info.TypeName = Spawn.Type;
	Info.InitialTranslationMeters = TranslationMeters;
	Info.InitialRotationXyzw = RotationXyzw;
	SpawnedObjects.Add(Spawn.ActorId, MoveTemp(Info));
	return true;
}

void UURSSceneConfigComponent::DestroyActorsWithIds(const TSet<FString>& ActorIds)
{
	AActor* Owner = GetOwner();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	TArray<AMjArticulation*> ToDestroy;
	for (AMjArticulation* Articulation : TActorRange<AMjArticulation>(World))
	{
		if (Articulation && ActorIds.Contains(Articulation->ActorId))
		{
			ToDestroy.Add(Articulation);
		}
	}

	for (AMjArticulation* Articulation : ToDestroy)
	{
		const FName StaleName = MakeUniqueObjectName(
			Articulation->GetOuter(), Articulation->GetClass(),
			FName(*FString::Printf(TEXT("stale_%s"), *Articulation->GetName())));
		Articulation->Rename(*StaleName.ToString(), Articulation->GetOuter(),
			REN_DontCreateRedirectors | REN_NonTransactional);
		World->DestroyActor(Articulation);
	}
}

bool UURSSceneConfigComponent::SpawnOneRobot(
	AAMjManager* Manager,
	const URSoccerLab::FURSRobotSpawn& Spawn,
	FString& OutError)
{
    const auto* PackagePtr=RobotPackages.Find(Spawn.Type);
    if (!PackagePtr) { OutError=TEXT("unloaded robot package: ")+Spawn.Type; return false; }
    const auto& Package=**PackagePtr;

	const FVector TranslationMeters = Spawn.TranslationMeters.Get(
		FVector(0.0, 0.0, Package.DefaultBaseHeightM));
	const FQuat RotationXyzw = Spawn.RotationQuatXyzw.Get(FQuat::Identity);

	double MjPos[3] = {TranslationMeters.X, TranslationMeters.Y, TranslationMeters.Z};
	const FVector UELocation = MjUtils::MjToUEPosition(MjPos);

	double MjQuatWxyz[4] = {RotationXyzw.W, RotationXyzw.X, RotationXyzw.Y, RotationXyzw.Z};
	const FQuat UERotation = MjUtils::MjToUERotation(MjQuatWxyz);
	const FRotator UERotator = UERotation.Rotator();

	FActorSpawnParameters Params;
	Params.Name = FName(*Spawn.ActorId);
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AMjArticulation* Articulation = Manager->GetWorld()->SpawnActor<AURSExternalRobot>(
		AURSExternalRobot::StaticClass(), UELocation, UERotator, Params);
	if (!Articulation)
	{
		OutError = FString::Printf(TEXT("SpawnActor returned null for actor_id '%s'"), *Spawn.ActorId);
		return false;
	}

	Articulation->ActorId = Spawn.ActorId;
	if (Articulation->GetName() != Spawn.ActorId)
	{
		const FName DesiredName(*Spawn.ActorId);
		if (!Articulation->Rename(*Spawn.ActorId, nullptr, REN_DontCreateRedirectors | REN_NonTransactional))
		{
			UE_LOG(LogTemp, Warning, TEXT("URSoccerLab scene config: could not rename actor to '%s'."), *Spawn.ActorId);
		}
	}
#if WITH_EDITOR
	Articulation->SetActorLabel(Spawn.ActorId);
#endif

    if (!FExternalRobotLoader::Populate(CastChecked<AURSExternalRobot>(Articulation),Package,OutError))
    { Articulation->Destroy(); return false; }
    // Keep the configured camera channel names; MJCF binding names remain intact.
    TArray<UMjCamera*> ExternalCameras; Articulation->GetComponents<UMjCamera>(ExternalCameras);
    for (UMjCamera* Camera:ExternalCameras)
    {
        if (Camera->MjName==Package.LeftCamera) Camera->Rename(*ActiveConfig.Vision.LeftCamera);
        else if (Camera->MjName==Package.RightCamera) Camera->Rename(*ActiveConfig.Vision.RightCamera);
    }
	ConfigureRobotCameras(Articulation, Spawn.ActorId);
	HideImportedFieldGeoms(Articulation);

	FURSSpawnedRobotInfo Info;
	Info.ActorId = Spawn.ActorId;
	Info.TypeName = Spawn.Type;
	Info.InitialTranslationMeters = TranslationMeters;
	Info.InitialRotationXyzw = RotationXyzw;
	SpawnedRobots.Add(Spawn.ActorId, MoveTemp(Info));
	return true;
}

bool UURSSceneConfigComponent::GetInitialPose(
	const FString& ActorId,
	FVector& OutTranslationMeters,
	FQuat& OutRotationXyzw) const
{
	const FURSSpawnedRobotInfo* Info = SpawnedRobots.Find(ActorId);
	if (!Info)
	{
		return false;
	}
	OutTranslationMeters = Info->InitialTranslationMeters;
	OutRotationXyzw = Info->InitialRotationXyzw;
	return true;
}

void UURSSceneConfigComponent::ConfigureCameraEffects(FPostProcessSettings& Settings, double RateHz) const
{
	const auto& R = ActiveConfig.Render;
	int32 MotionBlurEnabled = R.bIsSet ? int32(R.bMotionBlur) : 1;
	FParse::Value(FCommandLine::Get(), TEXT("URSMotionBlur="), MotionBlurEnabled);

	float MotionBlurAmount = R.MotionBlurAmount;
	FParse::Value(FCommandLine::Get(), TEXT("URSMotionBlurAmount="), MotionBlurAmount);
	MotionBlurAmount = FMath::Clamp(MotionBlurAmount, 0.0f, 1.0f);

	float MotionBlurMax = R.MotionBlurMaxPercent;
	FParse::Value(FCommandLine::Get(), TEXT("URSMotionBlurMax="), MotionBlurMax);
	MotionBlurMax = FMath::Clamp(MotionBlurMax, 0.0f, 100.0f);

	int32 MotionBlurTargetFps = R.MotionBlurTargetFps > 0 ? R.MotionBlurTargetFps : FMath::Clamp(FMath::RoundToInt(RateHz), 1, 120);
	FParse::Value(FCommandLine::Get(), TEXT("URSMotionBlurTargetFPS="), MotionBlurTargetFps);
	MotionBlurTargetFps = FMath::Clamp(MotionBlurTargetFps, 0, 120);

 if (R.bIsSet && !R.bEnable) MotionBlurEnabled = 0;
 Settings.bOverride_MotionBlurAmount = true;
 Settings.MotionBlurAmount = MotionBlurEnabled ? MotionBlurAmount : 0.0f;
 Settings.bOverride_MotionBlurMax = true;
 Settings.MotionBlurMax = MotionBlurMax;
 Settings.bOverride_MotionBlurTargetFPS = true;
 Settings.MotionBlurTargetFPS = MotionBlurTargetFps;
 Settings.bOverride_MotionBlurPerObjectSize = true;
 Settings.MotionBlurPerObjectSize = 0.0f;
 if (R.bIsSet)
 {
  // Captures inherit the hall's volume, which explicitly enables auto exposure.
  // Override it on every camera so illumination changes remain measurable.
  Settings.bOverride_AutoExposureMethod = true;
  Settings.AutoExposureMethod = R.bAutoExposure ? AEM_Histogram : AEM_Manual;
  Settings.bOverride_AutoExposureBias = true;
  Settings.AutoExposureBias = R.ExposureCompensation;
  if (!R.bAutoExposure)
  {
   Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
   Settings.AutoExposureApplyPhysicalCameraExposure = false;
  }
  Settings.bOverride_FilmGrainIntensity = true;
  Settings.FilmGrainIntensity = R.bEnable ? R.FilmGrainIntensity : 0.0f;
  Settings.bOverride_FilmGrainIntensityShadows = true;
  Settings.FilmGrainIntensityShadows = R.FilmGrainShadows;
  Settings.bOverride_FilmGrainIntensityMidtones = true;
  Settings.FilmGrainIntensityMidtones = R.FilmGrainMidtones;
  Settings.bOverride_FilmGrainIntensityHighlights = true;
  Settings.FilmGrainIntensityHighlights = R.FilmGrainHighlights;
  Settings.bOverride_FilmGrainTexelSize = true;
  Settings.FilmGrainTexelSize = R.FilmGrainTexelSize;
 }
}

void UURSSceneConfigComponent::ConfigureRobotCameras(AMjArticulation* Articulation, const FString& ActorId)
{
	if (!Articulation)
	{
		return;
	}

 double CameraRateHz = ActiveConfig.CameraFreq > 0 ? ActiveConfig.CameraFreq : ActiveConfig.Vision.Rgb.RateHz;
 FParse::Value(FCommandLine::Get(), TEXT("URSCameraRateHz="), CameraRateHz);
 FPostProcessSettings Effects;
 ConfigureCameraEffects(Effects, CameraRateHz);
 const bool MotionBlurEnabled = Effects.MotionBlurAmount > 0;
 const float MotionBlurAmount = Effects.MotionBlurAmount;
 const float MotionBlurMax = Effects.MotionBlurMax;
 const int32 MotionBlurTargetFps = Effects.MotionBlurTargetFPS;

	TArray<UMjCamera*> Cameras;
	Articulation->GetComponents<UMjCamera>(Cameras);

	// SpawnActor may invoke BeginPlay before returning when a scene is
	// reloaded during play. In that case URLab may already have opened its
	// configured ZMQ/SHM publisher. Stop the camera first so those workers
	// and mappings are actually destroyed rather than merely ignored.
	TSet<UMjCamera*> CamerasToRestart;
	for (UMjCamera* Camera : Cameras)
	{
		if (Camera && Camera->IsStreamingActive())
		{
			CamerasToRestart.Add(Camera);
			Camera->SetStreamingEnabled(false);
		}
	}

	UMjCamera* LeftCamera = nullptr;
	UMjCamera* RightCamera = nullptr;
	for (UMjCamera* Camera : Cameras)
	{
		if (!Camera) continue;
		if (Camera->GetName() == ActiveConfig.Vision.LeftCamera)
		{
			LeftCamera = Camera;
		}
		if (Camera->GetName() == ActiveConfig.Vision.RightCamera)
		{
			RightCamera = Camera;
		}
	}
	if (!LeftCamera || !RightCamera)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[URS Camera] actor=%s could not resolve configured cameras left='%s' right='%s'."),
			*ActorId, *ActiveConfig.Vision.LeftCamera, *ActiveConfig.Vision.RightCamera);
	}
	else
	{
		LeftCamera->CaptureMode = EMjCameraMode::Real;
		if (ActiveConfig.Vision.Mode == EURSVisionMode::Rgbd)
		{
			// A URLab camera has one capture mode. Reuse the right-eye
			// component as a depth capture, but align it exactly with the
			// left-eye RGB component so RGB and depth share a viewpoint.
			RightCamera->CaptureMode = EMjCameraMode::Depth;
			RightCamera->Pos = LeftCamera->Pos;
			RightCamera->Quat = LeftCamera->Quat;
			RightCamera->bOverride_Pos = LeftCamera->bOverride_Pos;
			RightCamera->bOverride_Quat = LeftCamera->bOverride_Quat;
			RightCamera->SetRelativeTransform(LeftCamera->GetRelativeTransform());
			RightCamera->DepthFarCm = static_cast<float>(
				ActiveConfig.Vision.Depth.MaxDepthMeters * 100.0);
		}
		else
		{
			RightCamera->CaptureMode = EMjCameraMode::Real;
		}
	}

	for (int32 CamIdx = 0; CamIdx < Cameras.Num(); ++CamIdx)
	{
		UMjCamera* Camera = Cameras[CamIdx];
		if (!Camera)
		{
			continue;
		}
		// URSoccerLab owns camera delivery through its versioned TCP
		// transport. Do not also launch URLab's per-camera ZMQ/SHM workers:
		// they duplicate readback copies, consume ports, and can overwrite
		// the same one-frame buffers that TCP is scheduling.
		Camera->bEnableZmqBroadcast = false;
		Camera->bEnableShmBroadcast = false;
		if (Camera->CaptureComponent)
		{
			USceneCaptureComponent2D* Capture = Camera->CaptureComponent;
			Capture->bUseRayTracingIfEnabled = true;
			Capture->bAlwaysPersistRenderingState = true;
			Capture->ShowFlags.SetMotionBlur(MotionBlurEnabled != 0);

			FPostProcessSettings& PostProcess = Capture->PostProcessSettings;
			ConfigureCameraEffects(PostProcess, CameraRateHz);
		}
		if (Camera->resolution.Num() < 2)
		{
			Camera->bOverride_resolution = true;
			Camera->resolution = {640, 480};
		}
		if (Camera->fovy <= 0.0f)
		{
			Camera->bOverride_fovy = true;
			Camera->fovy = 90.0f;
		}
		Camera->Modify();
	}

	// Keep an already-running camera running, now solely as a capture source
	// for URSoccerLab's consolidated TCP transport. Cameras configured before
	// BeginPlay are enabled later by UURSRobotCoreComponent as usual.
	for (UMjCamera* Camera : CamerasToRestart)
	{
		Camera->SetStreamingEnabled(true);
	}

	UE_LOG(LogTemp, Log,
		TEXT("[URS Camera] actor=%s mode=%s motion_blur=%s amount=%.3f max=%.3f target_fps=%d."),
		*ActorId,
		ActiveConfig.Vision.Mode == EURSVisionMode::Rgbd ? TEXT("rgbd") : TEXT("stereo_rgb"),
		MotionBlurEnabled != 0 ? TEXT("on") : TEXT("off"),
		MotionBlurAmount,
		MotionBlurMax,
		MotionBlurTargetFps);
}

void UURSSceneConfigComponent::HideImportedFieldGeoms(AMjArticulation* Articulation)
{
	if (!Articulation)
	{
		return;
	}

	TArray<UMjGeom*> Geoms;
	Articulation->GetComponents<UMjGeom>(Geoms);
	for (UMjGeom* Geom : Geoms)
	{
		if (!Geom)
		{
			continue;
		}
		const FString MjName = Geom->MjName.IsEmpty() ? Geom->GetName() : Geom->MjName;
		if (MjName == TEXT("floor") || MjName == TEXT("vision_floor") || MjName == TEXT("vision_marker"))
		{
			Geom->SetGeomVisibility(false);
		}
	}
}

void UURSSceneConfigComponent::ApplyPhysicsConfig()
{
	const double Dt = ActiveConfig.MujocoDt;
	const double StateHz = ActiveConfig.StateFreq;
	const double CamHz = ActiveConfig.CameraFreq;

	if (Dt <= 0.0 && StateHz <= 0.0 && CamHz <= 0.0) return;

	GetWorld()->GetTimerManager().SetTimerForNextTick([this, Dt, StateHz, CamHz]()
	{
		if (Dt > 0.0)
		{
			for (TActorIterator<AAMjManager> It(GetWorld()); It; ++It)
			{
				if (It->PhysicsEngine && It->PhysicsEngine->GetModel())
				{
					It->PhysicsEngine->GetModel()->opt.timestep = Dt;
					UE_LOG(LogTemp, Log, TEXT("[URSoccerLab] physics timestep = %.6f (%.0f Hz)"), Dt, 1.0 / Dt);
				}
				break;
			}
		}
		if (GetOwner())
		{
			if (StateHz > 0.0)
			{
				if (auto* Transport = GetOwner()->FindComponentByClass<UURSTcpTransportComponent>())
					Transport->StateRateHz = StateHz;
			}
			if (CamHz > 0.0)
			{
				if (auto* Camera = GetOwner()->FindComponentByClass<UURSCameraStreamComponent>())
					Camera->SetCameraRate(CamHz);
			}
		}
	});
}

void UURSSceneConfigComponent::ApplyLightingConfig()
{
 if (!ActiveConfig.Lighting.bIsSet || !GetWorld()) return;
 int32 Count = 0;
 int32 Surfaces = 0;
 for (TActorIterator<AActor> It(GetWorld()); It; ++It)
 {
  if (It->ActorHasTag(TEXT("URS_EmissiveLampSurface")) && ActiveConfig.Lighting.EmissiveIntensity.IsSet())
  {
   TArray<UStaticMeshComponent*> Meshes;
   It->GetComponents(Meshes);
   for (auto* Mesh : Meshes)
   {
    for (int32 Index = 0; Index < Mesh->GetNumMaterials(); ++Index)
    {
     auto* Material = Mesh->GetMaterial(Index);
     FLinearColor Previous;
     if (!Material || !Material->GetVectorParameterValue(FMaterialParameterInfo(TEXT("EmissiveFactor")), Previous))
     {
      UE_LOG(LogTemp, Warning, TEXT("[URS Lighting] %s material %d lacks EmissiveFactor."), *It->GetName(), Index);
      continue;
     }
     auto* Dynamic = Cast<UMaterialInstanceDynamic>(Material);
     if (!Dynamic) Dynamic = Mesh->CreateDynamicMaterialInstance(Index);
     if (Dynamic)
     {
      const float Intensity = ActiveConfig.Lighting.EmissiveIntensity.GetValue();
      Dynamic->SetVectorParameterValue(TEXT("EmissiveFactor"), FLinearColor(Intensity, Intensity, Intensity, 1));
      Dynamic->SetScalarParameterValue(TEXT("EmissiveStrength"), 1.0f);
      ++Surfaces;
     }
    }
    // Recreate the proxy so config reapplication refreshes Lumen's cached emission.
    Mesh->MarkRenderStateDirty();
   }
  }
  if (!It->ActorHasTag(TEXT("URS_AutoEmissiveLamp"))) continue;
  TArray<UPointLightComponent*> Lights;
  It->GetComponents(Lights);
  for (auto* Light : Lights)
  {
   Light->SetIntensityUnits(ELightUnits::Lumens);
   Light->SetIntensity(ActiveConfig.Lighting.LampIntensityLumens);
   Light->SetSourceRadius(ActiveConfig.Lighting.SourceRadiusCm);
   Light->SetSpecularScale(ActiveConfig.Lighting.SpecularScale);
   ++Count;
  }
 }
 UE_LOG(LogTemp, Log, TEXT("[URS Lighting] %d auxiliary lamps at %.3f lumens each; %d emissive surfaces configured."), Count, ActiveConfig.Lighting.LampIntensityLumens, Surfaces);
}

void UURSSceneConfigComponent::ApplyRenderConfig()
{
	if (!ActiveConfig.Render.bIsSet) return;

	UWorld* World = GetWorld();
	if (!World || !GEngine) return;

	auto Exec = [World](const TCHAR* Cmd)
	{
		GEngine->Exec(World, Cmd);
	};

	const FURSRenderConfig& R = ActiveConfig.Render;

	if (!R.bEnable)
	{
		// Minimal render preset: lowest cost when rendering is "disabled".
		Exec(TEXT("r.ScreenPercentage 10"));
		Exec(TEXT("r.DynamicGlobalIlluminationMethod 0"));
		Exec(TEXT("r.ReflectionMethod 0"));
		Exec(TEXT("r.ShadowQuality 0"));
		Exec(TEXT("r.MotionBlurQuality 0"));
		Exec(TEXT("r.AntiAliasingMethod 0"));
		Exec(TEXT("r.ViewDistanceScale 0.1"));
		Exec(TEXT("r.DefaultFeature.AutoExposure 0"));
		Exec(TEXT("r.Lumen.HardwareRayTracing 0"));
		UE_LOG(LogTemp, Log, TEXT("[URSoccerLab] render: disabled (minimal preset applied)."));
		return;
	}

	Exec(R.bLumen ? TEXT("r.DynamicGlobalIlluminationMethod 1") : TEXT("r.DynamicGlobalIlluminationMethod 0"));
	Exec(R.bLumen ? TEXT("r.ReflectionMethod 1") : TEXT("r.ReflectionMethod 2"));
	// r.RayTracing is read-only at runtime; r.Lumen.HardwareRayTracing is the
	// settable toggle for hardware-accelerated Lumen traces.
	Exec(R.bHardwareRayTracing ? TEXT("r.Lumen.HardwareRayTracing 1") : TEXT("r.Lumen.HardwareRayTracing 0"));

	int32 AAMethod = AAM_TSR;
	if (R.AntiAliasing == TEXT("none")) AAMethod = AAM_None;
	else if (R.AntiAliasing == TEXT("fxaa")) AAMethod = AAM_FXAA;
	else if (R.AntiAliasing == TEXT("taa")) AAMethod = AAM_TemporalAA;
	Exec(*FString::Printf(TEXT("r.AntiAliasingMethod %d"), AAMethod));

	Exec(*FString::Printf(TEXT("r.ScreenPercentage %g"), R.ScreenPercentage));
	Exec(*FString::Printf(TEXT("r.ShadowQuality %d"), R.ShadowQuality));
	FPostProcessSettings Effects;
	ConfigureCameraEffects(Effects, ActiveConfig.Vision.Rgb.RateHz);
	Exec(Effects.MotionBlurAmount > 0 ? TEXT("r.MotionBlurQuality 4") : TEXT("r.MotionBlurQuality 0"));
	Exec(R.bAutoExposure ? TEXT("r.DefaultFeature.AutoExposure 1") : TEXT("r.DefaultFeature.AutoExposure 0"));
	Exec(*FString::Printf(TEXT("r.EyeAdaptationExposureCompensation %g"), R.ExposureCompensation));

	// nDisplay owns the render target dimensions. Applying the scene's normal
	// window resolution here changes the atlas size after the viewports have
	// been bound and makes camera tile copies invalid.
	const bool bNDisplayOwnsResolution =
		FParse::Param(FCommandLine::Get(), TEXT("URSNDisplayCameras"));
	if (!bNDisplayOwnsResolution && R.ResolutionX.IsSet() && R.ResolutionY.IsSet())
	{
		Exec(*FString::Printf(TEXT("r.setres %dx%d"),
			R.ResolutionX.GetValue(), R.ResolutionY.GetValue()));
	}

	UE_LOG(LogTemp, Log,
		TEXT("[URSoccerLab] render: enable=true lumen=%d hwrt=%d aa=%s screen=%g shadow=%d res=%s"),
		R.bLumen ? 1 : 0, R.bHardwareRayTracing ? 1 : 0, *R.AntiAliasing, R.ScreenPercentage, R.ShadowQuality,
		(!bNDisplayOwnsResolution && R.ResolutionX.IsSet() && R.ResolutionY.IsSet())
			? *FString::Printf(TEXT("%dx%d"), R.ResolutionX.GetValue(), R.ResolutionY.GetValue())
			: (bNDisplayOwnsResolution ? TEXT("nDisplay atlas") : TEXT("unchanged")));
}

// Keep the flat field surface as the substrate; optional 3D grass blades overlay it.
bool UURSSceneConfigComponent::ApplyFieldConfig(FString &OutError)
{
	const auto &F = ActiveConfig.Field;
	UWorld *World = GetWorld();
	if (!World || !RuntimeFieldMesh || !RuntimeFieldMaterial)
	{
		OutError = TEXT("runtime field mesh/material is missing");
		return false;
	}
	FFieldTextures Textures;
	if (!FFieldTextures::Load(F.Visual, ActiveConfig.SourceDirectory, Textures, OutError))
		return false;
	const FVector MeshSize = RuntimeFieldMesh->GetBoundingBox().GetSize();
	if (MeshSize.X <= 0 || MeshSize.Y <= 0)
	{
		OutError = TEXT("runtime field mesh has invalid bounds");
		return false;
	}
	if (!RuntimeFieldSurface)
	{
		RuntimeFieldSurface = NewObject<UStaticMeshComponent>(GetOwner(), TEXT("URSRuntimeField"));
		GetOwner()->AddInstanceComponent(RuntimeFieldSurface);
	}
	// Configure while unregistered, including on reload: changing a registered
	// static primitive's transform is unsupported and can leave stale render data.
	if (RuntimeFieldSurface->IsRegistered())
		RuntimeFieldSurface->UnregisterComponent();
	RuntimeFieldSurface->SetMobility(EComponentMobility::Static);
	RuntimeFieldSurface->SetStaticMesh(RuntimeFieldMesh);
	RuntimeFieldSurface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RuntimeFieldSurface->SetWorldLocation(FVector::ZeroVector);
	RuntimeFieldSurface->SetWorldRotation(FRotator::ZeroRotator);
	RuntimeFieldSurface->SetWorldScale3D(
		FVector((F.LengthM + 2 * F.BorderXM) * 100 / MeshSize.X,
				(F.WidthM + 2 * F.BorderYM) * 100 / MeshSize.Y, 1));
	RuntimeFieldSurface->SetMaterial(0, RuntimeFieldMaterial);
	auto *Material = RuntimeFieldSurface->CreateAndSetMaterialInstanceDynamic(0);
	// Preserve the existing glTF shader and field detail tiling.
	Textures.Apply(Material, F.Visual, FLinearColor(0, 0,
		(F.LengthM + 2 * F.BorderXM) / F.Visual.DetailTileSizeM,
		(F.WidthM + 2 * F.BorderYM) / F.Visual.DetailTileSizeM));
	RuntimeFieldSurface->RegisterComponent();

	if (!F.Visual.GrassMesh.IsEmpty())
	{
		const FString GrassPath = FPaths::ConvertRelativePathToFull(
			FPaths::IsRelative(F.Visual.GrassMesh)
				? FPaths::Combine(ActiveConfig.SourceDirectory, F.Visual.GrassMesh)
				: F.Visual.GrassMesh);
		FglTFRuntimeConfig LoaderConfig;
		LoaderConfig.TransformBaseType = EglTFRuntimeTransformBaseType::YForward;
		UglTFRuntimeAsset* GrassAsset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(
			GrassPath, false, LoaderConfig);
		if (!GrassAsset)
		{
			OutError = TEXT("failed to load field grass GLB: ") + GrassPath;
			return false;
		}
		FglTFRuntimeStaticMeshConfig MeshConfig;
		MeshConfig.Outer = GetTransientPackage();
		UStaticMesh* GrassMesh = GrassAsset->LoadStaticMeshRecursive(TEXT(""), {}, MeshConfig);
		if (!GrassMesh || !GrassAsset->GetErrors().IsEmpty())
		{
			OutError = TEXT("failed to build field grass mesh: ") + GrassPath;
			if (!GrassAsset->GetErrors().IsEmpty())
				OutError += TEXT(": ") + FString::Join(GrassAsset->GetErrors(), TEXT("; "));
			return false;
		}
		const FVector GrassBounds = GrassMesh->GetBoundingBox().GetSize();
		if (GrassBounds.X <= 0 || GrassBounds.Y <= 0)
		{
			OutError = TEXT("field grass mesh has invalid pitch bounds");
			return false;
		}
		if (!RuntimeGrassSurface)
		{
			RuntimeGrassSurface = NewObject<UStaticMeshComponent>(GetOwner(), TEXT("URSRuntimeGrass"));
			GetOwner()->AddInstanceComponent(RuntimeGrassSurface);
		}
		if (RuntimeGrassSurface->IsRegistered())
			RuntimeGrassSurface->UnregisterComponent();
		RuntimeGrassSurface->SetMobility(EComponentMobility::Static);
		RuntimeGrassSurface->SetStaticMesh(GrassMesh);
		RuntimeGrassSurface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		RuntimeGrassSurface->SetCastShadow(false);
		RuntimeGrassSurface->SetWorldLocation(FVector::ZeroVector);
		RuntimeGrassSurface->SetWorldRotation(FRotator::ZeroRotator);
		RuntimeGrassSurface->SetWorldScale3D(
			FVector((F.LengthM + 2 * F.BorderXM) * 100 / GrassBounds.X,
					(F.WidthM + 2 * F.BorderYM) * 100 / GrassBounds.Y, 1));
		RuntimeGrassSurface->RegisterComponent();
	}
	else if (RuntimeGrassSurface)
	{
		RuntimeGrassSurface->DestroyComponent();
		RuntimeGrassSurface = nullptr;
	}
	UE_LOG(LogTemp, Log, TEXT("URS field: length=%g width=%g borders=%g,%g base_color=%s grass_mesh=%s detail_tile=%g m"),
		F.LengthM, F.WidthM, F.BorderXM, F.BorderYM, *F.Visual.BaseColorMap, *F.Visual.GrassMesh, F.Visual.DetailTileSizeM);
	return true;
}

// Configure the existing hall ground before model compilation; create one only
// for worlds that lack the hall's ground actor (e.g. standalone scene tests).
bool UURSSceneConfigComponent::ApplyFieldPhysicsConfig(FString& OutError)
{
	UMjPlane* Ground = nullptr;
	for (TActorIterator<AMjArticulation> It(GetWorld()); It; ++It)
	{
		TArray<UMjPlane*> Planes;
		It->GetComponents(Planes);
		for (UMjPlane* Plane : Planes)
			if (Plane->MjName == TEXT("field_ground"))
			{
				if (Ground) { OutError = TEXT("multiple field_ground planes found"); return false; }
				Ground = Plane;
			}
	}
	if (!Ground)
	{
		RuntimeGround = GetWorld()->SpawnActor<AMjArticulation>();
		if (!RuntimeGround) { OutError = TEXT("could not spawn field ground"); return false; }
		RuntimeGround->ActorId = TEXT("__urs_ground");
		auto* Root = NewObject<UMjWorldBody>(RuntimeGround, TEXT("FieldWorldBody"));
		RuntimeGround->AddInstanceComponent(Root);
		RuntimeGround->SetRootComponent(Root);
		Root->RegisterComponent();
		Ground = NewObject<UMjPlane>(RuntimeGround, TEXT("field_ground"));
		RuntimeGround->AddInstanceComponent(Ground);
		Ground->SetupAttachment(Root);
		Ground->MjName = TEXT("field_ground");
		Ground->bOverride_Type = true; Ground->Type = EMjGeomType::Plane;
		Ground->bOverride_Pos = true; Ground->Pos = FVector::ZeroVector;
		Ground->bOverride_Quat = true; Ground->Quat = FQuat::Identity;
		Ground->bOverride_contype = true; Ground->contype = 1;
		Ground->bOverride_conaffinity = true; Ground->conaffinity = 1;
		Ground->bOverride_group = true; Ground->group = 3;
		Ground->RegisterComponent();
	}
	const auto& P = ActiveConfig.Field.Physics;
	Ground->bOverride_friction = true; Ground->friction = P.Friction;
	Ground->bOverride_condim = true; Ground->condim = P.Condim;
	Ground->bOverride_solref = true; Ground->solref = P.Solref;
	Ground->bOverride_solimp = true; Ground->solimp = P.Solimp;
	// MuJoCo planes are infinite; size controls only their debug visualization.
	Ground->bOverride_size = true;
	Ground->size = {float((ActiveConfig.Field.LengthM + 2 * ActiveConfig.Field.BorderXM) / 2),
		float((ActiveConfig.Field.WidthM + 2 * ActiveConfig.Field.BorderYM) / 2), 0.1f};
	Ground->SetGeomVisibility(false);
	UE_LOG(LogTemp, Log, TEXT("URS field physics: friction=%g,%g,%g condim=%d solref=%g,%g"),
		P.Friction[0], P.Friction[1], P.Friction[2], P.Condim, P.Solref[0], P.Solref[1]);
	return true;
}

// Static worldbody geoms participate in the same MuJoCo model as robots and ball.
bool UURSSceneConfigComponent::ApplyGoalsConfig(FString &OutError)
{
	if (!GoalCylinderMesh || !GoalMaterial)
	{
		OutError = TEXT("goal cylinder mesh/material is missing");
		return false;
	}
	if (RuntimeGoals)
		RuntimeGoals->Destroy();
	RuntimeGoals = GetWorld()->SpawnActor<AMjArticulation>();
	if (!RuntimeGoals)
	{
		OutError = TEXT("could not spawn goals");
		return false;
	}
	RuntimeGoals->ActorId = TEXT("__urs_goals");
	auto *Root = NewObject<UMjWorldBody>(RuntimeGoals, TEXT("GoalWorldBody"));
	RuntimeGoals->AddInstanceComponent(Root);
	RuntimeGoals->SetRootComponent(Root);
	Root->RegisterComponent();
	const auto &G = ActiveConfig.Goals;
	const double PostHeight = G.HeightM + 2 * G.PostRadiusM;
	for (int32 GoalIndex = 0; GoalIndex < 2; ++GoalIndex)
	{
		const auto &Pose = G.Poses[GoalIndex];
		double Position[3] = {Pose.TranslationMeters.X, Pose.TranslationMeters.Y, Pose.TranslationMeters.Z};
		const FQuat Yaw(FVector::UpVector, FMath::DegreesToRadians(Pose.YawDeg));
		double Quaternion[4] = {Yaw.W, Yaw.X, Yaw.Y, Yaw.Z};
		const FTransform GoalTransform(MjUtils::MjToUERotation(Quaternion), MjUtils::MjToUEPosition(Position));
		for (int32 Part = 0; Part < 3; ++Part)
		{
			const bool Crossbar = Part == 2;
			const double Length = Crossbar ? G.WidthM + 4 * G.PostRadiusM : PostHeight;
			const FVector LocalCenter(0, Crossbar ? 0 : (Part == 0 ? -1 : 1) * (G.WidthM / 2 + G.PostRadiusM) * 100,
									  (Crossbar ? G.HeightM + G.PostRadiusM : PostHeight / 2) * 100);
			const FQuat Rotation =
				GoalTransform.GetRotation() * (Crossbar ? FQuat(FVector::ForwardVector, PI / 2) : FQuat::Identity);
			const FVector Center = GoalTransform.TransformPosition(LocalCenter);
			const FVector Scale(2 * G.PostRadiusM, 2 * G.PostRadiusM, Length);
			const FString Name = FString::Printf(TEXT("goal_%d_%s"), GoalIndex,
												 Crossbar	 ? TEXT("crossbar")
												 : Part == 0 ? TEXT("left_post")
															 : TEXT("right_post"));
			auto *Geom = NewObject<UMjCylinder>(RuntimeGoals, *Name);
			RuntimeGoals->AddInstanceComponent(Geom);
			Geom->SetupAttachment(Root);
			Geom->MjName = Name;
			Geom->SetRelativeTransform(FTransform(Rotation, Center, Scale));
			Geom->Pos = Center;
			Geom->bOverride_Pos = true;
			Geom->Quat = Rotation;
			Geom->bOverride_Quat = true;
			Geom->bOverride_size = true;
			Geom->contype = 1;
			Geom->bOverride_contype = true;
			Geom->conaffinity = 1;
			Geom->bOverride_conaffinity = true;
			Geom->group = 3;
			Geom->bOverride_group = true;
			Geom->RegisterComponent();
			Geom->SetGeomVisibility(false);
			// Keep visuals independent of hidden MuJoCo debug primitives.
			auto *Visual = NewObject<UStaticMeshComponent>(RuntimeGoals, *(Name + TEXT("_visual")));
			RuntimeGoals->AddInstanceComponent(Visual);
			Visual->SetupAttachment(Root);
			Visual->SetStaticMesh(GoalCylinderMesh);
			Visual->SetMaterial(0, GoalMaterial);
			Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Visual->SetRelativeTransform(FTransform(Rotation, Center, Scale));
			Visual->RegisterComponent();
		}
	}
	UE_LOG(LogTemp, Log, TEXT("URS goals: two goals, six collision cylinders; clear width=%g height=%g radius=%g"),
		   G.WidthM, G.HeightM, G.PostRadiusM);
	return true;
}

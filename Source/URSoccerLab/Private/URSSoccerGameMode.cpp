#include "URSSoccerGameMode.h"

#include "EngineUtils.h"
#include "GameFramework/SpectatorPawn.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "MuJoCo/Core/AMjManager.h"
#include "Scene/URSSceneConfigComponent.h"
#include "Scene/URSRobotTypeRegistry.h"
#include "Core/URSRobotCoreComponent.h"
#include "Transport/URSTcpTransportComponent.h"
#include "NDisplay/URSDisplayClusterCameraBinderComponent.h"

AURSSoccerGameMode::AURSSoccerGameMode()
{
	DefaultPawnClass = ASpectatorPawn::StaticClass();
}

void AURSSoccerGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	URSoccerLab::FURSRobotTypeRegistry::Get().RegisterDefaultTypes();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	AAMjManager* Manager = nullptr;
	for (TActorIterator<AAMjManager> It(World); It; ++It)
	{
		Manager = *It;
		break;
	}
	if (!Manager)
	{
		UE_LOG(LogTemp, Warning, TEXT("URSSoccerGameMode: no AAMjManager in world; skipping robot bootstrap."));
		return;
	}

	UURSSceneConfigComponent* SceneComp = Manager->FindComponentByClass<UURSSceneConfigComponent>();
	if (!SceneComp)
	{
		UE_LOG(LogTemp, Warning, TEXT("URSSoccerGameMode: no UURSSceneConfigComponent on AAMjManager; skipping robot bootstrap."));
		return;
	}

	FString SceneConfigOverride;
	if (FParse::Value(FCommandLine::Get(), TEXT("URSSceneConfig="), SceneConfigOverride)
		&& !SceneConfigOverride.IsEmpty())
	{
		SceneComp->ConfigPath = SceneConfigOverride;
		UE_LOG(LogTemp, Log, TEXT("URSSoccerGameMode: using scene config override '%s'."),
			*SceneConfigOverride);
	}

	// Spawn robots BEFORE any BeginPlay. AAMjManager::BeginPlay compiles the
	// MuJoCo model; if the robots exist in the world at that point, they are
	// discovered via TActorIterator and enter the compiled mjModel.
	FString ApplyError;
	if (!SceneComp->ApplyConfig(ApplyError))
	{
		UE_LOG(LogTemp, Error, TEXT("URSSoccerGameMode: scene config apply failed: %s"), *ApplyError);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("URSoccerGameMode: spawned %d robot(s) from scene config before BeginPlay."),
			SceneComp->GetSpawnedRobots().Num());
	}

	if (!Manager->FindComponentByClass<UURSRobotCoreComponent>())
	{
		UURSRobotCoreComponent* Core = NewObject<UURSRobotCoreComponent>(Manager, TEXT("URSRobotCore"));
		Core->RegisterComponent();
	}
	if (!Manager->FindComponentByClass<UURSTcpTransportComponent>())
	{
		UURSTcpTransportComponent* Transport = NewObject<UURSTcpTransportComponent>(Manager, TEXT("URSTcpTransport"));
		Transport->RegisterComponent();
	}
	if ((FParse::Param(FCommandLine::Get(), TEXT("URSNDisplayCameras"))
			|| FParse::Param(FCommandLine::Get(), TEXT("dc_cluster")))
		&& !Manager->FindComponentByClass<UURSDisplayClusterCameraBinderComponent>())
	{
		UURSDisplayClusterCameraBinderComponent* Binder =
			NewObject<UURSDisplayClusterCameraBinderComponent>(Manager, TEXT("URSDisplayClusterCameraBinder"));
		Binder->RegisterComponent();
	}
}

void AURSSoccerGameMode::StartPlay()
{
	Super::StartPlay();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// This is diagnostic-only. UE dispatches ordinary deferred
	// SceneCaptureComponent2D updates from the viewport draw, so suppressing
	// world rendering also freezes those camera targets. nDisplay is exempt
	// because its atlas is itself the main viewport.
	int32 bDisableMainViewport = 0;
	FParse::Value(
		FCommandLine::Get(), TEXT("URSDisableMainViewport="), bDisableMainViewport);
	if (bDisableMainViewport != 0
		&& !FParse::Param(FCommandLine::Get(), TEXT("dc_cluster")))
	{
		// Keep the RHI and SceneCaptureComponent2D rendering active while
		// suppressing the unused spectator/main-view scene render.
		UGameplayStatics::SetEnableWorldRendering(World, false);
		UE_LOG(LogTemp, Log,
			TEXT("URSSoccerGameMode: main viewport world rendering disabled; scene captures remain active."));
	}

	for (TActorIterator<AAMjManager> It(World); It; ++It)
	{
		if (UURSRobotCoreComponent* Core = It->FindComponentByClass<UURSRobotCoreComponent>())
		{
			// URLab has completed AAMjManager::BeginPlay and compiled mjModel by
			// this point, so Initialize's retry path can build qpos endpoints.
			Core->Initialize();
		}
		break;
	}
}

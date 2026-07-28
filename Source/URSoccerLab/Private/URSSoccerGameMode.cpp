#include "URSSoccerGameMode.h"

#include "EngineUtils.h"
#include "GameFramework/SpectatorPawn.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "MuJoCo/Core/AMjManager.h"
#include "Scene/URSSceneConfigComponent.h"
#include "Scene/URSRobotTypeRegistry.h"
#include "Core/URSRobotCoreComponent.h"
#include "Transport/URSTcpTransportComponent.h"

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
}

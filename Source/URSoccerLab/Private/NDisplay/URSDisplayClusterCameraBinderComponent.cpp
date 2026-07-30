#include "NDisplay/URSDisplayClusterCameraBinderComponent.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Containers/DisplayClusterProjectionCameraPolicySettings.h"
#include "EngineUtils.h"
#include "IDisplayCluster.h"
#include "IDisplayClusterProjection.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "Render/IDisplayClusterRenderManager.h"
#include "Render/Viewport/IDisplayClusterViewport.h"
#include "Render/Viewport/IDisplayClusterViewportManager.h"
#include "TimerManager.h"
#include "UnrealClient.h"

UURSDisplayClusterCameraBinderComponent::UURSDisplayClusterCameraBinderComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UURSDisplayClusterCameraBinderComponent::BeginPlay()
{
	Super::BeginPlay();
	FParse::Value(
		FCommandLine::Get(),
		TEXT("URSNDisplayCameraCount="),
		RequestedCameraCount);
	RequestedCameraCount = FMath::Clamp(RequestedCameraCount, 1, 20);
	FParse::Value(
		FCommandLine::Get(),
		TEXT("URSNDisplayCameraName="),
		RequestedCameraName);
}

void UURSDisplayClusterCameraBinderComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bBound)
	{
		bBound = TryBindCameras();
		if (bBound)
		{
			SetComponentTickEnabled(false);
		}
	}
}

bool UURSDisplayClusterCameraBinderComponent::TryBindCameras()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	TArray<UMjCamera*> AllCameras;
	for (TActorIterator<AMjArticulation> It(World); It; ++It)
	{
		TArray<UMjCamera*> ActorCameras;
		It->GetComponents<UMjCamera>(ActorCameras);
		AllCameras.Append(ActorCameras);
	}

	TArray<UMjCamera*> Cameras = AllCameras;
	if (!RequestedCameraName.IsEmpty())
	{
		Cameras.RemoveAll([this](const UMjCamera* Camera)
		{
			return !Camera || Camera->MjName != RequestedCameraName;
		});
	}
	Cameras.Sort([](const UMjCamera& Left, const UMjCamera& Right)
	{
		const FString LeftKey = Left.GetOwner()->GetName() + TEXT("/") + Left.MjName;
		const FString RightKey = Right.GetOwner()->GetName() + TEXT("/") + Right.MjName;
		return LeftKey < RightKey;
	});

	if (Cameras.Num() < RequestedCameraCount)
	{
		return false;
	}

	IDisplayClusterRenderManager* RenderManager =
		IDisplayCluster::Get().GetRenderMgr();
	if (!RenderManager || !RenderManager->GetViewportManager())
	{
		return false;
	}
	TArray<IDisplayClusterViewport*> Viewports;
	Viewports.Reserve(RequestedCameraCount);
	for (int32 Index = 0; Index < RequestedCameraCount; ++Index)
	{
		const FString ViewportId = FString::Printf(TEXT("camera_%02d"), Index);
		IDisplayClusterViewport* Viewport =
			RenderManager->GetViewportManager()->FindViewport(ViewportId);
		if (!Viewport)
		{
			return false;
		}
		Viewports.Add(Viewport);
	}

	// nDisplay owns camera rendering while this adapter is active. Disable
	// every URLab SceneCapture, including cameras omitted from a partial-view
	// profiling run, so they cannot silently skew its cost.
	for (UMjCamera* Camera : AllCameras)
	{
		if (Camera && Camera->CaptureComponent)
		{
			Camera->CaptureComponent->bCaptureEveryFrame = false;
			Camera->CaptureComponent->bCaptureOnMovement = false;
		}
	}

	CameraProxies.Reserve(RequestedCameraCount);
	for (int32 Index = 0; Index < RequestedCameraCount; ++Index)
	{
		UMjCamera* Source = Cameras[Index];
		if (!Source || !Source->CaptureComponent)
		{
			return false;
		}

		AActor* Owner = Source->GetOwner();
		const FName ProxyName(*FString::Printf(TEXT("URSNDisplayCamera_%02d"), Index));
		UCameraComponent* Proxy = NewObject<UCameraComponent>(Owner, ProxyName);
		Owner->AddInstanceComponent(Proxy);
		Proxy->SetupAttachment(Source->CaptureComponent);
		Proxy->SetRelativeTransform(FTransform::Identity);
		Proxy->FieldOfView = Source->fovy;
		if (Source->resolution.Num() >= 2 && Source->resolution[1] > 0)
		{
			Proxy->AspectRatio =
				static_cast<float>(Source->resolution[0])
				/ static_cast<float>(Source->resolution[1]);
		}
		Proxy->PostProcessSettings = Source->CaptureComponent->PostProcessSettings;
		Proxy->PostProcessBlendWeight = Source->CaptureComponent->PostProcessBlendWeight;
		Proxy->RegisterComponent();

		const FString ViewportId = FString::Printf(TEXT("camera_%02d"), Index);
		FDisplayClusterProjectionCameraPolicySettings Settings;
		Settings.FOVMultiplier = 1.0f;
		Settings.bCameraOverrideDefaults = true;
		if (!IDisplayClusterProjection::Get().CameraPolicySetCamera(
			Viewports[Index]->GetProjectionPolicy(), Proxy, Settings))
		{
			return false;
		}
		CameraProxies.Add(Proxy);

		UE_LOG(LogTemp, Log,
			TEXT("[URS nDisplay] viewport '%s' bound to '%s/%s'."),
			*ViewportId, *Owner->GetName(), *Source->MjName);
	}

	UE_LOG(LogTemp, Log,
		TEXT("[URS nDisplay] bound %d cameras%s; independent SceneCapture rendering disabled."),
		CameraProxies.Num(),
		RequestedCameraName.IsEmpty()
			? TEXT("")
			: *FString::Printf(TEXT(" named '%s'"), *RequestedCameraName));

	if (FParse::Param(FCommandLine::Get(), TEXT("URSNDisplayScreenshot")))
	{
		FString ScreenshotName = TEXT("urs_ndisplay_four_cameras");
		FParse::Value(
			FCommandLine::Get(),
			TEXT("URSNDisplayScreenshotName="),
			ScreenshotName);
		ScreenshotName = FPaths::GetBaseFilename(ScreenshotName);

		double ScreenshotDelay = 2.0;
		FParse::Value(
			FCommandLine::Get(),
			TEXT("URSNDisplayScreenshotDelay="),
			ScreenshotDelay);
		ScreenshotDelay = FMath::Clamp(ScreenshotDelay, 0.1, 120.0);

		FTimerHandle ScreenshotTimer;
		World->GetTimerManager().SetTimer(
			ScreenshotTimer,
			[ScreenshotName]()
			{
				const FString Path = FPaths::Combine(
					FPaths::ProjectSavedDir(),
					TEXT("Screenshots"),
					ScreenshotName + TEXT(".png"));
				FScreenshotRequest::RequestScreenshot(Path, false, false);
			},
			static_cast<float>(ScreenshotDelay),
			false);
	}
	return true;
}

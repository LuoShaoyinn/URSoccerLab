#include "NDisplay/URSDisplayClusterCameraBinderComponent.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Containers/DisplayClusterProjectionCameraPolicySettings.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "IDisplayCluster.h"
#include "IDisplayClusterCallbacks.h"
#include "IDisplayClusterProjection.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "Render/IDisplayClusterRenderManager.h"
#include "Render/Viewport/IDisplayClusterViewport.h"
#include "Render/Viewport/IDisplayClusterViewportManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "Scene/URSSceneConfigComponent.h"
#include "Shader.h"
#include "TimerManager.h"
#include "UnrealClient.h"

struct UURSDisplayClusterCameraBinderComponent::FReadbackSlot
{
	explicit FReadbackSlot(const FName& Name)
		: Readback(MakeUnique<FRHIGPUTextureReadback>(Name))
	{
	}

	TUniquePtr<FRHIGPUTextureReadback> Readback;
	FIntPoint Size = FIntPoint::ZeroValue;
	bool bInFlight = false;
};

UURSDisplayClusterCameraBinderComponent::UURSDisplayClusterCameraBinderComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

UURSDisplayClusterCameraBinderComponent::~UURSDisplayClusterCameraBinderComponent() = default;

void UURSDisplayClusterCameraBinderComponent::BeginPlay()
{
	Super::BeginPlay();

	if (const UURSSceneConfigComponent* SceneConfig =
		GetOwner() ? GetOwner()->FindComponentByClass<UURSSceneConfigComponent>() : nullptr)
	{
		const URSoccerLab::FURSSceneConfig& Config = SceneConfig->GetActiveConfig();
		if (Config.Vision.Mode == URSoccerLab::EURSVisionMode::Rgbd)
		{
			RequestedCameraCount = Config.Robots.Num();
			RequestedCameraName = Config.Vision.LeftCamera;
		}
		else
		{
			RequestedCameraCount = Config.Robots.Num() * 2;
		}
	}

	int32 CommandLineCameraCount = 0;
	if (FParse::Value(
		FCommandLine::Get(),
		TEXT("URSNDisplayCameraCount="),
		CommandLineCameraCount))
	{
		RequestedCameraCount = CommandLineCameraCount;
	}
	RequestedCameraCount = FMath::Clamp(RequestedCameraCount, 1, 20);
	FParse::Value(
		FCommandLine::Get(),
		TEXT("URSNDisplayCameraName="),
		RequestedCameraName);

	ReadbackSlots.Reserve(4);
	for (int32 Index = 0; Index < 4; ++Index)
	{
		ReadbackSlots.Add(MakeShared<FReadbackSlot, ESPMode::ThreadSafe>(
			*FString::Printf(TEXT("URSAtlasReadback_%d"), Index)));
	}
}

void UURSDisplayClusterCameraBinderComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	bReadbackRequested.Store(false);
	if (BackBufferDelegateHandle.IsValid())
	{
		IDisplayCluster::Get().GetCallbacks()
			.OnDisplayClusterPostBackbufferUpdated_RenderThread()
			.Remove(BackBufferDelegateHandle);
		BackBufferDelegateHandle.Reset();
	}
	FlushRenderingCommands();
	ReadbackSlots.Reset();
	Super::EndPlay(EndPlayReason);
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
	}
	DrainCompletedAtlases();
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

	// nDisplay owns RGB rendering while this adapter is active. Keep depth
	// SceneCaptures alive in RGBD mode; depth is still produced independently
	// until it can be extracted from an nDisplay view attachment.
	for (UMjCamera* Camera : AllCameras)
	{
		if (Camera && Camera->CaptureComponent
			&& Camera->CaptureMode != EMjCameraMode::Depth)
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
		CameraRects.Add(
			CameraKey(Owner->GetName(), Source->MjName),
			Viewports[Index]->GetRenderSettings().Rect);

		UE_LOG(LogTemp, Log,
			TEXT("[URS nDisplay] viewport '%s' bound to '%s/%s'."),
			*ViewportId, *Owner->GetName(), *Source->MjName);
	}

	UE_LOG(LogTemp, Log,
		TEXT("[URS nDisplay] bound %d RGB cameras%s; duplicate RGB SceneCaptures disabled."),
		CameraProxies.Num(),
		RequestedCameraName.IsEmpty()
			? TEXT("")
			: *FString::Printf(TEXT(" named '%s'"), *RequestedCameraName));

	// nDisplay resolves its atlas into the output backbuffer after Slate has
	// constructed its ready-to-present RDG passes. Reading from Slate's earlier
	// callback therefore captures the still-cleared (black) buffer. Epic's node
	// media capture uses this post-nDisplay callback for the same reason.
	BackBufferDelegateHandle = IDisplayCluster::Get().GetCallbacks()
		.OnDisplayClusterPostBackbufferUpdated_RenderThread()
		.AddUObject(
			this,
			&UURSDisplayClusterCameraBinderComponent::OnDisplayClusterBackBufferReady_RenderThread);

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

bool UURSDisplayClusterCameraBinderComponent::RequestRgbFrame()
{
	if (!bBound || OutstandingReadbacks.Load() >= ReadbackSlots.Num())
	{
		return false;
	}
	bool bExpected = false;
	return bReadbackRequested.CompareExchange(bExpected, true);
}

FString UURSDisplayClusterCameraBinderComponent::CameraKey(
	const FString& ActorId,
	const FString& CameraName)
{
	return ActorId + TEXT("/") + CameraName;
}

bool UURSDisplayClusterCameraBinderComponent::CopyRgbFrame(
	const FString& ActorId,
	const FString& CameraName,
	const uint64 MinimumSequence,
	TArray<FColor>& OutPixels,
	int32& OutWidth,
	int32& OutHeight,
	uint64& OutSequence) const
{
	if (LatestAtlasSequence <= MinimumSequence || LatestAtlasPixels.IsEmpty())
	{
		return false;
	}
	const FIntRect* Rect = CameraRects.Find(CameraKey(ActorId, CameraName));
	if (!Rect || Rect->Min.X < 0 || Rect->Min.Y < 0
		|| Rect->Max.X > LatestAtlasSize.X || Rect->Max.Y > LatestAtlasSize.Y
		|| Rect->Width() <= 0 || Rect->Height() <= 0)
	{
		return false;
	}

	OutWidth = Rect->Width();
	OutHeight = Rect->Height();
	OutPixels.SetNumUninitialized(OutWidth * OutHeight);
	for (int32 Row = 0; Row < OutHeight; ++Row)
	{
		const FColor* Source =
			LatestAtlasPixels.GetData()
			+ (Rect->Min.Y + Row) * LatestAtlasSize.X
			+ Rect->Min.X;
		FColor* Destination = OutPixels.GetData() + Row * OutWidth;
		FMemory::Memcpy(Destination, Source, OutWidth * sizeof(FColor));
	}
	OutSequence = LatestAtlasSequence;
	return true;
}

void UURSDisplayClusterCameraBinderComponent::DrainCompletedAtlases()
{
	FCompletedAtlas Completed;
	while (CompletedAtlases.Dequeue(Completed))
	{
		if (Completed.Pixels.Num() == Completed.Size.X * Completed.Size.Y)
		{
			LatestAtlasSize = Completed.Size;
			LatestAtlasPixels = MoveTemp(Completed.Pixels);
			++LatestAtlasSequence;
		}
	}
}

void UURSDisplayClusterCameraBinderComponent::OnDisplayClusterBackBufferReady_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	FViewport* Viewport)
{
	check(IsInRenderingThread());

	for (TSharedPtr<FReadbackSlot, ESPMode::ThreadSafe>& Slot : ReadbackSlots)
	{
		if (!Slot->bInFlight || !Slot->Readback->IsReady())
		{
			continue;
		}

		int32 RowPitchPixels = 0;
		int32 BufferHeight = 0;
		const FColor* Source = static_cast<const FColor*>(
			Slot->Readback->Lock(RowPitchPixels, &BufferHeight));
		if (Source && RowPitchPixels >= Slot->Size.X && BufferHeight >= Slot->Size.Y)
		{
			FCompletedAtlas Completed;
			Completed.Size = Slot->Size;
			Completed.Pixels.SetNumUninitialized(Slot->Size.X * Slot->Size.Y);
			for (int32 Row = 0; Row < Slot->Size.Y; ++Row)
			{
				FMemory::Memcpy(
					Completed.Pixels.GetData() + Row * Slot->Size.X,
					Source + Row * RowPitchPixels,
					Slot->Size.X * sizeof(FColor));
			}
			CompletedAtlases.Enqueue(MoveTemp(Completed));
		}
		Slot->Readback->Unlock();
		Slot->bInFlight = false;
		--OutstandingReadbacks;
	}

	FRHITexture* BackBuffer = Viewport ? Viewport->GetRenderTargetTexture() : nullptr;
	if (!BackBuffer || !bReadbackRequested.Exchange(false))
	{
		return;
	}

	FReadbackSlot* FreeSlot = nullptr;
	for (TSharedPtr<FReadbackSlot, ESPMode::ThreadSafe>& Slot : ReadbackSlots)
	{
		if (!Slot->bInFlight)
		{
			FreeSlot = Slot.Get();
			break;
		}
	}
	if (!FreeSlot)
	{
		return;
	}

	const FIntPoint Extent = BackBuffer->GetSizeXY();
	// nDisplay commonly presents through a 10-bit swapchain. Convert it before
	// readback so the CPU-side FColor/JPEG path always receives 8-bit BGRA.
	FRDGBuilder GraphBuilder(RHICmdList);
	FRDGTextureRef Source = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(BackBuffer, TEXT("URS_nDisplayBackBuffer")));
	const FRDGTextureDesc ConvertedDesc = FRDGTextureDesc::Create2D(
		Extent,
		PF_B8G8R8A8,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_RenderTargetable);
	FRDGTextureRef Converted = GraphBuilder.CreateTexture(
		ConvertedDesc,
		TEXT("URS_nDisplayAtlasBGRA8"));
	AddDrawTexturePass(
		GraphBuilder,
		GetGlobalShaderMap(GMaxRHIFeatureLevel),
		Source,
		Converted,
		FRDGDrawTextureInfo());
	AddEnqueueCopyPass(GraphBuilder, FreeSlot->Readback.Get(), Converted);
	GraphBuilder.Execute();
	FreeSlot->Size = Extent;
	FreeSlot->bInFlight = true;
	++OutstandingReadbacks;
}

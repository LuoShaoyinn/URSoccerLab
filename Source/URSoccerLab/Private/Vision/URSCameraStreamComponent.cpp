#include "Vision/URSCameraStreamComponent.h"
#include "Core/URSRobotCoreComponent.h"
#include "Scene/URSSceneConfigComponent.h"
#include "NDisplay/URSDisplayClusterCameraBinderComponent.h"
#include "IImageWrapperModule.h"
#include "Async/Async.h"
#include "Misc/CommandLine.h"
#include "Misc/Compression.h"
#include "HAL/PlatformTime.h"
UURSCameraStreamComponent::UURSCameraStreamComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}
void UURSCameraStreamComponent::BeginPlay()
{
	Super::BeginPlay();
	Mailbox = MakeShared<FMailbox, ESPMode::ThreadSafe>();
	ImageWrapperModule = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	Core = GetOwner()->FindComponentByClass<UURSRobotCoreComponent>();
	NDisplayBinder = GetOwner()->FindComponentByClass<UURSDisplayClusterCameraBinderComponent>();
	if (const auto* Config = GetOwner()->FindComponentByClass<UURSSceneConfigComponent>())
		VisionConfig = Config->GetActiveConfig().Vision;
	CameraRateHz = VisionConfig.Rgb.RateHz;
	if (const auto* Config = GetOwner()->FindComponentByClass<UURSSceneConfigComponent>())
		if (Config->GetActiveConfig().CameraFreq > 0)
			CameraRateHz = Config->GetActiveConfig().CameraFreq;
	CameraCompress = VisionConfig.Rgb.Compression != URSoccerLab::EURSRgbCompression::Raw && VisionConfig.Rgb.Compression != URSoccerLab::EURSRgbCompression::Jpeg ? TEXT("video") : VisionConfig.Rgb.Compression == URSoccerLab::EURSRgbCompression::Jpeg ? TEXT("jpeg") : TEXT("raw");
	JpegQuality = VisionConfig.Rgb.JpegQuality;
	FParse::Value(FCommandLine::Get(), TEXT("URSCameraRateHz="), CameraRateHz);
	CameraRateHz = FMath::Clamp(CameraRateHz, 1.0, 120.0);
	VisionConfig.Rgb.RateHz = CameraRateHz;
	if (Core.IsValid())
		Core->OnRobotsChanged.AddDynamic(this, &UURSCameraStreamComponent::OnRobotsChanged);
	OnRobotsChanged();
}
void UURSCameraStreamComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Core.IsValid())
		Core->OnRobotsChanged.RemoveDynamic(this, &UURSCameraStreamComponent::OnRobotsChanged);
	OnEncodedFrame.Clear();
	for (auto& Job : EncodeJobs)
		Job.Wait();
	EncodeJobs.Empty();
	Mailbox.Reset();
	Super::EndPlay(Reason);
}
void UURSCameraStreamComponent::OnRobotsChanged()
{
	++CaptureGeneration;
	CameraStates.Empty();
	if (Core.IsValid())
		for (const auto& Id : Core->GetRobotIds())
			{
				FCameraState State; State.ActorId = Id;
				if (CameraCompress == TEXT("video")) State.VideoEncoder = MakeShared<URSoccerLab::FVideoEncoder, ESPMode::ThreadSafe>(VisionConfig.Rgb);
				CameraStates.Add(MoveTemp(State));
			}
	NextRgbTimeSec = FPlatformTime::Seconds();
}
void UURSCameraStreamComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                              FActorComponentTickFunction* TickFunction)
{
	Super::TickComponent(DeltaTime, TickType, TickFunction);
	EncodeJobs.RemoveAll([](const TFuture<void>& Job) { return Job.IsReady(); });
	DrainCompletedFrames();
	if (!NDisplayBinder.IsValid())
		NDisplayBinder = GetOwner()->FindComponentByClass<UURSDisplayClusterCameraBinderComponent>();
	TickCameraCapture();
	TickDepthCapture();
}
void UURSCameraStreamComponent::DrainCompletedFrames()
{
	if (!Mailbox)
		return;
	FCompletedFrame Result;
	while (Mailbox->Frames.Dequeue(Result))
	{
		if (Result.Generation != CaptureGeneration)
			continue;
		for (auto& Camera : CameraStates)
			if (Camera.ActorId == Result.Frame.ActorId)
				{
					if (Result.Frame.MessageType == 2) Camera.bDepthEncodeInFlight = false;
					else Camera.bRgbEncodeInFlight = false;
				}
		if (!Result.Frame.Images.IsEmpty())
			OnEncodedFrame.Broadcast(Result.Frame);
	}
}
void UURSCameraStreamComponent::TickCameraCapture()
{
	if (!Core.IsValid() || !bCaptureDemand || !OnEncodedFrame.IsBound())
		return;
	const double Now = FPlatformTime::Seconds();
	const bool bUseNDisplay = NDisplayBinder.IsValid() && NDisplayBinder->IsReady();
	const TArray<FString> RobotIds = Core->GetRobotIds();
	if (RobotIds.Num() == 0)
		return;

	// Request camera readback at CameraRateHz
	const double RgbInterval = CameraRateHz > 0 ? 1.0 / CameraRateHz : 0;
	if (RgbInterval > 0 && Now >= NextRgbTimeSec)
	{
		do
		{
			NextRgbTimeSec += RgbInterval;
		} while (NextRgbTimeSec <= Now);

		if (bUseNDisplay)
		{
			NDisplayBinder->RequestRgbFrame();
		}
		else
		{
			for (const FString& ActorId : RobotIds)
			{
				Core->RequestNamedCameraReadback(ActorId, VisionConfig.LeftCamera);
				if (VisionConfig.Mode == URSoccerLab::EURSVisionMode::StereoRgb)
					Core->RequestNamedCameraReadback(ActorId, VisionConfig.RightCamera);
			}
		}
	}

	// Consume ready camera frames and submit bounded encoder jobs.
	// nDisplay and direct URLab readbacks share the same encoder.
	for (int32 Ri = 0; Ri < RobotIds.Num(); ++Ri)
	{
		if (Ri >= CameraStates.Num())
			break;
		if (CameraStates[Ri].bRgbEncodeInFlight || Now < CameraStates[Ri].NextEncodeTime) continue;
		const FString& ActorId = RobotIds[Ri];
		FURSRobotState State;
		if (!Core->GetRobotState(ActorId, State))
			continue;

		TArray<FString> CamNames = {VisionConfig.LeftCamera};
		if (VisionConfig.Mode == URSoccerLab::EURSVisionMode::StereoRgb)
			CamNames.Add(VisionConfig.RightCamera);

		const uint64 PreviousNDisplaySequence = CameraStates[Ri].LastNDisplayRgbSequence;
		const uint64 LatestNDisplaySequence = bUseNDisplay ? NDisplayBinder->GetLatestRgbFrameSequence() : 0;
		if (bUseNDisplay && LatestNDisplaySequence <= PreviousNDisplaySequence)
			continue;

		if (!bUseNDisplay)
		{
			bool bAllReady = true;
			for (const FString& CameraName : CamNames)
				bAllReady = bAllReady && Core->IsCameraFrameReady(ActorId, CameraName);
			if (!bAllReady)
				continue;
		}

		// Consume pixels for all cameras
		TArray<URSoccerLab::FRawCameraImage> Images;
		bool bValid = true;
		for (const FString& Cn : CamNames)
		{
			URSoccerLab::FRawCameraImage Img;
			Img.Name = Cn;
			uint64 ImageNDisplaySequence = 0;
			const bool bGotPixels =
			    bUseNDisplay ? NDisplayBinder->CopyRgbFrame(ActorId, Cn, PreviousNDisplaySequence, Img.Pixels,
				                                            Img.Width, Img.Height, ImageNDisplaySequence)
				             : Core->ConsumeCameraFrame(ActorId, Cn, Img.Pixels);
			if (bGotPixels)
			{
				if (Img.Pixels.Num() > 0)
				{
					if (!bUseNDisplay)
					{
						for (const FURSCameraInfo& Camera : State.Cameras)
						{
							if (Camera.Name == Cn)
							{
								Img.Width = Camera.Width;
								Img.Height = Camera.Height;
								break;
							}
						}
					}
				}
				if (bUseNDisplay && ImageNDisplaySequence != LatestNDisplaySequence)
					bValid = false;
				Images.Add(MoveTemp(Img));
			}
			else
			{
				bValid = false;
				break;
			}
		}

		if (!bValid || Images.Num() != CamNames.Num())
			continue;
		if (bUseNDisplay)
			CameraStates[Ri].LastNDisplayRgbSequence = LatestNDisplaySequence;

		// Bounded asynchronous encode: at most one in-flight RGB job per robot.
		// If the previous encode has not finished, this capture opportunity is
		// dropped instead of accumulating a queue.
		if (CameraStates[Ri].bRgbEncodeInFlight)
			continue;

		if (CameraStates[Ri].NextEncodeTime == 0) CameraStates[Ri].NextEncodeTime = Now;
		do { CameraStates[Ri].NextEncodeTime += RgbInterval; } while (CameraStates[Ri].NextEncodeTime <= Now);
		const uint32 Seq = Sequence++;
		const uint32 Gen = CaptureGeneration;
		const double SimTime = State.SimTime;
		const auto VideoEncoder = CameraStates[Ri].VideoEncoder;
		const bool bJpeg = CameraCompress == TEXT("jpeg");
		const int32 Quality = JpegQuality;
		IImageWrapperModule* Module = ImageWrapperModule;
		auto CompletionMailbox = Mailbox;
		CameraStates[Ri].bRgbEncodeInFlight = true;
		EncodeJobs.Add(Async(EAsyncExecution::ThreadPool, [CompletionMailbox, Gen, Seq, SimTime, ActorId, VideoEncoder, bJpeg,
		                                                   Quality, Module, Images = MoveTemp(Images)]() {
			FCompletedFrame Result;
			Result.Generation = Gen;
			Result.Frame.ActorId = ActorId;
			Result.Frame.Sequence = Seq;
			Result.Frame.SimTime = SimTime;
			if (VideoEncoder)
			{
				if (!VideoEncoder->Encode(Images, Result.Frame)) Result.Frame.Images.Empty();
			}
			else for (const auto& Image : Images)
			{
				URSoccerLab::FEncodedCameraImage Encoded;
				if (!URSoccerLab::FImageEncoder::Encode(Image, bJpeg, Quality, *Module, Encoded))
				{
					Result.Frame.Images.Empty();
					break;
				}
				Result.Frame.Images.Add(MoveTemp(Encoded));
			}
			CompletionMailbox->Frames.Enqueue(MoveTemp(Result));
		}));
	}
}

bool UURSCameraStreamComponent::IsLayoutReady() const
{
	// nDisplay updates camera metadata once its viewports are bound. Consumers
	// should subscribe only after that layout is stable, avoiding startup reconnects.
	if (FParse::Param(FCommandLine::Get(), TEXT("URSNDisplayCameras")))
		return NDisplayBinder.IsValid() && NDisplayBinder->IsReady();
	return true;
}

void UURSCameraStreamComponent::TickDepthCapture()
{
 if (!Core.IsValid() || !bCaptureDemand || VisionConfig.Mode != URSoccerLab::EURSVisionMode::Rgbd) return;
 const double Now = FPlatformTime::Seconds();
 if (Now >= NextDepthTime)
 {
  NextDepthTime = Now + 1.0 / VisionConfig.Depth.RateHz;
  for (const auto& Camera : CameraStates) Core->RequestNamedCameraReadback(Camera.ActorId, VisionConfig.RightCamera);
 }
 for (auto& Camera : CameraStates)
 {
  if (Camera.bDepthEncodeInFlight || !Core->IsCameraFrameReady(Camera.ActorId, VisionConfig.RightCamera)) continue;
  TArray<float> Depth; int32 W = 0, H = 0;
  if (!Core->ConsumeDepthCameraFrame(Camera.ActorId, VisionConfig.RightCamera, Depth)) continue;
  FURSRobotState State;
  if (!Core->GetRobotState(Camera.ActorId, State)) continue;
  for (const auto& Info : State.Cameras) if (Info.Name == VisionConfig.RightCamera) { W = Info.Width; H = Info.Height; }
  if (W <= 0 || H <= 0 || Depth.Num() != W*H) continue;
  Camera.bDepthEncodeInFlight = true;
  const auto Outbox = Mailbox; const auto Settings = VisionConfig.Depth;
  const FString ActorId = Camera.ActorId, Name = VisionConfig.RightCamera;
  const uint32 Gen = CaptureGeneration, Seq = Sequence++;
  const double SimTime = State.SimTime;
  EncodeJobs.Add(Async(EAsyncExecution::ThreadPool, [Outbox, Settings, ActorId, Name, Gen, Seq, SimTime, W, H, Depth = MoveTemp(Depth)] {
   FCompletedFrame Result; Result.Generation = Gen;
   Result.Frame.ActorId = ActorId; Result.Frame.MessageType = 2;
   Result.Frame.Sequence = Seq; Result.Frame.SimTime = SimTime;
   URSoccerLab::FEncodedCameraImage Image; Image.Name = Name; Image.Width = W; Image.Height = H;
   if (Settings.Compression == URSoccerLab::EURSDepthCompression::RawFloat32)
   {
    Image.PixelFormat = 1; Image.Data.Append(reinterpret_cast<const uint8*>(Depth.GetData()), Depth.Num()*sizeof(float));
    Image.RawLength = Image.Data.Num();
   }
   else
   {
    Image.PixelFormat = 2; TArray<uint16> Millimeters; Millimeters.SetNumUninitialized(Depth.Num());
    for (int32 I = 0; I < Depth.Num(); ++I)
     Millimeters[I] = FMath::IsFinite(Depth[I]) && Depth[I] > 0 && Depth[I] <= Settings.MaxDepthMeters
       ? uint16(FMath::Clamp(FMath::RoundToInt(Depth[I]*1000), 1, 65535)) : 0;
    Image.RawLength = Millimeters.Num()*sizeof(uint16);
    if (Settings.Compression == URSoccerLab::EURSDepthCompression::ZlibUint16Millimeters)
    {
     Image.Codec = 2; int32 Length = FCompression::CompressMemoryBound(NAME_Zlib, Image.RawLength);
     Image.Data.SetNumUninitialized(Length);
     if (FCompression::CompressMemory(NAME_Zlib, Image.Data.GetData(), Length, Millimeters.GetData(), Image.RawLength))
      Image.Data.SetNum(Length);
     else Image.Data.Empty();
    }
    else Image.Data.Append(reinterpret_cast<const uint8*>(Millimeters.GetData()), Image.RawLength);
   }
   if (!Image.Data.IsEmpty()) Result.Frame.Images.Add(MoveTemp(Image));
   Outbox->Frames.Enqueue(MoveTemp(Result));
  }));
 }
}

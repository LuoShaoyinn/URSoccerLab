#include "Transport/URSTcpTransportComponent.h"
#include "ImageCore.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "Core/URSRobotCoreComponent.h"
#include "NDisplay/URSDisplayClusterCameraBinderComponent.h"
#include "Scene/URSSceneConfigComponent.h"
#include "Network/URSNetworkThread.h"
#include "Network/URSJson.h"
#include "Transport/URSTcpProtocol.h"
#include "Runtime/URSAdminProtocol.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Async/Async.h"
#include "Misc/CommandLine.h"
#include "HAL/PlatformTime.h"

UURSTcpTransportComponent::UURSTcpTransportComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UURSTcpTransportComponent::BeginPlay()
{
	Super::BeginPlay();
	ImageWrapperModule = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

	if (AActor* Owner = GetOwner())
	{
		if (const UURSSceneConfigComponent* SceneConfig = Owner->FindComponentByClass<UURSSceneConfigComponent>())
			VisionConfig = SceneConfig->GetActiveConfig().Vision;

		Core = Owner->FindComponentByClass<UURSRobotCoreComponent>();
		NDisplayBinder = Owner->FindComponentByClass<UURSDisplayClusterCameraBinderComponent>();
		if (Core.IsValid())
			Core->OnRobotsChanged.AddDynamic(this, &UURSTcpTransportComponent::OnRobotsChanged);
	}

	CameraRateHz = VisionConfig.Rgb.RateHz;
	CameraCompress = VisionConfig.Rgb.Compression == URSoccerLab::EURSRgbCompression::Jpeg ? TEXT("jpeg") : TEXT("raw");
	JpegQuality = VisionConfig.Rgb.JpegQuality;
	DepthRateHz = VisionConfig.Depth.RateHz;

	double RequestedCameraRateHz = CameraRateHz;
	if (FParse::Value(FCommandLine::Get(), TEXT("URSCameraRateHz="), RequestedCameraRateHz))
		CameraRateHz = FMath::Clamp(RequestedCameraRateHz, 1.0, 120.0);

	UE_LOG(LogTemp, Log, TEXT("[URS TCP] Vision: mode=stereo_rgb rgb=%.0fHz/%s(q=%d) depth=%.0fHz/%s"),
		CameraRateHz, *CameraCompress, JpegQuality, DepthRateHz, *DepthCompress);

	if (bAutoStart) StartTransport();
}

void UURSTcpTransportComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopTransport();
	Super::EndPlay(EndPlayReason);
}

bool UURSTcpTransportComponent::StartTransport()
{
	// Admin listener on game thread
	auto* SSS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	AdminListenerSock = SSS->CreateSocket(NAME_Stream, TEXT("URS-Admin"), false);
	if (AdminListenerSock)
	{
		AdminListenerSock->SetReuseAddr();
		AdminListenerSock->SetNonBlocking(true);
		TSharedRef<FInternetAddr> Addr = SSS->GetLocalBindAddr(*GLog);
		Addr->SetPort(AdminPort);
		if (AdminListenerSock->Bind(*Addr) && AdminListenerSock->Listen(16))
			UE_LOG(LogTemp, Log, TEXT("[URS TCP] Admin listening on port %d"), AdminPort);
	}

	RebuildNetworkThread();
	return true;
}

void UURSTcpTransportComponent::StopTransport()
{
	bVisionAccept.store(false);
	if (NetThread)
	{
		NetThread->Stop();
		delete NetThread; NetThread = nullptr;
	}
	CloseSocket(AdminListenerSock);
	for (auto& C : AdminClients)
		CloseSocket(C.Socket);
	AdminClients.Empty();
}

void UURSTcpTransportComponent::RebuildNetworkThread()
{
	if (!Core.IsValid()) return;

	TArray<FString> RobotIds = Core->GetRobotIds();
	if (RobotIds.Num() == 0) return;
	if (FParse::Param(FCommandLine::Get(), TEXT("URSNDisplayCameras"))
		&& (!NDisplayBinder.IsValid() || !NDisplayBinder->IsReady()))
	{
		// URLab can build the compiled robot model before the nDisplay binder
		// has finished binding its camera viewports. Do not expose a listener
		// that will immediately be torn down by the subsequent rebuild.
		UE_LOG(LogTemp, Verbose, TEXT("[URS TCP] Deferring network startup until nDisplay is ready."));
		return;
	}

	// Stop existing network thread
	if (NetThread) { NetThread->Stop(); delete NetThread; NetThread = nullptr; }

	// Build network thread endpoints
	TArray<URSNetworkThread::FRobotEndpoint> NetEndpoints;
	FScopeLock CoreEndpointLock(&Core->EndpointMutex);
	auto& CoreEndpoints = Core->GetEndpoints();

	for (int32 Ri = 0; Ri < CoreEndpoints.Num(); ++Ri)
	{
		URSNetworkThread::FRobotEndpoint NE;
		NE.ActorId = CoreEndpoints[Ri].ActorId;

		// Wire triple buffer pointers
		NE.StateBuf = CoreEndpoints[Ri].StateBuffer;
		NE.CmdBuf = CoreEndpoints[Ri].CmdBuffer;
		NE.GainBuf = CoreEndpoints[Ri].GainBuffer;

		// Build metadata for JSON
		for (const auto& Ji : CoreEndpoints[Ri].Joints)
			if (Ji.JointType != mjJNT_FREE)
				NE.Meta.JointNames.Add(Ji.Name);
		for (const auto& Ai : CoreEndpoints[Ri].Actuators)
			NE.Meta.ActuatorNames.Add(Ai.Name);
		for (const auto& Ce : CoreEndpoints[Ri].Cameras)
		{
			if (auto* Cam = Ce.Camera.Get())
			{
				NE.Meta.CameraNames.Add(Ce.Name);
				NE.Meta.CameraWidths.Add(Cam->resolution.Num() > 0 ? Cam->resolution[0] : 0);
				NE.Meta.CameraHeights.Add(Cam->resolution.Num() > 1 ? Cam->resolution[1] : 0);
				NE.Meta.CameraFormats.Add(Cam->CaptureMode == EMjCameraMode::Depth ? TEXT("float32_depth") : TEXT("bgra8"));
			}
		}

		NE.Meta.bPrivSelfPos = CoreEndpoints[Ri].Privilege.bSelfPos;
		NE.Meta.bPrivBallPosRelated = CoreEndpoints[Ri].Privilege.bBallPosRelated;
		NE.Meta.bPrivBallVelRelated = CoreEndpoints[Ri].Privilege.bBallVelRelated;
		NE.Meta.bPrivAllPos = CoreEndpoints[Ri].Privilege.bAllPos;
		NE.Meta.Noise = { CoreEndpoints[Ri].Noise.Qpos, CoreEndpoints[Ri].Noise.Qvel, CoreEndpoints[Ri].Noise.Qtor,
			CoreEndpoints[Ri].Noise.ImuQuat, CoreEndpoints[Ri].Noise.ImuAngVel,
			CoreEndpoints[Ri].Noise.CameraImuQuat, CoreEndpoints[Ri].Noise.CameraImuAngVel,
			CoreEndpoints[Ri].Noise.SelfPos, CoreEndpoints[Ri].Noise.BallPosRelated,
			CoreEndpoints[Ri].Noise.BallVelRelated, CoreEndpoints[Ri].Noise.AllPos };

		// Open TCP listener for this robot
		int32 Port = RobotBasePort + Ri;
		NE.Listener.Listen(Port);
		UE_LOG(LogTemp, Log, TEXT("[URS TCP] Robot '%s' listening on port %d"), *NE.ActorId, Port);

		NetEndpoints.Add(MoveTemp(NE));
	}

	// Start network thread
	++NetworkGeneration; // invalidate in-flight encode jobs from the prior layout
	NetThread = new URSNetworkThread();
	NetThread->Start(MoveTemp(NetEndpoints), AdminPort, StateRateHz, CameraRateHz);

	// Build camera state for game thread
	CameraStates.Empty();
	for (int32 Ri = 0; Ri < CoreEndpoints.Num(); ++Ri)
		CameraStates.Add({ CoreEndpoints[Ri].ActorId, false });
}

void UURSTcpTransportComponent::OnRobotsChanged()
{
	RebuildNetworkThread();
}

void UURSTcpTransportComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFn)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFn);
	if (!NetThread && Core.IsValid() && Core->GetRobotIds().Num() > 0)
		RebuildNetworkThread();
	TickAdmin();
	TickCameraCapture();
	DrainCompletedVision();
}

// ============================================================================
// Camera capture (game thread only — uses UE render API)
// ============================================================================

void UURSTcpTransportComponent::TickCameraCapture()
{
	if (!Core.IsValid() || !NetThread) return;
	const double Now = FPlatformTime::Seconds();
	const bool bUseNDisplay = NDisplayBinder.IsValid() && NDisplayBinder->IsReady();
	const TArray<FString> RobotIds = Core->GetRobotIds();
	if (RobotIds.Num() == 0) return;

	// Request camera readback at CameraRateHz
	const double RgbInterval = CameraRateHz > 0 ? 1.0 / CameraRateHz : 0;
	if (RgbInterval > 0 && Now >= NextRgbTimeSec)
	{
		do { NextRgbTimeSec += RgbInterval; } while (NextRgbTimeSec <= Now);

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

	// Consume ready camera frames, encode, and hand complete packets to the
	// network thread. nDisplay and direct URLab readbacks share this encoder.
	for (int32 Ri = 0; Ri < RobotIds.Num(); ++Ri)
	{
		if (Ri >= CameraStates.Num()) break;
		const FString& ActorId = RobotIds[Ri];
		FURSRobotState State;
		if (!Core->GetRobotState(ActorId, State)) continue;

		TArray<FString> CamNames = { VisionConfig.LeftCamera };
		if (VisionConfig.Mode == URSoccerLab::EURSVisionMode::StereoRgb)
			CamNames.Add(VisionConfig.RightCamera);

		const uint64 PreviousNDisplaySequence =
			CameraStates[Ri].LastNDisplayRgbSequence;
		const uint64 LatestNDisplaySequence = bUseNDisplay
			? NDisplayBinder->GetLatestRgbFrameSequence() : 0;
		if (bUseNDisplay && LatestNDisplaySequence <= PreviousNDisplaySequence)
			continue;

		if (!bUseNDisplay)
		{
			bool bAllReady = true;
			for (const FString& CameraName : CamNames)
				bAllReady = bAllReady && Core->IsCameraFrameReady(ActorId, CameraName);
			if (!bAllReady) continue;
		}

		// Consume pixels for all cameras
		struct FRawImage {
			TArray<FColor> Pixels;
			int32 Width = 0, Height = 0;
			FString Name;
		};
		TArray<FRawImage> Images;
		bool bValid = true;
		for (const FString& Cn : CamNames)
		{
			FRawImage Img;
			Img.Name = Cn;
			uint64 ImageNDisplaySequence = 0;
			const bool bGotPixels = bUseNDisplay
				? NDisplayBinder->CopyRgbFrame(
					ActorId, Cn, PreviousNDisplaySequence, Img.Pixels,
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
			else { bValid = false; break; }
		}

		if (!bValid || Images.Num() != CamNames.Num()) continue;
		if (bUseNDisplay)
			CameraStates[Ri].LastNDisplayRgbSequence = LatestNDisplaySequence;

		// Bounded asynchronous encode: at most one in-flight RGB job per robot.
		// If the previous encode has not finished, this capture opportunity is
		// dropped instead of accumulating a queue.
		if (CameraStates[Ri].bRgbEncodeInFlight)
			continue;

		static uint32 GlobalSeq = 0;
		const uint32 Seq = GlobalSeq++;
		const uint32 Gen = NetworkGeneration;
		const double SimTime = State.SimTime;
		const FString Compress = CameraCompress;
		const int32 Quality = JpegQuality;
		IImageWrapperModule* LocalImageWrapper = ImageWrapperModule;
		TWeakObjectPtr<UURSTcpTransportComponent> WeakThis(this);

		CameraStates[Ri].bRgbEncodeInFlight = true;

		const uint8 ImageCount = Images.Num();
		Async(EAsyncExecution::ThreadPool, [WeakThis, Ri, Gen, Seq, SimTime, Compress, Quality, LocalImageWrapper, ImageCount, Images = MoveTemp(Images)]()
		{
			TArray<uint8> Payload;
			Payload.Add(0x02); // version
			Payload.Add(ImageCount);
			const uint16 Flags = 0;
			Payload.Append((uint8*)&Flags, 2);
			Payload.Append((uint8*)&Seq, 4);
			Payload.Append((uint8*)&SimTime, 8);

			for (const FRawImage& Img : Images)
			{
				FTCHARToUTF8 NameConv(*Img.Name);
				uint8 NameLen = (uint8)NameConv.Length();

				uint32 RawLen = Img.Pixels.Num() * 4;
				uint32 DataLen = RawLen;
				const uint8* DataPtr = (const uint8*)Img.Pixels.GetData();
				uint8 Codec = 0x00; // raw

				bool bPixelsValid = false;
				if (Img.Pixels.Num() == Img.Width * Img.Height && Img.Pixels.Num() > 0)
				{
					bPixelsValid = true;
					int32 NonZero = 0;
					for (int32 i = 0; i < FMath::Min(100, Img.Pixels.Num()); ++i)
						if (Img.Pixels[i].DWColor() != 0) { ++NonZero; break; }
					if (NonZero == 0) bPixelsValid = false;
				}

				TArray64<uint8> JpegData;
				if (Compress == TEXT("jpeg") && LocalImageWrapper && bPixelsValid)
				{
					FImageView View(Img.Pixels.GetData(), Img.Width, Img.Height);
					if (LocalImageWrapper->CompressImage(JpegData, EImageFormat::JPEG, View, Quality)
						&& JpegData.Num() > 2 && JpegData[0] == 0xFF && JpegData[1] == 0xD8)
					{
						DataLen = JpegData.Num();
						DataPtr = JpegData.GetData();
						Codec = 0x01;
					}
				}

				Payload.Add(NameLen);
				Payload.Append((uint8*)NameConv.Get(), NameLen);
				Payload.Add(Codec);
				Payload.Add(0x00); // pixel format BGRA8
				Payload.Add(0x00); // reserved
				uint16 W = (uint16)Img.Width, H = (uint16)Img.Height;
				Payload.Append((uint8*)&W, 2);
				Payload.Append((uint8*)&H, 2);
				Payload.Append((uint8*)&RawLen, 4);
				Payload.Append((uint8*)&DataLen, 4);
				Payload.Append(DataPtr, DataLen);
			}

			if (UURSTcpTransportComponent* Self = WeakThis.Get())
			{
				FCompletedVisionPacket Pkt;
				Pkt.RobotIdx = Ri;
				Pkt.FrameType = URSoccerLab::TcpProtocol::TypeRgb;
				Pkt.Generation = Gen;
				Pkt.Payload = MoveTemp(Payload);
				Self->CompletedVisionPackets.Enqueue(MoveTemp(Pkt));
			}
		});
	}
}

void UURSTcpTransportComponent::DrainCompletedVision()
{
	// Hand off async-encoded vision packets to the network thread. Runs on the
	// game thread each tick; the encode jobs enqueue results on the pool.
	if (!NetThread) return;
	FCompletedVisionPacket Pkt;
	while (CompletedVisionPackets.Dequeue(Pkt))
	{
		// Discard completions from a prior network configuration: after a
		// rebuild the numeric RobotIdx may map to a different robot (or be out
		// of range), which would misroute the stale frame.
		if (Pkt.Generation != NetworkGeneration)
			continue;
		if (Pkt.RobotIdx >= 0 && Pkt.RobotIdx < CameraStates.Num())
			CameraStates[Pkt.RobotIdx].bRgbEncodeInFlight = false;
		NetThread->EnqueueCameraFrame(Pkt.RobotIdx, Pkt.FrameType,
			Pkt.Payload.GetData(), Pkt.Payload.Num());
	}
}

// ============================================================================
// Admin (game thread — needs CallbackMutex for SetPose etc.)
// ============================================================================

void UURSTcpTransportComponent::TickAdmin()
{
	if (!AdminListenerSock) return;

	// Accept new admin connections
	auto* SSS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	bool bPending = false;
	AdminListenerSock->HasPendingConnection(bPending);
	while (bPending)
	{
		TSharedRef<FInternetAddr> Remote = SSS->CreateInternetAddr();
		FSocket* ClientSock = AdminListenerSock->Accept(*Remote, TEXT("URS-Admin-C"));
		if (ClientSock)
		{
			ClientSock->SetNonBlocking(true);
			FAdminClient NewClient;
			NewClient.Socket = ClientSock;
			AdminClients.Add(MoveTemp(NewClient));
			UE_LOG(LogTemp, Warning, TEXT("[ADMIN] Client accepted, total=%d"), AdminClients.Num());
		}
		AdminListenerSock->HasPendingConnection(bPending);
	}

	// Process admin clients
	uint8 RecvBuf[65536];
	for (int32 Ci = AdminClients.Num() - 1; Ci >= 0; --Ci)
	{
		auto& Client = AdminClients[Ci];
		int32 BytesRead = 0;
		bool bGotData = false;
		while (Client.Socket->Recv(RecvBuf, sizeof(RecvBuf), BytesRead))
		{
			if (BytesRead > 0)
			{
				Client.ReadBuf.Append(RecvBuf, BytesRead);
				bGotData = true;
			}
			else break;
		}
		if (bGotData)
		{
			UE_LOG(LogTemp, Warning, TEXT("[ADMIN] Recv %d bytes, buf=%d"),
				Client.ReadBuf.Num(), Client.ReadBuf.Num());
		}
		if (Client.Socket->GetConnectionState() == SCS_ConnectionError)
		{
			CloseSocket(Client.Socket);
			AdminClients.RemoveAt(Ci);
			continue;
		}

		// Parse frames
		int32 Consumed = 0;
		while (Client.ReadBuf.Num() - Consumed >= 5)
		{
			const uint8* D = Client.ReadBuf.GetData() + Consumed;
			int32 FrameLen = (D[0] << 24) | (D[1] << 16) | (D[2] << 8) | D[3];
			if (FrameLen < 1 || FrameLen > 16*1024*1024) { CloseSocket(Client.Socket); AdminClients.RemoveAt(Ci); break; }
			if (Client.ReadBuf.Num() - Consumed < 4 + FrameLen) break;

			if (D[4] == 0x00) // JSON
			{
				FUTF8ToTCHAR Conv((const ANSICHAR*)(D + 5), FrameLen - 1);
				FString JsonStr(Conv.Length(), Conv.Get());
				ProcessAdminJson(Client, JsonStr);
			}
			Consumed += 4 + FrameLen;
		}
		if (Consumed > 0) Client.ReadBuf.RemoveAt(0, Consumed);

		// Flush writes
		if (Client.WriteBuf.Num() > 0)
		{
			int32 Sent = 0;
			Client.Socket->Send(Client.WriteBuf.GetData(), Client.WriteBuf.Num(), Sent);
			if (Sent > 0) Client.WriteBuf.RemoveAt(0, Sent);
		}
	}
}

void UURSTcpTransportComponent::ProcessAdminJson(FAdminClient& Client, const FString& JsonStr)
{
	auto SendReply = [&](const FString& ReplyStr)
	{
		FTCHARToUTF8 Utf8(*ReplyStr);
		int32 FrameLen = 1 + Utf8.Length();
		Client.WriteBuf.Add((FrameLen >> 24) & 0xFF);
		Client.WriteBuf.Add((FrameLen >> 16) & 0xFF);
		Client.WriteBuf.Add((FrameLen >> 8) & 0xFF);
		Client.WriteBuf.Add(FrameLen & 0xFF);
		Client.WriteBuf.Add(0x00);
		Client.WriteBuf.Append((const uint8*)Utf8.Get(), Utf8.Length());
	};

	using namespace URSoccerLab;

	FAdminPoseRequest Req;
	const EAdminRequestParse Parse = FAdminProtocol::ParseRequest(JsonStr, Req);
	if (Parse != EAdminRequestParse::Accepted)
	{
		SendReply(FAdminProtocol::BuildErrorReply(
			FAdminProtocol::CommandName(Req.Op), FAdminProtocol::LexToString(Parse), TEXT("")));
		return;
	}

	const FString CmdName = FAdminProtocol::CommandName(Req.Op);

	if (!Core.IsValid())
	{
		SendReply(FAdminProtocol::BuildErrorReply(CmdName, TEXT("not_ready"), TEXT("Core unavailable")));
		return;
	}

	const FVector* Trans = Req.TranslationMeters.IsSet() ? &Req.TranslationMeters.GetValue() : nullptr;
	const FQuat* Rot = Req.RotationQuatXyzw.IsSet() ? &Req.RotationQuatXyzw.GetValue() : nullptr;
	const TArray<float>* Jq = Req.JointQpos.IsSet() ? &Req.JointQpos.GetValue() : nullptr;

	switch (Req.Op)
	{
	case EAdminOp::SetPose:
	{
		FURSPoseResult R = Core->SetPose(Req.ActorId, Trans, Rot, Jq);
		if (R.bOk)
			SendReply(FAdminProtocol::BuildOkSetPoseReply(Req.ActorId, R.AppliedTranslation, R.AppliedRotation, R.AppliedJointQpos, R.SimTime));
		else
			SendReply(FAdminProtocol::BuildErrorReply(CmdName, R.Error, R.Message));
		break;
	}
	case EAdminOp::GetPose:
	{
		FURSPoseResult R = Core->GetPose(Req.ActorId);
		if (R.bOk)
			SendReply(FAdminProtocol::BuildOkGetPoseReply(Req.ActorId, R.AppliedTranslation, R.AppliedRotation, R.AppliedJointQpos, R.SimTime));
		else
			SendReply(FAdminProtocol::BuildErrorReply(CmdName, R.Error, R.Message));
		break;
	}
	case EAdminOp::Reset:
	{
		FURSPoseResult R = Core->ResetRobot(Req.ActorId);
		if (R.bOk) SendReply(FAdminProtocol::BuildOkReply(CmdName, Req.ActorId));
		else       SendReply(FAdminProtocol::BuildErrorReply(CmdName, R.Error, R.Message));
		break;
	}
	case EAdminOp::LockPose:
	{
		FURSPoseResult R = Core->SetPoseLock(Req.ActorId, true, Trans, Rot, Jq);
		if (R.bOk) SendReply(FAdminProtocol::BuildOkReply(CmdName, Req.ActorId));
		else       SendReply(FAdminProtocol::BuildErrorReply(CmdName, R.Error, R.Message));
		break;
	}
	case EAdminOp::UnlockPose:
	{
		FURSPoseResult R = Core->SetPoseLock(Req.ActorId, false);
		if (R.bOk) SendReply(FAdminProtocol::BuildOkReply(CmdName, Req.ActorId));
		else       SendReply(FAdminProtocol::BuildErrorReply(CmdName, R.Error, R.Message));
		break;
	}
	default:
		SendReply(FAdminProtocol::BuildErrorReply(CmdName, TEXT("unknown_command"), TEXT("")));
		break;
	}
}

void UURSTcpTransportComponent::CloseSocket(FSocket* Sock)
{
	if (Sock)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Sock);
	}
}

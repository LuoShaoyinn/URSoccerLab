#include "Transport/URSTcpTransportComponent.h"
#include "ImageCore.h"
#include "MuJoCo/Components/Sensors/MjCamera.h"
#include "Core/URSRobotCoreComponent.h"
#include "NDisplay/URSDisplayClusterCameraBinderComponent.h"
#include "Scene/URSSceneConfigComponent.h"
#include "Network/URSNetworkThread.h"
#include "Network/URSJson.h"
#include "Transport/URSTcpProtocol.h"

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

	// Stop existing network thread
	if (NetThread) { NetThread->Stop(); delete NetThread; NetThread = nullptr; }

	// Build network thread endpoints
	TArray<URSNetworkThread::FRobotEndpoint> NetEndpoints;
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

	// Request camera readback at CameraRateHz
	const double RgbInterval = CameraRateHz > 0 ? 1.0 / CameraRateHz : 0;
	if (RgbInterval > 0 && Now >= NextRgbTimeSec)
	{
		do { NextRgbTimeSec += RgbInterval; } while (NextRgbTimeSec <= Now);

		auto& Endpoints = Core->GetEndpoints();
		for (int32 Ri = 0; Ri < Endpoints.Num(); ++Ri)
		{
			if (Ri >= CameraStates.Num()) break;
			if (!bUseNDisplay)
			{
				Core->RequestNamedCameraReadback(Endpoints[Ri].ActorId, VisionConfig.LeftCamera);
				if (VisionConfig.Mode == URSoccerLab::EURSVisionMode::StereoRgb)
					Core->RequestNamedCameraReadback(Endpoints[Ri].ActorId, VisionConfig.RightCamera);
			}
		}
	}

	// Consume ready camera frames → encode → push to network thread
	if (bUseNDisplay) return; // nDisplay path handles its own encoding

	auto& Endpoints = Core->GetEndpoints();
	for (int32 Ri = 0; Ri < Endpoints.Num(); ++Ri)
	{
		TArray<FString> CamNames = { VisionConfig.LeftCamera };
		if (VisionConfig.Mode == URSoccerLab::EURSVisionMode::StereoRgb)
			CamNames.Add(VisionConfig.RightCamera);

		bool bAllReady = true;
		for (const FString& Cn : CamNames)
			bAllReady = bAllReady && Core->IsCameraFrameReady(Endpoints[Ri].ActorId, Cn);

		if (!bAllReady) continue;

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
			if (Core->ConsumeCameraFrame(Endpoints[Ri].ActorId, Cn, Img.Pixels))
			{
				if (Img.Pixels.Num() > 0)
				{
					Img.Width = FMath::Sqrt((float)Img.Pixels.Num() * (4.0f / 3.0f)); // estimate
					Img.Height = Img.Pixels.Num() / FMath::Max(1, Img.Width);
					// Better: get from camera resolution
					for (const auto& Ce : Endpoints[Ri].Cameras)
					{
						if (Ce.Name == Cn && Ce.Camera.IsValid())
						{
							Img.Width = Ce.Camera->resolution.Num() > 0 ? Ce.Camera->resolution[0] : 0;
							Img.Height = Ce.Camera->resolution.Num() > 1 ? Ce.Camera->resolution[1] : 0;
							break;
						}
					}
				}
				Images.Add(MoveTemp(Img));
			}
			else { bValid = false; break; }
		}

		if (!bValid || Images.Num() != CamNames.Num()) continue;

		// Build v2 image message and encode (async)
		const uint8 ImageMsgVersion = 0x02;
		const uint8 ImageCount = Images.Num();
		const uint16 Flags = 0;
		const uint32 Sequence = 0; // TODO: track per-robot sequence
		static uint32 GlobalSeq = 0;
		uint32 Seq = GlobalSeq++;

		// For now, encode synchronously (simpler — async can be re-added)
		TArray<uint8> Payload;
		Payload.Add(ImageMsgVersion);
		Payload.Add(ImageCount);
		Payload.Append((uint8*)&Flags, 2);
		Payload.Append((uint8*)&Seq, 4);
		// sim_time from latest state snapshot
		double SimTime = Endpoints[Ri].StateBuffer->Front().SimTime;
		Payload.Append((uint8*)&SimTime, 8);

		for (const FRawImage& Img : Images)
		{
			FTCHARToUTF8 NameConv(*Img.Name);
			uint8 NameLen = (uint8)NameConv.Length();

			// Encode JPEG first (if requested)
			uint32 RawLen = Img.Pixels.Num() * 4;
			uint32 DataLen = RawLen;
			const uint8* DataPtr = (const uint8*)Img.Pixels.GetData();
			uint8 Codec = 0x00; // default raw

			// Check pixel validity
			bool bPixelsValid = false;
			if (Img.Pixels.Num() == Img.Width * Img.Height && Img.Pixels.Num() > 0)
			{
				bPixelsValid = true;
				// Quick check: not all zero
				int32 NonZero = 0;
				for (int32 i = 0; i < FMath::Min(100, Img.Pixels.Num()); ++i)
					if (Img.Pixels[i].DWColor() != 0) { ++NonZero; break; }
				if (NonZero == 0) bPixelsValid = false;
			}

			// JPEG encoding — try CompressImage, fall back to raw
			TArray64<uint8> JpegData; // must outlive the payload append below
			if (CameraCompress == TEXT("jpeg") && ImageWrapperModule && bPixelsValid)
			{
				FImageView View(Img.Pixels.GetData(), Img.Width, Img.Height);
				if (ImageWrapperModule->CompressImage(JpegData, EImageFormat::JPEG, View, JpegQuality)
					&& JpegData.Num() > 2 && JpegData[0] == 0xFF && JpegData[1] == 0xD8)
				{
					DataLen = JpegData.Num();
					DataPtr = JpegData.GetData();
					Codec = 0x01;
				}
			}

			// Write entry: [name_len][name][codec][pixfmt][reserved][w][h][rawlen][datalen][data]
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

		// Push to network thread
		NetThread->EnqueueCameraFrame(Ri, URSoccerLab::TcpProtocol::TypeRgb, Payload.GetData(), Payload.Num());
	}
}

void UURSTcpTransportComponent::DrainCompletedVision()
{
	// Currently encoding is synchronous in TickCameraCapture
	// This is for async encode results if we switch back to async
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
		}
		AdminListenerSock->HasPendingConnection(bPending);
	}

	// Process admin clients
	uint8 RecvBuf[65536];
	for (int32 Ci = AdminClients.Num() - 1; Ci >= 0; --Ci)
	{
		auto& Client = AdminClients[Ci];
		int32 BytesRead = 0;
		while (Client.Socket->Recv(RecvBuf, sizeof(RecvBuf), BytesRead))
		{
			if (BytesRead > 0)
				Client.ReadBuf.Append(RecvBuf, BytesRead);
			else break;
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
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return;

	auto SendReply = [&](TSharedPtr<FJsonObject> ReplyObj)
	{
		FString ReplyStr;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ReplyStr);
		FJsonSerializer::Serialize(ReplyObj.ToSharedRef(), W);
		FTCHARToUTF8 Utf8(*ReplyStr);
		int32 FrameLen = 1 + Utf8.Length();
		Client.WriteBuf.Add((FrameLen >> 24) & 0xFF);
		Client.WriteBuf.Add((FrameLen >> 16) & 0xFF);
		Client.WriteBuf.Add((FrameLen >> 8) & 0xFF);
		Client.WriteBuf.Add(FrameLen & 0xFF);
		Client.WriteBuf.Add(0x00);
		Client.WriteBuf.Append((const uint8*)Utf8.Get(), Utf8.Length());
	};

	FString Command;
	if (!Root->TryGetStringField(TEXT("command"), Command)) return;

	if (Command == TEXT("set_pose") && Core.IsValid())
	{
		FString ActorId;
		Root->TryGetStringField(TEXT("actor_id"), ActorId);
		const TArray<TSharedPtr<FJsonValue>>* Trans;
		FVector TransV = FVector::ZeroVector;
		if (Root->TryGetArrayField(TEXT("translation_m"), Trans) && Trans->Num() >= 3)
			TransV = FVector((*Trans)[0]->AsNumber(), (*Trans)[1]->AsNumber(), (*Trans)[2]->AsNumber());
		FQuat RotQuat = FQuat::Identity;
		const TArray<TSharedPtr<FJsonValue>>* Rot;
		if (Root->TryGetArrayField(TEXT("rotation_quat_xyzw"), Rot) && Rot->Num() >= 4)
			RotQuat = FQuat((*Rot)[1]->AsNumber(), (*Rot)[2]->AsNumber(), (*Rot)[3]->AsNumber(), (*Rot)[0]->AsNumber());
		TArray<float> JointQpos;
		const TArray<TSharedPtr<FJsonValue>>* JQ;
		if (Root->TryGetArrayField(TEXT("joint_qpos"), JQ))
			for (const auto& V : *JQ) JointQpos.Add(V->AsNumber());

		FURSPoseResult Result = Core->SetPose(ActorId, &TransV, &RotQuat, &JointQpos);
		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), Result.bOk);
		SendReply(Reply);
	}
	else if (Command == TEXT("reset") && Core.IsValid())
	{
		FString ActorId;
		Root->TryGetStringField(TEXT("actor_id"), ActorId);
		FURSPoseResult Result = Core->ResetRobot(ActorId);
		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), Result.bOk);
		SendReply(Reply);
	}
	else if (Command == TEXT("get_pose") && Core.IsValid())
	{
		FString ActorId;
		Root->TryGetStringField(TEXT("actor_id"), ActorId);
		FURSPoseResult Result = Core->GetPose(ActorId);
		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), Result.bOk);
		if (Result.bOk)
		{
			Reply->SetArrayField(TEXT("base_pos"), {
				MakeShared<FJsonValueNumber>(Result.AppliedTranslation.X),
				MakeShared<FJsonValueNumber>(Result.AppliedTranslation.Y),
				MakeShared<FJsonValueNumber>(Result.AppliedTranslation.Z) });
			Reply->SetArrayField(TEXT("base_quat"), {
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.X),
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.Y),
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.Z),
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.W) });
			TArray<TSharedPtr<FJsonValue>> JQ;
			for (float Q : Result.AppliedJointQpos) JQ.Add(MakeShared<FJsonValueNumber>(Q));
			Reply->SetArrayField(TEXT("joint_qpos"), JQ);
		}
		SendReply(Reply);
	}
	else if (Command == TEXT("lock_pose") && Core.IsValid())
	{
		FString ActorId;
		Root->TryGetStringField(TEXT("actor_id"), ActorId);
		FVector Trans = FVector::ZeroVector; FQuat Rot = FQuat::Identity; TArray<float> JQ;
		const TArray<TSharedPtr<FJsonValue>>* T;
		if (Root->TryGetArrayField(TEXT("translation_m"), T) && T->Num() >= 3)
			Trans = FVector((*T)[0]->AsNumber(), (*T)[1]->AsNumber(), (*T)[2]->AsNumber());
		const TArray<TSharedPtr<FJsonValue>>* R;
		if (Root->TryGetArrayField(TEXT("rotation_quat_xyzw"), R) && R->Num() >= 4)
			Rot = FQuat((*R)[1]->AsNumber(), (*R)[2]->AsNumber(), (*R)[3]->AsNumber(), (*R)[0]->AsNumber());
		const TArray<TSharedPtr<FJsonValue>>* J;
		if (Root->TryGetArrayField(TEXT("joint_qpos"), J))
			for (const auto& V : *J) JQ.Add(V->AsNumber());
		Core->SetPoseLock(ActorId, true, &Trans, &Rot, &JQ);
		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), true);
		SendReply(Reply);
	}
	else if (Command == TEXT("unlock_pose") && Core.IsValid())
	{
		FString ActorId;
		Root->TryGetStringField(TEXT("actor_id"), ActorId);
		Core->SetPoseLock(ActorId, false);
		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), true);
		SendReply(Reply);
	}
}

void UURSTcpTransportComponent::CloseSocket(FSocket* Sock)
{
	if (Sock)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Sock);
	}
}

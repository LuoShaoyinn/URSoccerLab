#include "Transport/URSTcpTransportComponent.h"
#include "Core/URSRobotCoreComponent.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"

#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

UURSTcpTransportComponent::UURSTcpTransportComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UURSTcpTransportComponent::BeginPlay()
{
	Super::BeginPlay();
	if (bAutoStart)
	{
		StartTransport();
	}
}

void UURSTcpTransportComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopTransport();
	Super::EndPlay(EndPlayReason);
}

bool UURSTcpTransportComponent::StartTransport()
{
	if (AActor* Owner = GetOwner())
	{
		Core = Owner->FindComponentByClass<UURSRobotCoreComponent>();
	}

	if (!Core.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[URS TCP] No UURSRobotCoreComponent found."));
		return false;
	}

	RebuildListeners();
	UE_LOG(LogTemp, Log, TEXT("[URS TCP] Transport started."));
	return true;
}

void UURSTcpTransportComponent::StopTransport()
{
	for (FRobotListener& L : RobotListeners)
	{
		CloseListener(L);
	}
	RobotListeners.Reset();
	CloseListener(AdminListener);
}

void UURSTcpTransportComponent::CloseListener(FRobotListener& Listener)
{
	for (FTcpClient& C : Listener.Clients)
	{
		CloseSocket(C.Socket);
	}
	Listener.Clients.Reset();
	CloseSocket(Listener.ListenerSocket);
	Listener.ListenerSocket = nullptr;
}

void UURSTcpTransportComponent::CloseSocket(FSocket* Sock)
{
	if (Sock)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Sock);
	}
}

void UURSTcpTransportComponent::RebuildListeners()
{
	StopTransport();

	TArray<FString> RobotIds = Core->GetRobotIds();
	LastKnownRobots = RobotIds;

	ISocketSubsystem* SSS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

	auto CreateListener = [&](int32 Port) -> FSocket*
	{
		FSocket* ListenSock = SSS->CreateSocket(NAME_Stream, TEXT("URS"), false);
		if (!ListenSock) return nullptr;
		ListenSock->SetReuseAddr();
		ListenSock->SetNonBlocking(true);

		TSharedRef<FInternetAddr> Addr = SSS->CreateInternetAddr();
		Addr->SetIp(0);
		Addr->SetPort(Port);
		if (!ListenSock->Bind(*Addr))
		{
			UE_LOG(LogTemp, Error, TEXT("[URS TCP] Failed to bind port %d"), Port);
			CloseSocket(ListenSock);
			return nullptr;
		}
		if (!ListenSock->Listen(16))
		{
			UE_LOG(LogTemp, Error, TEXT("[URS TCP] Failed to listen on port %d"), Port);
			CloseSocket(ListenSock);
			return nullptr;
		}
		return ListenSock;
	};

	for (int32 i = 0; i < RobotIds.Num(); ++i)
	{
		FRobotListener L;
		L.ActorId = RobotIds[i];
		L.ListenerSocket = CreateListener(RobotBasePort + i);
		if (L.ListenerSocket)
		{
			UE_LOG(LogTemp, Log, TEXT("[URS TCP] Robot '%s' listening on port %d"), *RobotIds[i], RobotBasePort + i);
		}
		RobotListeners.Add(MoveTemp(L));
	}

	AdminListener.ActorId = TEXT("admin");
	AdminListener.ListenerSocket = CreateListener(AdminPort);
	if (AdminListener.ListenerSocket)
	{
		UE_LOG(LogTemp, Log, TEXT("[URS TCP] Admin listening on port %d"), AdminPort);
	}
}

void UURSTcpTransportComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFn)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFn);

	if (!Core.IsValid())
	{
		return;
	}

	TArray<FString> CurrentRobots = Core->GetRobotIds();
	if (CurrentRobots != LastKnownRobots)
	{
		RebuildListeners();
	}

	for (FRobotListener& L : RobotListeners)
	{
		AcceptNewConnections(L);
		ReadFromClients(L);
	}
	AcceptNewConnections(AdminListener);
	ReadFromClients(AdminListener);

	TickStatePublish();
	TickCameraPublish();
}

void UURSTcpTransportComponent::AcceptNewConnections(FRobotListener& Listener)
{
	if (!Listener.ListenerSocket) return;

	ISocketSubsystem* SSS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TSharedRef<FInternetAddr> RemoteAddr = SSS->CreateInternetAddr();
	while (true)
	{
		FSocket* ClientSock = Listener.ListenerSocket->Accept(*RemoteAddr, TEXT("URS Client"));
		if (!ClientSock) break;

		ClientSock->SetNonBlocking(true);
		ClientSock->SetNoDelay();
		FTcpClient Client;
		Client.Socket = ClientSock;
		Listener.Clients.Add(MoveTemp(Client));
		UE_LOG(LogTemp, Log, TEXT("[URS TCP] Client connected to '%s' (%d total)"),
			*Listener.ActorId, Listener.Clients.Num());
	}
}

void UURSTcpTransportComponent::ReadFromClients(FRobotListener& Listener)
{
	for (int32 Idx = Listener.Clients.Num() - 1; Idx >= 0; --Idx)
	{
		FTcpClient& Client = Listener.Clients[Idx];
		FSocket* Sock = Client.Socket;
		if (!Sock)
		{
			Listener.Clients.RemoveAt(Idx);
			continue;
		}

		uint8 TmpBuf[8192];
		int32 BytesRead = 0;
		while (Sock->Recv(TmpBuf, sizeof(TmpBuf), BytesRead))
		{
			if (BytesRead <= 0) break;
			Client.ReadBuffer.Append(TmpBuf, BytesRead);
		}

		if (Sock->GetConnectionState() == SCS_ConnectionError)
		{
			CloseSocket(Sock);
			Listener.Clients.RemoveAt(Idx);
			continue;
		}

		bool bBadFrame = false;
		while (Client.ReadBuffer.Num() >= 5)
		{
			const uint32 FrameLen =
				(static_cast<uint32>(Client.ReadBuffer[0]) << 24) |
				(static_cast<uint32>(Client.ReadBuffer[1]) << 16) |
				(static_cast<uint32>(Client.ReadBuffer[2]) << 8) |
				 static_cast<uint32>(Client.ReadBuffer[3]);

			if (FrameLen < 1 || FrameLen > 16 * 1024 * 1024)
			{
				bBadFrame = true;
				break;
			}
			if (static_cast<uint32>(Client.ReadBuffer.Num()) < 4u + FrameLen) break;

			uint8 FrameType = Client.ReadBuffer[4];
			const int32 PayloadSize = FrameLen - 1;
			const uint8* PayloadData = PayloadSize > 0 ? Client.ReadBuffer.GetData() + 5 : nullptr;

			if (FrameType == URSProtocol::Type_JSON && PayloadSize > 0)
			{
				FString JsonStr(UTF8_TO_TCHAR(reinterpret_cast<const char*>(PayloadData)));
				if (Listener.ActorId == TEXT("admin"))
				{
					ProcessAdminRequest(Sock, JsonStr);
				}
				else
				{
					ProcessCommand(Listener.ActorId, JsonStr);
				}
			}

			Client.ReadBuffer.RemoveAt(0, 4 + FrameLen);
		}

		if (bBadFrame)
		{
			CloseSocket(Sock);
			Listener.Clients.RemoveAt(Idx);
		}
	}
}

void UURSTcpTransportComponent::ProcessCommand(const FString& ActorId, const FString& JsonStr)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return;

	TMap<FString, float> NamedValues;
	for (const auto& Pair : Root->Values)
	{
		const double Val = Pair.Value->AsNumber();
		if (FMath::IsFinite(Val))
		{
			NamedValues.Add(Pair.Key, static_cast<float>(Val));
		}
	}

	Core->SubmitCommand(ActorId, NamedValues);
}

void UURSTcpTransportComponent::ProcessAdminRequest(FSocket* Sock, const FString& JsonStr)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);

	auto SendReply = [Sock, this](TSharedPtr<FJsonObject> ReplyObj)
	{
		FString ReplyStr;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ReplyStr);
		FJsonSerializer::Serialize(ReplyObj.ToSharedRef(), W);

		FTCHARToUTF8 Utf8(*ReplyStr);
		TArray<uint8> Frame;
		const uint32 TotalLen = 1 + Utf8.Length();
		Frame.SetNumUninitialized(4 + TotalLen);
		Frame[0] = (TotalLen >> 24) & 0xFF;
		Frame[1] = (TotalLen >> 16) & 0xFF;
		Frame[2] = (TotalLen >> 8) & 0xFF;
		Frame[3] = TotalLen & 0xFF;
		Frame[4] = URSProtocol::Type_JSON;
		FMemory::Memcpy(Frame.GetData() + 5, (const uint8*)Utf8.Get(), Utf8.Length());

		int32 BytesSent = 0;
		Sock->Send(Frame.GetData(), Frame.Num(), BytesSent);
	};

	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		auto Err = MakeShared<FJsonObject>();
		Err->SetBoolField(TEXT("ok"), false);
		Err->SetStringField(TEXT("error"), TEXT("bad_json"));
		SendReply(Err);
		return;
	}

	FString Command;
	if (!Root->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		auto Err = MakeShared<FJsonObject>();
		Err->SetBoolField(TEXT("ok"), false);
		Err->SetStringField(TEXT("error"), TEXT("missing_command"));
		SendReply(Err);
		return;
	}

	const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
	TSharedPtr<FJsonObject> DefaultArgsObj;
	if (!Root->TryGetObjectField(TEXT("args"), ArgsPtr) || !ArgsPtr || !ArgsPtr->IsValid())
	{
		DefaultArgsObj = MakeShared<FJsonObject>();
		ArgsPtr = &DefaultArgsObj;
	}
	const TSharedPtr<FJsonObject>& Args = *ArgsPtr;

	FString ActorId;
	if (!Args->TryGetStringField(TEXT("actor_id"), ActorId) || ActorId.IsEmpty())
	{
		auto Err = MakeShared<FJsonObject>();
		Err->SetBoolField(TEXT("ok"), false);
		Err->SetStringField(TEXT("error"), TEXT("missing_actor_id"));
		SendReply(Err);
		return;
	}

	if (Command == TEXT("set_pose"))
	{
		TOptional<FVector> Trans;
		TOptional<FQuat> Rot;
		TOptional<TArray<float>> JointQpos;

		const TArray<TSharedPtr<FJsonValue>>* TArr;
		if (Args->TryGetArrayField(TEXT("translation_m"), TArr) && TArr && TArr->Num() == 3)
		{
			Trans = FVector((*TArr)[0]->AsNumber(), (*TArr)[1]->AsNumber(), (*TArr)[2]->AsNumber());
		}
		const TArray<TSharedPtr<FJsonValue>>* RArr;
		if (Args->TryGetArrayField(TEXT("rotation_quat_xyzw"), RArr) && RArr && RArr->Num() == 4)
		{
			Rot = FQuat((*RArr)[0]->AsNumber(), (*RArr)[1]->AsNumber(), (*RArr)[2]->AsNumber(), (*RArr)[3]->AsNumber());
		}
		const TArray<TSharedPtr<FJsonValue>>* JArr;
		if (Args->TryGetArrayField(TEXT("joint_qpos"), JArr) && JArr)
		{
			TArray<float> Qpos;
			for (const auto& V : *JArr) Qpos.Add(static_cast<float>(V->AsNumber()));
			JointQpos = MoveTemp(Qpos);
		}

		FURSPoseResult Result = Core->SetPose(ActorId,
			Trans.IsSet() ? &Trans.GetValue() : nullptr,
			Rot.IsSet() ? &Rot.GetValue() : nullptr,
			JointQpos.IsSet() ? &JointQpos.GetValue() : nullptr);

		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), Result.bOk);
		Reply->SetStringField(TEXT("command"), TEXT("set_pose"));
		if (Result.bOk)
		{
			auto ResObj = MakeShared<FJsonObject>();
			ResObj->SetStringField(TEXT("actor_id"), ActorId);
			ResObj->SetArrayField(TEXT("translation_m"), {
				MakeShared<FJsonValueNumber>(Result.AppliedTranslation.X),
				MakeShared<FJsonValueNumber>(Result.AppliedTranslation.Y),
				MakeShared<FJsonValueNumber>(Result.AppliedTranslation.Z) });
			ResObj->SetArrayField(TEXT("rotation_quat_xyzw"), {
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.X),
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.Y),
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.Z),
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.W) });
			TArray<TSharedPtr<FJsonValue>> Jqpos;
			for (float v : Result.AppliedJointQpos) Jqpos.Add(MakeShared<FJsonValueNumber>(v));
			ResObj->SetArrayField(TEXT("joint_qpos"), Jqpos);
			ResObj->SetNumberField(TEXT("sim_time"), Result.SimTime);
			Reply->SetObjectField(TEXT("result"), ResObj);
		}
		else
		{
			Reply->SetStringField(TEXT("error"), Result.Error);
			Reply->SetStringField(TEXT("message"), Result.Message);
		}
		SendReply(Reply);
	}
	else if (Command == TEXT("get_pose"))
	{
		FURSPoseResult Result = Core->GetPose(ActorId);
		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), Result.bOk);
		Reply->SetStringField(TEXT("command"), TEXT("get_pose"));
		if (Result.bOk)
		{
			auto ResObj = MakeShared<FJsonObject>();
			ResObj->SetStringField(TEXT("actor_id"), ActorId);
			ResObj->SetArrayField(TEXT("translation_m"), {
				MakeShared<FJsonValueNumber>(Result.AppliedTranslation.X),
				MakeShared<FJsonValueNumber>(Result.AppliedTranslation.Y),
				MakeShared<FJsonValueNumber>(Result.AppliedTranslation.Z) });
			ResObj->SetArrayField(TEXT("rotation_quat_xyzw"), {
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.X),
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.Y),
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.Z),
				MakeShared<FJsonValueNumber>(Result.AppliedRotation.W) });
			TArray<TSharedPtr<FJsonValue>> Jqpos;
			for (float v : Result.AppliedJointQpos) Jqpos.Add(MakeShared<FJsonValueNumber>(v));
			ResObj->SetArrayField(TEXT("joint_qpos"), Jqpos);
			ResObj->SetNumberField(TEXT("sim_time"), Result.SimTime);
			Reply->SetObjectField(TEXT("result"), ResObj);
		}
		else
		{
			Reply->SetStringField(TEXT("error"), Result.Error);
			Reply->SetStringField(TEXT("message"), Result.Message);
		}
		SendReply(Reply);
	}
	else if (Command == TEXT("reset"))
	{
		FURSPoseResult Result = Core->ResetRobot(ActorId);
		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), Result.bOk);
		Reply->SetStringField(TEXT("command"), TEXT("reset"));
		if (Result.bOk)
		{
			auto ResObj = MakeShared<FJsonObject>();
			ResObj->SetStringField(TEXT("actor_id"), ActorId);
			ResObj->SetNumberField(TEXT("sim_time"), Result.SimTime);
			Reply->SetObjectField(TEXT("result"), ResObj);
		}
		else
		{
			Reply->SetStringField(TEXT("error"), Result.Error);
			Reply->SetStringField(TEXT("message"), Result.Message);
		}
		SendReply(Reply);
	}
	else if (Command == TEXT("lock_pose"))
	{
		TOptional<FVector> Trans;
		TOptional<FQuat> Rot;
		TOptional<TArray<float>> JointQpos;
		const TArray<TSharedPtr<FJsonValue>>* TArr;
		if (Args->TryGetArrayField(TEXT("translation_m"), TArr) && TArr && TArr->Num() == 3)
			Trans = FVector((*TArr)[0]->AsNumber(), (*TArr)[1]->AsNumber(), (*TArr)[2]->AsNumber());
		const TArray<TSharedPtr<FJsonValue>>* RArr;
		if (Args->TryGetArrayField(TEXT("rotation_quat_xyzw"), RArr) && RArr && RArr->Num() == 4)
			Rot = FQuat((*RArr)[0]->AsNumber(), (*RArr)[1]->AsNumber(), (*RArr)[2]->AsNumber(), (*RArr)[3]->AsNumber());
		const TArray<TSharedPtr<FJsonValue>>* JArr;
		if (Args->TryGetArrayField(TEXT("joint_qpos"), JArr) && JArr)
		{
			TArray<float> Qpos;
			for (const auto& V : *JArr) Qpos.Add(static_cast<float>(V->AsNumber()));
			JointQpos = MoveTemp(Qpos);
		}
		Core->SetPoseLock(ActorId, true,
			Trans.IsSet() ? &Trans.GetValue() : nullptr,
			Rot.IsSet() ? &Rot.GetValue() : nullptr,
			JointQpos.IsSet() ? &JointQpos.GetValue() : nullptr);
		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), true);
		Reply->SetStringField(TEXT("command"), TEXT("lock_pose"));
		SendReply(Reply);
	}
	else if (Command == TEXT("unlock_pose"))
	{
		Core->SetPoseLock(ActorId, false);
		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), true);
		Reply->SetStringField(TEXT("command"), TEXT("unlock_pose"));
		SendReply(Reply);
	}
	else
	{
		auto Err = MakeShared<FJsonObject>();
		Err->SetBoolField(TEXT("ok"), false);
		Err->SetStringField(TEXT("error"), TEXT("unknown_command"));
		Err->SetStringField(TEXT("message"), FString::Printf(TEXT("Unknown command: %s"), *Command));
		SendReply(Err);
	}
}

void UURSTcpTransportComponent::TickStatePublish()
{
	const double Now = FPlatformTime::Seconds();
	const double Interval = StateRateHz > 0 ? 1.0 / StateRateHz : 0.0;
	if (Interval <= 0 || Now - LastStateTimeSec < Interval) return;
	LastStateTimeSec = Now;

	for (FRobotListener& L : RobotListeners)
	{
		if (L.Clients.Num() == 0) continue;

		FString Json = BuildStateJson(L.ActorId);
		FTCHARToUTF8 Utf8(*Json);
		SendToClients(L, URSProtocol::Type_JSON, reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
}

void UURSTcpTransportComponent::SendToClients(FRobotListener& Listener, uint8 FrameType, const uint8* PayloadData, int32 PayloadSize)
{
	for (int32 Idx = Listener.Clients.Num() - 1; Idx >= 0; --Idx)
	{
		FSocket* Sock = Listener.Clients[Idx].Socket;
		if (!Sock)
		{
			Listener.Clients.RemoveAt(Idx);
			continue;
		}
		if (!SendFrame(Sock, FrameType, PayloadData, PayloadSize))
		{
			CloseSocket(Sock);
			Listener.Clients.RemoveAt(Idx);
		}
	}
}

void UURSTcpTransportComponent::TickCameraPublish()
{
	const double Now = FPlatformTime::Seconds();
	const double Interval = CameraRateHz > 0 ? 1.0 / CameraRateHz : 0.0;

	if (Interval > 0 && Now - LastCameraTimeSec >= Interval)
	{
		LastCameraTimeSec = Now;
		for (FRobotListener& L : RobotListeners)
		{
			if (L.Clients.Num() == 0) continue;
			Core->RequestCameraReadback(L.ActorId);
		}
	}

	for (FRobotListener& L : RobotListeners)
	{
		if (L.Clients.Num() == 0) continue;

		FURSRobotState State;
		if (!Core->GetRobotState(L.ActorId, State)) continue;

		for (const FURSCameraInfo& CamInfo : State.Cameras)
		{
			TArray<FColor> Pixels;
			if (!Core->ConsumeCameraFrame(L.ActorId, CamInfo.Name, Pixels) || Pixels.Num() == 0) continue;

			TArray<uint8> Encoded = EncodeCameraFrame(Pixels, CamInfo.Width, CamInfo.Height);
			SendToClients(L, URSProtocol::Type_Camera, Encoded.GetData(), Encoded.Num());
		}
	}
}

FString UURSTcpTransportComponent::BuildStateJson(const FString& ActorId)
{
	FURSRobotState State;
	if (!Core->GetRobotState(ActorId, State)) return TEXT("{}");

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("sim_time"), State.SimTime);
	Root->SetBoolField(TEXT("command_timed_out"), State.bCommandTimedOut);

	auto BaseObj = MakeShared<FJsonObject>();
	BaseObj->SetArrayField(TEXT("pos"), {
		MakeShared<FJsonValueNumber>(State.BasePos.X),
		MakeShared<FJsonValueNumber>(State.BasePos.Y),
		MakeShared<FJsonValueNumber>(State.BasePos.Z) });
	BaseObj->SetArrayField(TEXT("quat"), {
		MakeShared<FJsonValueNumber>(State.BaseQuat.W),
		MakeShared<FJsonValueNumber>(State.BaseQuat.X),
		MakeShared<FJsonValueNumber>(State.BaseQuat.Y),
		MakeShared<FJsonValueNumber>(State.BaseQuat.Z) });
	TArray<TSharedPtr<FJsonValue>> VelArr;
	for (double V : State.BaseVel)
	{
		VelArr.Add(MakeShared<FJsonValueNumber>(V));
	}
	BaseObj->SetArrayField(TEXT("vel"), VelArr);
	Root->SetObjectField(TEXT("base"), BaseObj);

	TSharedPtr<FJsonObject> JointsObj = MakeShared<FJsonObject>();
	const bool bScalar = State.JointNames.Num() == State.JointQpos.Num()
		&& State.JointNames.Num() == State.JointQvel.Num();
	if (bScalar)
	{
		for (int32 i = 0; i < State.JointNames.Num(); ++i)
		{
			auto JObj = MakeShared<FJsonObject>();
			JObj->SetNumberField(TEXT("qpos"), State.JointQpos[i]);
			JObj->SetNumberField(TEXT("qvel"), State.JointQvel[i]);
			JointsObj->SetObjectField(State.JointNames[i], JObj);
		}
	}
	Root->SetObjectField(TEXT("joints"), JointsObj);

	TSharedPtr<FJsonObject> ActuatorsObj = MakeShared<FJsonObject>();
	if (State.ActuatorNames.Num() == State.MotorCommand.Num())
	{
		for (int32 i = 0; i < State.ActuatorNames.Num(); ++i)
		{
			ActuatorsObj->SetNumberField(State.ActuatorNames[i], State.MotorCommand[i]);
		}
	}
	Root->SetObjectField(TEXT("actuators"), ActuatorsObj);

	TArray<TSharedPtr<FJsonValue>> CamerasArr;
	for (const FURSCameraInfo& Cam : State.Cameras)
	{
		auto CamObj = MakeShared<FJsonObject>();
		CamObj->SetStringField(TEXT("name"), Cam.Name);
		CamObj->SetNumberField(TEXT("width"), Cam.Width);
		CamObj->SetNumberField(TEXT("height"), Cam.Height);
		CamObj->SetStringField(TEXT("format"), Cam.Format);
		CamerasArr.Add(MakeShared<FJsonValueObject>(CamObj));
	}
	Root->SetArrayField(TEXT("cameras"), CamerasArr);

	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root.ToSharedRef(), W);
	return Json;
}

TArray<uint8> UURSTcpTransportComponent::EncodeCameraFrame(const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
	TArray<uint8> Out;

	const uint8 Codec = (CameraCompress == TEXT("jpeg")) ? URSProtocol::CameraCodec_JPEG : URSProtocol::CameraCodec_Raw;
	const uint8 Flags = 0x01;

	Out.Add(Codec);
	Out.Add(Flags);
	Out.Add(static_cast<uint8>(Width & 0xFF));
	Out.Add(static_cast<uint8>((Width >> 8) & 0xFF));
	Out.Add(static_cast<uint8>(Height & 0xFF));
	Out.Add(static_cast<uint8>((Height >> 8) & 0xFF));

	if (Codec == URSProtocol::CameraCodec_JPEG && Pixels.Num() > 0)
	{
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper(Module.CreateImageWrapper(EImageFormat::JPEG));
		const uint8* RawBGRA = reinterpret_cast<const uint8*>(Pixels.GetData());
		if (Wrapper->SetRaw(RawBGRA, Width * Height * 4, Width, Height, ERGBFormat::BGRA, 8))
		{
			const auto& Compressed = Wrapper->GetCompressed(JpegQuality);
			Out.Append(Compressed);
		}
	}
	else if (Pixels.Num() > 0)
	{
		const int32 RawSize = Pixels.Num() * 4;
		Out.Append(reinterpret_cast<const uint8*>(Pixels.GetData()), RawSize);
	}

	return Out;
}

bool UURSTcpTransportComponent::SendFrame(FSocket* Sock, uint8 FrameType, const uint8* PayloadData, int32 PayloadSize)
{
	if (!Sock) return false;

	const uint32 FrameLen = 1 + PayloadSize;
	uint8 Header[5];
	Header[0] = (FrameLen >> 24) & 0xFF;
	Header[1] = (FrameLen >> 16) & 0xFF;
	Header[2] = (FrameLen >> 8) & 0xFF;
	Header[3] = FrameLen & 0xFF;
	Header[4] = FrameType;

	TArray<uint8> Frame;
	Frame.Append(Header, 5);
	if (PayloadSize > 0)
	{
		Frame.Append(PayloadData, PayloadSize);
	}

	int32 BytesSent = 0;
	const bool bOk = Sock->Send(Frame.GetData(), Frame.Num(), BytesSent);
	if (!bOk || BytesSent < Frame.Num())
	{
		return false;
	}
	return true;
}


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
	FlushAllWrites();
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
				FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(PayloadData), PayloadSize);
				FString JsonStr(Converter.Length(), Converter.Get());
				if (Listener.ActorId == TEXT("admin"))
				{
					ProcessAdminRequest(Client, JsonStr);
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

void UURSTcpTransportComponent::ProcessAdminRequest(FTcpClient& Client, const FString& JsonStr)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);

	auto SendReply = [&Client, this](TSharedPtr<FJsonObject> ReplyObj)
	{
		FString ReplyStr;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ReplyStr);
		FJsonSerializer::Serialize(ReplyObj.ToSharedRef(), W);

		FTCHARToUTF8 Utf8(*ReplyStr);
		EnqueueFrame(Client, URSProtocol::Type_JSON,
			reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
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
		if (Args->TryGetArrayField(TEXT("translation_m"), TArr) && TArr)
		{
			if (TArr->Num() != 3)
			{
				auto Err = MakeShared<FJsonObject>();
				Err->SetBoolField(TEXT("ok"), false);
				Err->SetStringField(TEXT("error"), TEXT("invalid_translation"));
				Err->SetStringField(TEXT("message"), FString::Printf(TEXT("translation_m must have 3 elements, got %d"), TArr->Num()));
				SendReply(Err);
				return;
			}
			double Tx = (*TArr)[0]->AsNumber();
			double Ty = (*TArr)[1]->AsNumber();
			double Tz = (*TArr)[2]->AsNumber();
			if (!FMath::IsFinite(Tx) || !FMath::IsFinite(Ty) || !FMath::IsFinite(Tz))
			{
				auto Err = MakeShared<FJsonObject>();
				Err->SetBoolField(TEXT("ok"), false);
				Err->SetStringField(TEXT("error"), TEXT("invalid_translation"));
				Err->SetStringField(TEXT("message"), TEXT("translation_m contains NaN or infinity"));
				SendReply(Err);
				return;
			}
			Trans = FVector(Tx, Ty, Tz);
		}
		const TArray<TSharedPtr<FJsonValue>>* RArr;
		if (Args->TryGetArrayField(TEXT("rotation_quat_xyzw"), RArr) && RArr)
		{
			if (RArr->Num() != 4)
			{
				auto Err = MakeShared<FJsonObject>();
				Err->SetBoolField(TEXT("ok"), false);
				Err->SetStringField(TEXT("error"), TEXT("invalid_rotation"));
				Err->SetStringField(TEXT("message"), FString::Printf(TEXT("rotation_quat_xyzw must have 4 elements, got %d"), RArr->Num()));
				SendReply(Err);
				return;
			}
			double Rx = (*RArr)[0]->AsNumber();
			double Ry = (*RArr)[1]->AsNumber();
			double Rz = (*RArr)[2]->AsNumber();
			double Rw = (*RArr)[3]->AsNumber();
			if (!FMath::IsFinite(Rx) || !FMath::IsFinite(Ry) || !FMath::IsFinite(Rz) || !FMath::IsFinite(Rw))
			{
				auto Err = MakeShared<FJsonObject>();
				Err->SetBoolField(TEXT("ok"), false);
				Err->SetStringField(TEXT("error"), TEXT("invalid_rotation"));
				Err->SetStringField(TEXT("message"), TEXT("rotation_quat_xyzw contains NaN or infinity"));
				SendReply(Err);
				return;
			}
			const double QuatLenSq = Rx * Rx + Ry * Ry + Rz * Rz + Rw * Rw;
			if (QuatLenSq < KINDA_SMALL_NUMBER)
			{
				auto Err = MakeShared<FJsonObject>();
				Err->SetBoolField(TEXT("ok"), false);
				Err->SetStringField(TEXT("error"), TEXT("invalid_rotation"));
				Err->SetStringField(TEXT("message"), TEXT("rotation_quat_xyzw is zero-length"));
				SendReply(Err);
				return;
			}
			const double InvLen = 1.0 / FMath::Sqrt(QuatLenSq);
			Rot = FQuat(Rx * InvLen, Ry * InvLen, Rz * InvLen, Rw * InvLen);
		}
		const TArray<TSharedPtr<FJsonValue>>* JArr;
		if (Args->TryGetArrayField(TEXT("joint_qpos"), JArr) && JArr)
		{
			TArray<float> Qpos;
			for (const auto& V : *JArr)
			{
				double Val = V->AsNumber();
				if (!FMath::IsFinite(Val))
				{
					auto Err = MakeShared<FJsonObject>();
					Err->SetBoolField(TEXT("ok"), false);
					Err->SetStringField(TEXT("error"), TEXT("invalid_joint_qpos"));
					Err->SetStringField(TEXT("message"), TEXT("joint_qpos contains NaN or infinity"));
					SendReply(Err);
					return;
				}
				Qpos.Add(static_cast<float>(Val));
			}
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
		if (Args->TryGetArrayField(TEXT("translation_m"), TArr) && TArr)
		{
			if (TArr->Num() != 3)
			{
				auto Err = MakeShared<FJsonObject>();
				Err->SetBoolField(TEXT("ok"), false);
				Err->SetStringField(TEXT("error"), TEXT("invalid_translation"));
				Err->SetStringField(TEXT("message"), FString::Printf(TEXT("translation_m must have 3 elements, got %d"), TArr->Num()));
				SendReply(Err);
				return;
			}
			double Tx = (*TArr)[0]->AsNumber(), Ty = (*TArr)[1]->AsNumber(), Tz = (*TArr)[2]->AsNumber();
			if (!FMath::IsFinite(Tx) || !FMath::IsFinite(Ty) || !FMath::IsFinite(Tz))
			{
				auto Err = MakeShared<FJsonObject>();
				Err->SetBoolField(TEXT("ok"), false);
				Err->SetStringField(TEXT("error"), TEXT("invalid_translation"));
				SendReply(Err);
				return;
			}
			Trans = FVector(Tx, Ty, Tz);
		}
		const TArray<TSharedPtr<FJsonValue>>* RArr;
		if (Args->TryGetArrayField(TEXT("rotation_quat_xyzw"), RArr) && RArr)
		{
			if (RArr->Num() != 4)
			{
				auto Err = MakeShared<FJsonObject>();
				Err->SetBoolField(TEXT("ok"), false);
				Err->SetStringField(TEXT("error"), TEXT("invalid_rotation"));
				Err->SetStringField(TEXT("message"), FString::Printf(TEXT("rotation_quat_xyzw must have 4 elements, got %d"), RArr->Num()));
				SendReply(Err);
				return;
			}
			double Rx = (*RArr)[0]->AsNumber(), Ry = (*RArr)[1]->AsNumber(), Rz = (*RArr)[2]->AsNumber(), Rw = (*RArr)[3]->AsNumber();
			if (!FMath::IsFinite(Rx) || !FMath::IsFinite(Ry) || !FMath::IsFinite(Rz) || !FMath::IsFinite(Rw))
			{
				auto Err = MakeShared<FJsonObject>();
				Err->SetBoolField(TEXT("ok"), false);
				Err->SetStringField(TEXT("error"), TEXT("invalid_rotation"));
				SendReply(Err);
				return;
			}
			const double LenSq = Rx * Rx + Ry * Ry + Rz * Rz + Rw * Rw;
			if (LenSq < KINDA_SMALL_NUMBER)
			{
				auto Err = MakeShared<FJsonObject>();
				Err->SetBoolField(TEXT("ok"), false);
				Err->SetStringField(TEXT("error"), TEXT("invalid_rotation"));
				SendReply(Err);
				return;
			}
			const double InvLen = 1.0 / FMath::Sqrt(LenSq);
			Rot = FQuat(Rx * InvLen, Ry * InvLen, Rz * InvLen, Rw * InvLen);
		}
		const TArray<TSharedPtr<FJsonValue>>* JArr;
		if (Args->TryGetArrayField(TEXT("joint_qpos"), JArr) && JArr)
		{
			TArray<float> Qpos;
			for (const auto& V : *JArr)
			{
				double Val = V->AsNumber();
				if (!FMath::IsFinite(Val))
				{
					auto Err = MakeShared<FJsonObject>();
					Err->SetBoolField(TEXT("ok"), false);
					Err->SetStringField(TEXT("error"), TEXT("invalid_joint_qpos"));
					SendReply(Err);
					return;
				}
				Qpos.Add(static_cast<float>(Val));
			}
			JointQpos = MoveTemp(Qpos);
		}
		FURSPoseResult LockResult = Core->SetPoseLock(ActorId, true,
			Trans.IsSet() ? &Trans.GetValue() : nullptr,
			Rot.IsSet() ? &Rot.GetValue() : nullptr,
			JointQpos.IsSet() ? &JointQpos.GetValue() : nullptr);
		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), LockResult.bOk);
		Reply->SetStringField(TEXT("command"), TEXT("lock_pose"));
		if (!LockResult.bOk)
		{
			Reply->SetStringField(TEXT("error"), LockResult.Error);
			Reply->SetStringField(TEXT("message"), LockResult.Message);
		}
		SendReply(Reply);
	}
	else if (Command == TEXT("unlock_pose"))
	{
		FURSPoseResult UnlockResult = Core->SetPoseLock(ActorId, false);
		auto Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("ok"), UnlockResult.bOk);
		Reply->SetStringField(TEXT("command"), TEXT("unlock_pose"));
		if (!UnlockResult.bOk)
		{
			Reply->SetStringField(TEXT("error"), UnlockResult.Error);
			Reply->SetStringField(TEXT("message"), UnlockResult.Message);
		}
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
		FTcpClient& Client = Listener.Clients[Idx];
		if (!Client.Socket)
		{
			Listener.Clients.RemoveAt(Idx);
			continue;
		}
		EnqueueFrame(Client, FrameType, PayloadData, PayloadSize);
		if (Client.WriteBuffer.Num() > MaxSendQueueBytes)
		{
			CloseSocket(Client.Socket);
			Listener.Clients.RemoveAt(Idx);
		}
	}
}

void UURSTcpTransportComponent::EnqueueFrame(FTcpClient& Client, uint8 FrameType, const uint8* PayloadData, int32 PayloadSize)
{
	const uint32 FrameLen = 1 + PayloadSize;
	const int32 OldNum = Client.WriteBuffer.Num();
	Client.WriteBuffer.AddUninitialized(5 + PayloadSize);
	uint8* Dest = Client.WriteBuffer.GetData() + OldNum;
	Dest[0] = (FrameLen >> 24) & 0xFF;
	Dest[1] = (FrameLen >> 16) & 0xFF;
	Dest[2] = (FrameLen >> 8) & 0xFF;
	Dest[3] = FrameLen & 0xFF;
	Dest[4] = FrameType;
	if (PayloadSize > 0)
	{
		FMemory::Memcpy(Dest + 5, PayloadData, PayloadSize);
	}
}

bool UURSTcpTransportComponent::FlushClientWrites(FTcpClient& Client)
{
	if (!Client.Socket || Client.WriteBuffer.Num() == 0) return true;

	int32 TotalSent = 0;
	while (TotalSent < Client.WriteBuffer.Num())
	{
		int32 BytesSent = 0;
		const int32 Remaining = Client.WriteBuffer.Num() - TotalSent;
		const bool bOk = Client.Socket->Send(
			Client.WriteBuffer.GetData() + TotalSent, Remaining, BytesSent);

		if (!bOk || BytesSent <= 0)
		{
			const ESocketConnectionState State = Client.Socket->GetConnectionState();
			if (State == SCS_ConnectionError)
			{
				return false;
			}
			break;
		}
		TotalSent += BytesSent;
	}

	if (TotalSent > 0)
	{
		Client.WriteBuffer.RemoveAt(0, TotalSent);
	}
	return true;
}

void UURSTcpTransportComponent::FlushAllWrites()
{
	for (FRobotListener& L : RobotListeners)
	{
		for (int32 Idx = L.Clients.Num() - 1; Idx >= 0; --Idx)
		{
			FTcpClient& Client = L.Clients[Idx];
			if (!FlushClientWrites(Client))
			{
				CloseSocket(Client.Socket);
				L.Clients.RemoveAt(Idx);
			}
		}
	}

	for (int32 Idx = AdminListener.Clients.Num() - 1; Idx >= 0; --Idx)
	{
		FTcpClient& Client = AdminListener.Clients[Idx];
		if (!FlushClientWrites(Client))
		{
			CloseSocket(Client.Socket);
			AdminListener.Clients.RemoveAt(Idx);
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

		TArray<uint8> Packed;
		const uint8 Codec = (CameraCompress == TEXT("jpeg")) ? URSProtocol::CameraCodec_JPEG : URSProtocol::CameraCodec_Raw;
		Packed.Add(Codec);
		Packed.Add(static_cast<uint8>(State.Cameras.Num()));

		// Simulation timestamp (8-byte LE double) for correlating with state
		uint8 SimTimeBytes[8];
		FMemory::Memcpy(SimTimeBytes, &State.SimTime, 8);
		Packed.Append(SimTimeBytes, 8);

		bool bAnyNewFrame = false;

		for (const FURSCameraInfo& CamInfo : State.Cameras)
		{
			TArray<FColor> Pixels;
			bool bHasFrame = Core->ConsumeCameraFrame(L.ActorId, CamInfo.Name, Pixels) && Pixels.Num() > 0;
			if (bHasFrame) bAnyNewFrame = true;

			TArray<uint8> Encoded;
			if (bHasFrame && Codec == URSProtocol::CameraCodec_JPEG)
			{
				IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
				TSharedPtr<IImageWrapper> Wrapper(Module.CreateImageWrapper(EImageFormat::JPEG));
				const uint8* RawBGRA = reinterpret_cast<const uint8*>(Pixels.GetData());
				if (Wrapper->SetRaw(RawBGRA, CamInfo.Width * CamInfo.Height * 4, CamInfo.Width, CamInfo.Height, ERGBFormat::BGRA, 8))
				{
					Encoded = Wrapper->GetCompressed(JpegQuality);
				}
			}
			else if (bHasFrame)
			{
				Encoded.Append(reinterpret_cast<const uint8*>(Pixels.GetData()), Pixels.Num() * 4);
			}

			Packed.Add(static_cast<uint8>(CamInfo.Width & 0xFF));
			Packed.Add(static_cast<uint8>((CamInfo.Width >> 8) & 0xFF));
			Packed.Add(static_cast<uint8>(CamInfo.Height & 0xFF));
			Packed.Add(static_cast<uint8>((CamInfo.Height >> 8) & 0xFF));
			int32 Len = Encoded.Num();
			Packed.Add(static_cast<uint8>(Len & 0xFF));
			Packed.Add(static_cast<uint8>((Len >> 8) & 0xFF));
			Packed.Add(static_cast<uint8>((Len >> 16) & 0xFF));
			Packed.Add(static_cast<uint8>((Len >> 24) & 0xFF));
			Packed.Append(Encoded);
		}

		if (bAnyNewFrame)
		{
			SendToClients(L, URSProtocol::Type_Camera, Packed.GetData(), Packed.Num());
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


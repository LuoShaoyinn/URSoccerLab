#include "Transport/URSTcpTransportComponent.h"
#include "Core/URSRobotCoreComponent.h"
#include "NDisplay/URSDisplayClusterCameraBinderComponent.h"
#include "Scene/URSSceneConfigComponent.h"

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
#include "Async/Async.h"
#include "Misc/CommandLine.h"
#include "Misc/Compression.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"

namespace
{
struct FURSImageEntry
{
	FString CameraName;
	uint8 Codec = URSoccerLab::TcpProtocol::ImageCodecRaw;
	uint8 PixelFormat = URSoccerLab::TcpProtocol::PixelFormatBgra8;
	uint16 Width = 0;
	uint16 Height = 0;
	uint32 UncompressedBytes = 0;
	TArray<uint8> Data;
};

struct FURSRawRgbImage
{
	FString CameraName;
	uint16 Width = 0;
	uint16 Height = 0;
	TArray<FColor> Pixels;
};

void AppendU16Le(TArray<uint8>& Out, const uint16 Value)
{
	Out.Add(static_cast<uint8>(Value & 0xff));
	Out.Add(static_cast<uint8>((Value >> 8) & 0xff));
}

void AppendU32Le(TArray<uint8>& Out, const uint32 Value)
{
	Out.Add(static_cast<uint8>(Value & 0xff));
	Out.Add(static_cast<uint8>((Value >> 8) & 0xff));
	Out.Add(static_cast<uint8>((Value >> 16) & 0xff));
	Out.Add(static_cast<uint8>((Value >> 24) & 0xff));
}

void AppendF64Le(TArray<uint8>& Out, const double Value)
{
	static_assert(PLATFORM_LITTLE_ENDIAN, "URS image protocol currently requires a little-endian host");
	uint8 Bytes[sizeof(double)];
	FMemory::Memcpy(Bytes, &Value, sizeof(double));
	Out.Append(Bytes, sizeof(double));
}

TArray<uint8> BuildImageMessage(
	const uint32 Sequence,
	const double SimTime,
	const TArray<FURSImageEntry>& Entries)
{
	TArray<uint8> Packed;
	Packed.Add(URSoccerLab::TcpProtocol::ImageMessageVersion);
	Packed.Add(static_cast<uint8>(FMath::Min(Entries.Num(), 255)));
	AppendU16Le(Packed, 0); // reserved flags
	AppendU32Le(Packed, Sequence);
	AppendF64Le(Packed, SimTime);

	for (const FURSImageEntry& Entry : Entries)
	{
		FTCHARToUTF8 CameraNameUtf8(*Entry.CameraName);
		const int32 NameLength = FMath::Min(CameraNameUtf8.Length(), 255);
		Packed.Add(static_cast<uint8>(NameLength));
		Packed.Append(reinterpret_cast<const uint8*>(CameraNameUtf8.Get()), NameLength);
		Packed.Add(Entry.Codec);
		Packed.Add(Entry.PixelFormat);
		Packed.Add(0); // reserved
		AppendU16Le(Packed, Entry.Width);
		AppendU16Le(Packed, Entry.Height);
		AppendU32Le(Packed, Entry.UncompressedBytes);
		AppendU32Le(Packed, static_cast<uint32>(Entry.Data.Num()));
		Packed.Append(Entry.Data);
	}
	return Packed;
}

const FURSCameraInfo* FindCameraInfo(const FURSRobotState& State, const FString& Name)
{
	return State.Cameras.FindByPredicate(
		[&Name](const FURSCameraInfo& Info) { return Info.Name == Name; });
}
} // namespace

UURSTcpTransportComponent::UURSTcpTransportComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	NoiseRng.Initialize(1973);
}

void UURSTcpTransportComponent::BeginPlay()
{
	Super::BeginPlay();
	AsyncVisionState = MakeShared<FAsyncVisionState, ESPMode::ThreadSafe>();
	ImageWrapperModule =
		&FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

	if (AActor* Owner = GetOwner())
	{
		if (const UURSSceneConfigComponent* SceneConfig =
			Owner->FindComponentByClass<UURSSceneConfigComponent>())
		{
			VisionConfig = SceneConfig->GetActiveConfig().Vision;
		}
	}
	CameraRateHz = VisionConfig.Rgb.RateHz;
	CameraCompress = VisionConfig.Rgb.Compression == URSoccerLab::EURSRgbCompression::Jpeg
		? TEXT("jpeg") : TEXT("raw");
	JpegQuality = VisionConfig.Rgb.JpegQuality;
	DepthRateHz = VisionConfig.Depth.RateHz;
	switch (VisionConfig.Depth.Compression)
	{
	case URSoccerLab::EURSDepthCompression::RawFloat32:
		DepthCompress = TEXT("raw_f32");
		break;
	case URSoccerLab::EURSDepthCompression::RawUint16Millimeters:
		DepthCompress = TEXT("raw_u16_mm");
		break;
	case URSoccerLab::EURSDepthCompression::ZlibUint16Millimeters:
	default:
		DepthCompress = TEXT("zlib_u16_mm");
		break;
	}

	double RequestedCameraRateHz = CameraRateHz;
	if (FParse::Value(FCommandLine::Get(), TEXT("URSCameraRateHz="), RequestedCameraRateHz))
	{
		CameraRateHz = FMath::Clamp(RequestedCameraRateHz, 1.0, 120.0);
	}

	FString RequestedCameraCompress;
	if (FParse::Value(FCommandLine::Get(), TEXT("URSCameraCompress="), RequestedCameraCompress))
	{
		RequestedCameraCompress.ToLowerInline();
		if (RequestedCameraCompress == TEXT("jpeg") || RequestedCameraCompress == TEXT("raw"))
		{
			CameraCompress = MoveTemp(RequestedCameraCompress);
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[URS TCP] Ignoring unsupported camera codec '%s'; expected jpeg or raw."),
				*RequestedCameraCompress);
		}
	}

	int32 RequestedJpegQuality = JpegQuality;
	if (FParse::Value(FCommandLine::Get(), TEXT("URSJpegQuality="), RequestedJpegQuality))
	{
		JpegQuality = FMath::Clamp(RequestedJpegQuality, 1, 100);
	}

	double RequestedDepthRateHz = DepthRateHz;
	if (FParse::Value(FCommandLine::Get(), TEXT("URSDepthRateHz="), RequestedDepthRateHz))
	{
		DepthRateHz = FMath::Clamp(RequestedDepthRateHz, 1.0, 120.0);
	}

	FString RequestedDepthCompress;
	if (FParse::Value(FCommandLine::Get(), TEXT("URSDepthCompress="), RequestedDepthCompress))
	{
		RequestedDepthCompress.ToLowerInline();
		if (RequestedDepthCompress == TEXT("raw_f32")
			|| RequestedDepthCompress == TEXT("raw_u16_mm")
			|| RequestedDepthCompress == TEXT("zlib_u16_mm"))
		{
			DepthCompress = MoveTemp(RequestedDepthCompress);
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[URS TCP] Ignoring unsupported depth codec '%s'."),
				*RequestedDepthCompress);
		}
	}
	bLogCameraStats = FParse::Param(FCommandLine::Get(), TEXT("URSCameraStats"));
	CameraStatsWindowStartSec = FPlatformTime::Seconds();
	NextRgbTimeSec = CameraStatsWindowStartSec;
	NextDepthTimeSec = CameraStatsWindowStartSec;

	UE_LOG(LogTemp, Log,
		TEXT("[URS TCP] Vision transport: mode=%s rgb=%.2fHz/%s(q=%d) depth=%.2fHz/%s stats=%s."),
		VisionConfig.Mode == URSoccerLab::EURSVisionMode::Rgbd ? TEXT("rgbd") : TEXT("stereo_rgb"),
		CameraRateHz, *CameraCompress, JpegQuality,
		DepthRateHz, *DepthCompress,
		bLogCameraStats ? TEXT("on") : TEXT("off"));

	if (bAutoStart)
	{
		StartTransport();
	}
}

void UURSTcpTransportComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AsyncVisionState.IsValid())
	{
		AsyncVisionState->bAcceptResults.Store(false);
		AsyncVisionState.Reset();
	}
	StopTransport();
	Super::EndPlay(EndPlayReason);
}

bool UURSTcpTransportComponent::StartTransport()
{
	if (AActor* Owner = GetOwner())
	{
		Core = Owner->FindComponentByClass<UURSRobotCoreComponent>();
		NDisplayBinder =
			Owner->FindComponentByClass<UURSDisplayClusterCameraBinderComponent>();
	}

	if (!Core.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[URS TCP] No UURSRobotCoreComponent found."));
		return false;
	}

	Core->OnRobotsChanged.AddDynamic(this, &UURSTcpTransportComponent::OnRobotsChanged);

	RebuildListeners();
	UE_LOG(LogTemp, Log, TEXT("[URS TCP] Transport started."));
	return true;
}

void UURSTcpTransportComponent::OnRobotsChanged()
{
	RebuildListeners();
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
	if (!URSoccerLab::TcpProtocol::IsValidPortLayout(
		RobotBasePort, RobotIds.Num(), AdminPort))
	{
		UE_LOG(LogTemp, Error,
			TEXT("[URS TCP] Invalid port layout: robot range starts at %d for %d robot(s), admin=%d."),
			RobotBasePort, RobotIds.Num(), AdminPort);
		return;
	}

	ISocketSubsystem* SSS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

	auto CreateListener = [&](int32 Port) -> FSocket*
	{
		FSocket* ListenSock = SSS->CreateSocket(NAME_Stream, TEXT("URS"), false);
		if (!ListenSock) return nullptr;
		// Keep simulator endpoints process-exclusive. On Linux Unreal's
		// SetReuseAddr() also enables SO_REUSEPORT, which lets two Unreal
		// instances accept different connections on the same robot/admin ports
		// and silently mixes state, commands, and camera frames across worlds.
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
		L.Generation = ++ListenerGenerationCounter;
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
	if (!NDisplayBinder.IsValid() && GetOwner())
	{
		NDisplayBinder =
			GetOwner()->FindComponentByClass<UURSDisplayClusterCameraBinderComponent>();
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
	const double CameraPublishStartSec = FPlatformTime::Seconds();
	// Clear completed bounded jobs before deciding whether this render tick
	// can accept another frame. Draining afterwards artificially limits a
	// fast encoder to every second game frame.
	DrainCompletedVisionPackets();
	TickCameraPublish();
	DrainCompletedVisionPackets();
	CameraStatsPublishSec += FPlatformTime::Seconds() - CameraPublishStartSec;
	FlushAllWrites();

	if (bLogCameraStats)
	{
		++CameraStatsTickCount;
		const double Now = FPlatformTime::Seconds();
		const double WindowSec = Now - CameraStatsWindowStartSec;
		if (WindowSec >= 1.0)
		{
			UE_LOG(LogTemp, Log,
				TEXT("[URS CameraStats] tick_hz=%.2f message_hz=%.2f camera_hz=%.2f ")
				TEXT("publish_ms_per_sec=%.2f encode_ms_per_sec=%.2f payload_mib_per_sec=%.3f"),
				static_cast<double>(CameraStatsTickCount) / WindowSec,
				static_cast<double>(CameraStatsMessageCount) / WindowSec,
				static_cast<double>(CameraStatsEntryCount) / WindowSec,
				CameraStatsPublishSec * 1000.0 / WindowSec,
				CameraStatsEncodeSec * 1000.0 / WindowSec,
				static_cast<double>(CameraStatsPayloadBytes) / WindowSec / (1024.0 * 1024.0));

			CameraStatsWindowStartSec = Now;
			CameraStatsPublishSec = 0.0;
			CameraStatsEncodeSec = 0.0;
			CameraStatsTickCount = 0;
			CameraStatsMessageCount = 0;
			CameraStatsEntryCount = 0;
			CameraStatsPayloadBytes = 0;
		}
	}
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

			if (FrameType == URSoccerLab::TcpProtocol::TypeJson && PayloadSize > 0)
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
		EnqueueFrame(Client, URSoccerLab::TcpProtocol::TypeJson,
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
		SendToClients(L, URSoccerLab::TcpProtocol::TypeJson, reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
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
	auto AdvanceClock = [Now](double& NextTime, const double RateHz) -> bool
	{
		const double Interval = RateHz > 0.0 ? 1.0 / RateHz : 0.0;
		if (Interval <= 0.0 || Now < NextTime)
		{
			return false;
		}
		// Preserve a fixed-rate phase. Resetting to Now quantizes sensor
		// rates against the game tick and unnecessarily lowers throughput.
		do
		{
			NextTime += Interval;
		}
		while (NextTime <= Now);
		return true;
	};

	const bool bRequestRgb = AdvanceClock(NextRgbTimeSec, CameraRateHz);
	const bool bRequestDepth =
		VisionConfig.Mode == URSoccerLab::EURSVisionMode::Rgbd
		&& AdvanceClock(NextDepthTimeSec, DepthRateHz);
	const bool bUseNDisplay =
		NDisplayBinder.IsValid() && NDisplayBinder->IsReady();
	const bool bAnyVisionClient = RobotListeners.ContainsByPredicate(
		[](const FRobotListener& Listener)
		{
			return !Listener.Clients.IsEmpty();
		});

	if (bRequestRgb || bRequestDepth)
	{
		if (bRequestRgb && bUseNDisplay && bAnyVisionClient)
		{
			NDisplayBinder->RequestRgbFrame();
		}
		for (FRobotListener& Listener : RobotListeners)
		{
			if (Listener.Clients.IsEmpty()) continue;
			if (bRequestRgb && !bUseNDisplay)
			{
				Core->RequestNamedCameraReadback(Listener.ActorId, VisionConfig.LeftCamera);
				if (VisionConfig.Mode == URSoccerLab::EURSVisionMode::StereoRgb)
				{
					Core->RequestNamedCameraReadback(Listener.ActorId, VisionConfig.RightCamera);
				}
			}
			if (bRequestDepth)
			{
				Core->RequestNamedCameraReadback(Listener.ActorId, VisionConfig.RightCamera);
			}
		}
	}

	for (FRobotListener& Listener : RobotListeners)
	{
		if (Listener.Clients.IsEmpty()) continue;

		FURSRobotState State;
		if (!Core->GetRobotState(Listener.ActorId, State)) continue;

		TArray<FString> RgbCameraNames{VisionConfig.LeftCamera};
		if (VisionConfig.Mode == URSoccerLab::EURSVisionMode::StereoRgb)
		{
			RgbCameraNames.Add(VisionConfig.RightCamera);
		}

		const uint64 NDisplaySequence = bUseNDisplay
			? NDisplayBinder->GetLatestRgbFrameSequence()
			: 0;
		bool bRgbReady = bUseNDisplay
			? NDisplaySequence > Listener.LastNDisplayRgbSequence
			: true;
		if (!bUseNDisplay)
		{
			for (const FString& CameraName : RgbCameraNames)
			{
				bRgbReady =
					bRgbReady
					&& Core->IsCameraFrameReady(Listener.ActorId, CameraName);
			}
		}
		if (bRgbReady && !Listener.bRgbEncodeInFlight)
		{
			TArray<FURSRawRgbImage> RawImages;
			bool bValidRgbSet = true;
			uint64 CopiedNDisplaySequence = 0;
			for (const FString& CameraName : RgbCameraNames)
			{
				const FURSCameraInfo* Info = FindCameraInfo(State, CameraName);
				TArray<FColor> Pixels;
				int32 Width = Info ? Info->Width : 0;
				int32 Height = Info ? Info->Height : 0;
				uint64 ImageNDisplaySequence = 0;
				const bool bGotPixels = bUseNDisplay
					? NDisplayBinder->CopyRgbFrame(
						Listener.ActorId,
						CameraName,
						Listener.LastNDisplayRgbSequence,
						Pixels,
						Width,
						Height,
						ImageNDisplaySequence)
					: Core->ConsumeCameraFrame(
						Listener.ActorId,
						CameraName,
						Pixels);
				if (!Info || !bGotPixels || Pixels.Num() != Width * Height
					|| (bUseNDisplay && CopiedNDisplaySequence != 0
						&& CopiedNDisplaySequence != ImageNDisplaySequence))
				{
					bValidRgbSet = false;
					break;
				}
				CopiedNDisplaySequence = ImageNDisplaySequence;

				FURSRawRgbImage& Raw = RawImages.AddDefaulted_GetRef();
				Raw.CameraName = CameraName;
				Raw.Width = static_cast<uint16>(Width);
				Raw.Height = static_cast<uint16>(Height);
				Raw.Pixels = MoveTemp(Pixels);
			}

			if (bValidRgbSet && RawImages.Num() == RgbCameraNames.Num()
				&& AsyncVisionState.IsValid() && ImageWrapperModule)
			{
				if (bUseNDisplay)
				{
					Listener.LastNDisplayRgbSequence = CopiedNDisplaySequence;
				}
				Listener.bRgbEncodeInFlight = true;
				const FString ActorId = Listener.ActorId;
				const uint64 Generation = Listener.Generation;
				const uint32 Sequence = Listener.NextRgbSequence++;
				const double SimTime = State.SimTime;
				const bool bUseJpeg = CameraCompress == TEXT("jpeg");
				const int32 Quality = JpegQuality;
				IImageWrapperModule* WrapperModule = ImageWrapperModule;
				TSharedPtr<FAsyncVisionState, ESPMode::ThreadSafe> AsyncState =
					AsyncVisionState;

				Async(EAsyncExecution::ThreadPool,
					[ActorId, Generation, Sequence, SimTime, bUseJpeg, Quality,
					 WrapperModule, AsyncState, Images = MoveTemp(RawImages)]() mutable
					{
						const double EncodeStartSec = FPlatformTime::Seconds();
						FCompletedVisionPacket Completed;
						Completed.ActorId = ActorId;
						Completed.ListenerGeneration = Generation;
						Completed.FrameType = URSoccerLab::TcpProtocol::TypeRgb;

						TArray<FURSImageEntry> Entries;
						bool bSuccess = !Images.IsEmpty();
						for (FURSRawRgbImage& Raw : Images)
						{
							FURSImageEntry& Entry = Entries.AddDefaulted_GetRef();
							Entry.CameraName = MoveTemp(Raw.CameraName);
							Entry.PixelFormat = URSoccerLab::TcpProtocol::PixelFormatBgra8;
							Entry.Width = Raw.Width;
							Entry.Height = Raw.Height;
							Entry.UncompressedBytes =
								static_cast<uint32>(Raw.Pixels.Num() * sizeof(FColor));

							if (bUseJpeg)
							{
								TSharedPtr<IImageWrapper> Wrapper(
									WrapperModule->CreateImageWrapper(EImageFormat::JPEG));
								if (Wrapper.IsValid() && Wrapper->SetRaw(
									reinterpret_cast<const uint8*>(Raw.Pixels.GetData()),
									Entry.UncompressedBytes,
									Raw.Width,
									Raw.Height,
									ERGBFormat::BGRA,
									8))
								{
									Entry.Data = Wrapper->GetCompressed(Quality);
									Entry.Codec = URSoccerLab::TcpProtocol::ImageCodecJpeg;
								}
							}
							else
							{
								Entry.Data.Append(
									reinterpret_cast<const uint8*>(Raw.Pixels.GetData()),
									Entry.UncompressedBytes);
								Entry.Codec = URSoccerLab::TcpProtocol::ImageCodecRaw;
							}
							bSuccess = bSuccess && !Entry.Data.IsEmpty();
							Completed.ImagePayloadBytes += Entry.Data.Num();
						}

						Completed.EntryCount = Entries.Num();
						Completed.bSuccess = bSuccess;
						if (bSuccess)
						{
							Completed.Payload =
								BuildImageMessage(Sequence, SimTime, Entries);
						}
						Completed.EncodeSeconds =
							FPlatformTime::Seconds() - EncodeStartSec;
						if (AsyncState->bAcceptResults.Load())
						{
							AsyncState->CompletedPackets.Enqueue(MoveTemp(Completed));
						}
					});
			}
		}

		if (VisionConfig.Mode != URSoccerLab::EURSVisionMode::Rgbd
			|| Listener.bDepthEncodeInFlight
			|| !Core->IsCameraFrameReady(Listener.ActorId, VisionConfig.RightCamera))
		{
			continue;
		}

		const FURSCameraInfo* Info = FindCameraInfo(State, VisionConfig.RightCamera);
		TArray<float> DepthMeters;
		if (!Info
			|| !Core->ConsumeDepthCameraFrame(
				Listener.ActorId, VisionConfig.RightCamera, DepthMeters)
			|| DepthMeters.Num() != Info->Width * Info->Height)
		{
			continue;
		}

		if (AsyncVisionState.IsValid())
		{
			Listener.bDepthEncodeInFlight = true;
			const FString ActorId = Listener.ActorId;
			const uint64 Generation = Listener.Generation;
			const uint32 Sequence = Listener.NextDepthSequence++;
			const double SimTime = State.SimTime;
			const FString Compression = DepthCompress;
			const FString CameraName = VisionConfig.LeftCamera;
			const uint16 Width = static_cast<uint16>(Info->Width);
			const uint16 Height = static_cast<uint16>(Info->Height);
			const double MaxDepthMeters = VisionConfig.Depth.MaxDepthMeters;
			TSharedPtr<FAsyncVisionState, ESPMode::ThreadSafe> AsyncState =
				AsyncVisionState;

			Async(EAsyncExecution::ThreadPool,
				[ActorId, Generation, Sequence, SimTime, Compression, CameraName,
				 Width, Height, MaxDepthMeters, AsyncState,
				 Depth = MoveTemp(DepthMeters)]() mutable
				{
					const double EncodeStartSec = FPlatformTime::Seconds();
					FCompletedVisionPacket Completed;
					Completed.ActorId = ActorId;
					Completed.ListenerGeneration = Generation;
					Completed.FrameType = URSoccerLab::TcpProtocol::TypeDepth;
					Completed.EntryCount = 1;

					FURSImageEntry Entry;
					Entry.CameraName = CameraName;
					Entry.Width = Width;
					Entry.Height = Height;
					if (Compression == TEXT("raw_f32"))
					{
						Entry.Codec = URSoccerLab::TcpProtocol::ImageCodecRaw;
						Entry.PixelFormat =
							URSoccerLab::TcpProtocol::PixelFormatDepthFloat32Meters;
						Entry.UncompressedBytes =
							static_cast<uint32>(Depth.Num() * sizeof(float));
						Entry.Data.Append(
							reinterpret_cast<const uint8*>(Depth.GetData()),
							Entry.UncompressedBytes);
					}
					else
					{
						TArray<uint8> Quantized;
						Quantized.SetNumUninitialized(Depth.Num() * sizeof(uint16));
						const double MaxMillimeters = MaxDepthMeters * 1000.0;
						for (int32 Index = 0; Index < Depth.Num(); ++Index)
						{
							const double Millimeters = FMath::IsFinite(Depth[Index])
								? FMath::Clamp(
									static_cast<double>(Depth[Index]) * 1000.0,
									0.0,
									MaxMillimeters)
								: 0.0;
							const uint16 Value =
								static_cast<uint16>(FMath::RoundToInt(Millimeters));
							Quantized[Index * 2] =
								static_cast<uint8>(Value & 0xff);
							Quantized[Index * 2 + 1] =
								static_cast<uint8>((Value >> 8) & 0xff);
						}

						Entry.PixelFormat =
							URSoccerLab::TcpProtocol::PixelFormatDepthUint16Millimeters;
						Entry.UncompressedBytes =
							static_cast<uint32>(Quantized.Num());
						if (Compression == TEXT("zlib_u16_mm"))
						{
							int32 CompressedSize =
								FCompression::CompressMemoryBound(
									NAME_Zlib, Quantized.Num());
							Entry.Data.SetNumUninitialized(CompressedSize);
							if (FCompression::CompressMemory(
								NAME_Zlib,
								Entry.Data.GetData(),
								CompressedSize,
								Quantized.GetData(),
								Quantized.Num(),
								COMPRESS_BiasSpeed))
							{
								Entry.Data.SetNum(
									CompressedSize, EAllowShrinking::No);
								Entry.Codec = URSoccerLab::TcpProtocol::ImageCodecZlib;
							}
							else
							{
								Entry.Data = MoveTemp(Quantized);
								Entry.Codec = URSoccerLab::TcpProtocol::ImageCodecRaw;
							}
						}
						else
						{
							Entry.Data = MoveTemp(Quantized);
							Entry.Codec = URSoccerLab::TcpProtocol::ImageCodecRaw;
						}
					}

					Completed.bSuccess = !Entry.Data.IsEmpty();
					Completed.ImagePayloadBytes = Entry.Data.Num();
					if (Completed.bSuccess)
					{
						TArray<FURSImageEntry> Entries{MoveTemp(Entry)};
						Completed.Payload =
							BuildImageMessage(Sequence, SimTime, Entries);
					}
					Completed.EncodeSeconds =
						FPlatformTime::Seconds() - EncodeStartSec;
					if (AsyncState->bAcceptResults.Load())
					{
						AsyncState->CompletedPackets.Enqueue(MoveTemp(Completed));
					}
				});
		}
	}
}

void UURSTcpTransportComponent::DrainCompletedVisionPackets()
{
	if (!AsyncVisionState.IsValid())
	{
		return;
	}

	FCompletedVisionPacket Completed;
	while (AsyncVisionState->CompletedPackets.Dequeue(Completed))
	{
		FRobotListener* Listener = RobotListeners.FindByPredicate(
			[&Completed](const FRobotListener& Candidate)
			{
				return Candidate.ActorId == Completed.ActorId
					&& Candidate.Generation == Completed.ListenerGeneration;
			});
		if (!Listener)
		{
			continue;
		}

		if (Completed.FrameType == URSoccerLab::TcpProtocol::TypeRgb)
		{
			Listener->bRgbEncodeInFlight = false;
		}
		else if (Completed.FrameType == URSoccerLab::TcpProtocol::TypeDepth)
		{
			Listener->bDepthEncodeInFlight = false;
		}

		CameraStatsEncodeSec += Completed.EncodeSeconds;
		if (!Completed.bSuccess || Completed.Payload.IsEmpty())
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[URS TCP] Vision encode failed for '%s' (type=0x%02x)."),
				*Completed.ActorId,
				Completed.FrameType);
			continue;
		}

		CameraStatsEntryCount += Completed.EntryCount;
		CameraStatsPayloadBytes += Completed.ImagePayloadBytes;
		++CameraStatsMessageCount;
		if (!Listener->Clients.IsEmpty())
		{
			SendToClients(
				*Listener,
				Completed.FrameType,
				Completed.Payload.GetData(),
				Completed.Payload.Num());
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

	auto Gaussian = [this]() -> double
	{
		const double U1 = FMath::Max(NoiseRng.GetFraction(), 1e-7);
		const double U2 = NoiseRng.GetFraction();
		return FMath::Sqrt(-2.0 * FMath::Loge(U1)) * FMath::Cos(2.0 * UE_DOUBLE_PI * U2);
	};
	auto Noisy = [&Gaussian](double V, double Sigma) -> double
	{
		return Sigma > 0.0 ? V + Gaussian() * Sigma : V;
	};

	auto BaseObj = MakeShared<FJsonObject>();
	BaseObj->SetArrayField(TEXT("pos"), {
		MakeShared<FJsonValueNumber>(State.BasePos.X),
		MakeShared<FJsonValueNumber>(State.BasePos.Y),
		MakeShared<FJsonValueNumber>(State.BasePos.Z) });
	FQuat Quat = State.BaseQuat;
	if (State.Noise.ImuQuat > 0.0)
	{
		const double S = State.Noise.ImuQuat;
		Quat.W += Gaussian() * S;
		Quat.X += Gaussian() * S;
		Quat.Y += Gaussian() * S;
		Quat.Z += Gaussian() * S;
		Quat.Normalize();
	}
	BaseObj->SetArrayField(TEXT("quat"), {
		MakeShared<FJsonValueNumber>(Quat.W),
		MakeShared<FJsonValueNumber>(Quat.X),
		MakeShared<FJsonValueNumber>(Quat.Y),
		MakeShared<FJsonValueNumber>(Quat.Z) });
	TArray<TSharedPtr<FJsonValue>> VelArr;
	const int32 NV = State.BaseVel.Num();
	for (int32 i = 0; i < NV; ++i)
	{
		double V = State.BaseVel[i];
		if (State.Noise.ImuAngVel > 0.0 && NV == 6 && i >= 3)
		{
			V += Gaussian() * State.Noise.ImuAngVel;
		}
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
			JObj->SetNumberField(TEXT("qpos"), Noisy(State.JointQpos[i], State.Noise.Qpos));
			JObj->SetNumberField(TEXT("qvel"), Noisy(State.JointQvel[i], State.Noise.Qvel));
			JointsObj->SetObjectField(State.JointNames[i], JObj);
		}
	}
	Root->SetObjectField(TEXT("joints"), JointsObj);

	TSharedPtr<FJsonObject> ActuatorsObj = MakeShared<FJsonObject>();
	if (State.ActuatorNames.Num() == State.MotorCommand.Num())
	{
		for (int32 i = 0; i < State.ActuatorNames.Num(); ++i)
		{
			ActuatorsObj->SetNumberField(State.ActuatorNames[i], Noisy(State.MotorCommand[i], State.Noise.Qtor));
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

	if (State.bPrivSelfPos)
	{
		Root->SetArrayField(TEXT("self_pos"), {
			MakeShared<FJsonValueNumber>(Noisy(State.SelfPos.X, State.Noise.SelfPos)),
			MakeShared<FJsonValueNumber>(Noisy(State.SelfPos.Y, State.Noise.SelfPos)),
			MakeShared<FJsonValueNumber>(Noisy(State.SelfPos.Z, State.Noise.SelfPos)) });
	}
	if (State.bPrivBallPosRelated)
	{
		Root->SetArrayField(TEXT("ball_pos_related"), {
			MakeShared<FJsonValueNumber>(Noisy(State.BallPosRelated.X, State.Noise.BallPosRelated)),
			MakeShared<FJsonValueNumber>(Noisy(State.BallPosRelated.Y, State.Noise.BallPosRelated)),
			MakeShared<FJsonValueNumber>(Noisy(State.BallPosRelated.Z, State.Noise.BallPosRelated)) });
	}
	if (State.bPrivBallVelRelated)
	{
		Root->SetArrayField(TEXT("ball_vel_related"), {
			MakeShared<FJsonValueNumber>(Noisy(State.BallVelRelated.X, State.Noise.BallVelRelated)),
			MakeShared<FJsonValueNumber>(Noisy(State.BallVelRelated.Y, State.Noise.BallVelRelated)),
			MakeShared<FJsonValueNumber>(Noisy(State.BallVelRelated.Z, State.Noise.BallVelRelated)) });
	}
	if (State.bPrivAllPos)
	{
		TSharedPtr<FJsonObject> AllPosObj = MakeShared<FJsonObject>();
		for (const TPair<FString, FVector>& Pair : State.AllPos)
		{
			AllPosObj->SetArrayField(Pair.Key, {
				MakeShared<FJsonValueNumber>(Noisy(Pair.Value.X, State.Noise.AllPos)),
				MakeShared<FJsonValueNumber>(Noisy(Pair.Value.Y, State.Noise.AllPos)),
				MakeShared<FJsonValueNumber>(Noisy(Pair.Value.Z, State.Noise.AllPos)) });
		}
		Root->SetObjectField(TEXT("all_pos"), AllPosObj);
	}

	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root.ToSharedRef(), W);
	return Json;
}

#include "Network/URSNetworkThread.h"
#include "Network/URSTcpFraming.h"
#include "Transport/URSTcpProtocol.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
URSNetworkThread::URSNetworkThread()
{
}
URSNetworkThread::~URSNetworkThread()
{
	Stop();
}
bool URSNetworkThread::Start(TArray<FURSRobotChannel>&& Channels, const FURSNetworkConfig& InConfig)
{
	Stop();
	if (!URSoccerLab::TcpProtocol::IsValidPortLayout(InConfig.RobotBasePort, Channels.Num(), InConfig.AdminPort))
		return false;
	TSet<FString> ActorIds;
	for (const auto& Channel : Channels)
	{
		if (Channel.ActorId.IsEmpty() || ActorIds.Contains(Channel.ActorId))
			return false;
		ActorIds.Add(Channel.ActorId);
	}
	Config = InConfig;
	StateRateHz = Config.StateRateHz;
	LastStateTime = 0;
	for (auto& Channel : Channels)
	{
		FRobotEndpoint Endpoint;
		Endpoint.Channel = MoveTemp(Channel);
		Endpoints.Add(MoveTemp(Endpoint));
	}
	Channels.Empty();
	bRunning.store(true);
	Runnable = new FRunnableImpl(this);
	Thread = FRunnableThread::Create(Runnable, TEXT("URSNetworkThread"), 0, TPri_Normal);
	if (!Thread || !bRunning.load())
	{
		Stop();
		return false;
	}
	return true;
}
bool URSNetworkThread::FRunnableImpl::Init()
{
	const bool bReady = Owner->OpenListeners();
	Owner->bRunning.store(bReady);
	if (!bReady)
		Owner->CloseSockets();
	return bReady;
}
void URSNetworkThread::FRunnableImpl::Exit()
{
	Owner->CloseSockets();
}
bool URSNetworkThread::OpenListeners()
{
	for (int32 Index = 0; Index < Endpoints.Num(); ++Index)
	{
		const int32 Port = Config.RobotBasePort + Index;
		if (!Endpoints[Index].Listener.Listen(Port))
		{
			UE_LOG(LogTemp, Error, TEXT("[URS TCP] Failed robot listener on port %d"), Port);
			return false;
		}
		UE_LOG(LogTemp, Log, TEXT("[URS TCP] Robot '%s' listening on port %d"), *Endpoints[Index].Channel.ActorId,
		       Port);
	}
	if (Config.AdminPort > 0)
	{
		if (!AdminListener.Listen(Config.AdminPort))
		{
			UE_LOG(LogTemp, Error, TEXT("[URS TCP] Failed admin listener on port %d"), Config.AdminPort);
			return false;
		}
		UE_LOG(LogTemp, Log, TEXT("[URS TCP] Admin listening on port %d"), Config.AdminPort);
	}
	return true;
}
void URSNetworkThread::CloseSockets()
{
	for (auto& Endpoint : Endpoints)
	{
		Endpoint.Clients.Empty();
		Endpoint.Listener.Close();
	}
	AdminClients.Empty();
	AdminListener.Close();
	bCameraSubscribers.store(false);
}
void URSNetworkThread::Stop()
{
	bRunning.store(false);
	bCameraSubscribers.store(false);
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	delete Runnable;
	Runnable = nullptr;
	Endpoints.Empty();
	AdminClients.Empty();
	URSoccerLab::FAdminMessage Discard;
	while (AdminRequests.Dequeue(Discard))
	{
	}
	while (AdminReplies.Dequeue(Discard))
	{
	}
	PendingAdminRequests.store(0);
	bCameraSubscribers.store(false);
	FScopeLock Lock(&CameraMutex);
	CameraFrames.Empty();
}
void URSNetworkThread::EnqueueCameraFrame(const URSoccerLab::FEncodedCameraFrame& Frame)
{
	if (!bRunning.load() || !Endpoints.ContainsByPredicate([&](const FRobotEndpoint& Endpoint) {
		    return Endpoint.Channel.ActorId == Frame.ActorId;
	    }))
		return;
	FScopeLock Lock(&CameraMutex);
	CameraFrames.Add(Frame.ActorId, Frame);
}
bool URSNetworkThread::DequeueAdminRequest(URSoccerLab::FAdminMessage& Out)
{
	if (!AdminRequests.Dequeue(Out))
		return false;
	PendingAdminRequests.fetch_sub(1);
	return true;
}
void URSNetworkThread::EnqueueAdminReply(uint64 ClientId, const FString& Json)
{
	AdminReplies.Enqueue({ClientId, Json});
}
uint32 URSNetworkThread::FRunnableImpl::Run()
{
	while (Owner->bRunning.load(std::memory_order_acquire))
	{
		Owner->AcceptConnections();
		Owner->ReadFromClients();
		Owner->DrainAdminReplies();
		Owner->PublishStates();
		Owner->DrainCameraQueues();
		Owner->FlushWrites();
		FPlatformProcess::Sleep(0.001); // tight loop — rates gated inside
	}
	return 0;
}

// ============================================================================
// Accept
// ============================================================================

void URSNetworkThread::AcceptConnections()
{
	for (FRobotEndpoint& Ep : Endpoints)
	{
		for (int32 Accepted = 0; Accepted < 16 && Ep.Listener.HasNewConnection(); ++Accepted)
		{
			FClient NewClient;
			if (Ep.Listener.Accept(NewClient.Socket))
			{
				NewClient.bConnected = true;
				Ep.Clients.Add(MoveTemp(NewClient));
			}
			else
				break;
		}
	}
	for (int32 Accepted = 0; Accepted < 16 && AdminListener.HasNewConnection(); ++Accepted)
	{
		FClient Client;
		if (AdminListener.Accept(Client.Socket))
		{
			Client.ClientId = NextClientId++;
			AdminClients.Add(MoveTemp(Client));
		}
		else
			break;
	}
}

// ============================================================================
// Read + parse frames (robot resynchronization, bounded admin requests)
// ============================================================================

void URSNetworkThread::ReadFromClients()
{
	for (int32 Index = 0; Index < Endpoints.Num(); ++Index)
		ReadClients(Endpoints[Index].Clients, Index);
	ReadClients(AdminClients, INDEX_NONE);
}
void URSNetworkThread::ReadClients(TArray<FClient>& Clients, int32 RobotIndex)
{
	uint8 Buffer[65536];
	for (int32 Index = Clients.Num() - 1; Index >= 0; --Index)
	{
		auto& Client = Clients[Index];
		for (int32 Read = 0; Read < 16; ++Read)
		{
			int32 Count = Client.Socket.Recv(Buffer, sizeof(Buffer));
			if (Count < 0)
			{
				Client.bConnected = false;
				break;
			}
			if (Count == 0)
				break;
			Client.ReadBuf.Append(Buffer, Count);
			if (Client.ReadBuf.Num() > URSoccerLab::TcpFraming::MaxFrameLength + 4)
			{
				Client.bConnected = false;
				break;
			}
		}
		int32 Consumed = 0;
		for (int32 Parsed = 0; Parsed < 64 && Client.bConnected && Client.ReadBuf.Num() - Consumed >= 5; ++Parsed)
		{
			const uint8* Data = Client.ReadBuf.GetData() + Consumed;
			const uint32 Length = URSoccerLab::TcpFraming::ReadLength(Data);
			if (Length < 1 || Length > URSoccerLab::TcpFraming::MaxFrameLength)
			{
				if (RobotIndex == INDEX_NONE)
				{
					Client.bConnected = false;
					break;
				}
				++Consumed;
				continue;
			}
			if (uint32(Client.ReadBuf.Num() - Consumed) < Length + 4)
				break;
			if (Data[4] == 0)
			{
				if (RobotIndex != INDEX_NONE)
				{
					auto& Endpoint = Endpoints[RobotIndex];
					URSoccerLab::FRobotMessage Message;
					if (URSoccerLab::FMessageProtocol::DecodeRobotMessage(Endpoint.Channel.Meta.ActuatorNames, Data + 5,
					                                                      Length - 1, Message))
						PublishRobotMessage(Endpoint.Channel, Message.bControllerParams ? nullptr : &Message.Command,
						                    Message.bControllerParams ? &Message.Gains : nullptr,
						                    Endpoint.AccumulatedGains);
				}
				else
				{
					if (PendingAdminRequests.load() >= 256)
					{
						Client.bConnected = false;
						break;
					}
					FUTF8ToTCHAR Json(reinterpret_cast<const ANSICHAR*>(Data + 5), Length - 1);
					PendingAdminRequests.fetch_add(1);
					AdminRequests.Enqueue({Client.ClientId, FString(Json.Length(), Json.Get())});
				}
			}
			Consumed += Length + 4;
		}
		if (!Client.bConnected)
		{
			Clients.RemoveAt(Index);
			continue;
		}
		if (Consumed > 0)
			Client.ReadBuf.RemoveAt(0, Consumed);
	}
}
void URSNetworkThread::PublishStates()
{
	const double Now = FPlatformTime::Seconds();
	const double Interval = StateRateHz > 0 ? 1.0 / StateRateHz : 0.016;
	if (Now - LastStateTime < Interval)
		return;
	LastStateTime = Now;

	for (FRobotEndpoint& Ep : Endpoints)
	{
		if (Ep.Clients.Num() == 0)
			continue;
		if (!Ep.Channel.StateBuf)
			continue;

		const FRobotSnapshot& Snap = Ep.Channel.StateBuf->Front();
		TArray<uint8> Json = URSoccerLab::FMessageProtocol::EncodeState(Snap, Ep.Channel.Meta);

		for (auto& Client : Ep.Clients)
			if (Client.bConnected)
				URSoccerLab::TcpFraming::Append(Client.WriteBuf, 0x00, Json.GetData(), Json.Num());
	}
}

void URSNetworkThread::DrainCameraQueues()
{
	TMap<FString, URSoccerLab::FEncodedCameraFrame> Frames;
	{
		FScopeLock Lock(&CameraMutex);
		Swap(Frames, CameraFrames);
	}
	for (auto& Endpoint : Endpoints)
	{
		const auto* Frame = Frames.Find(Endpoint.Channel.ActorId);
		if (!Frame || Endpoint.Clients.IsEmpty())
			continue;
		auto Payload = URSoccerLab::FMessageProtocol::EncodeCamera(*Frame);
		if (Payload.IsEmpty())
			continue;
		for (auto& Client : Endpoint.Clients)
			if (Client.bConnected)
				Client.PendingCameraPayload = Payload;
	}
}
void URSNetworkThread::DrainAdminReplies()
{
	URSoccerLab::FAdminMessage Reply;
	while (AdminReplies.Dequeue(Reply))
	{
		for (auto& Client : AdminClients)
			if (Client.ClientId == Reply.ClientId)
			{
				FTCHARToUTF8 Bytes(*Reply.Json);
				URSoccerLab::TcpFraming::Append(Client.WriteBuf, 0, reinterpret_cast<const uint8*>(Bytes.Get()),
				                                Bytes.Length());
				break;
			}
	}
}
void URSNetworkThread::FlushWrites()
{
	for (auto& Endpoint : Endpoints)
		FlushClients(Endpoint.Clients);
	FlushClients(AdminClients);
	bool bHasClients = false;
	for (const auto& Endpoint : Endpoints)
		bHasClients |= !Endpoint.Clients.IsEmpty();
	bCameraSubscribers.store(bHasClients);
}
void URSNetworkThread::FlushClients(TArray<FClient>& Clients)
{
	for (int32 Index = Clients.Num() - 1; Index >= 0; --Index)
	{
		auto& Client = Clients[Index];
		auto SendBuffered = [&]() {
			if (Client.WriteBuf.IsEmpty())
				return true;
			const int32 Sent = Client.Socket.Send(Client.WriteBuf.GetData(), Client.WriteBuf.Num());
			if (Sent < 0)
				return false;
			if (Sent > 0)
				Client.WriteBuf.RemoveAt(0, Sent);
			return true;
		};
		if (!SendBuffered())
		{
			UE_LOG(LogTemp, Log, TEXT("[URS TCP] Client disconnected during send"));
			Clients.RemoveAt(Index);
			continue;
		}
		// Replace only frames that have not entered the byte stream. A partially
		// sent frame must finish before another video frame can be framed/sent.
		if (Client.WriteBuf.IsEmpty() && !Client.PendingCameraPayload.IsEmpty())
		{
			URSoccerLab::TcpFraming::Append(Client.WriteBuf, 1, Client.PendingCameraPayload.GetData(),
			                                Client.PendingCameraPayload.Num());
			Client.PendingCameraPayload.Empty();
			if (!SendBuffered())
			{
				Clients.RemoveAt(Index);
				continue;
			}
		}
		if (Client.WriteBuf.Num() > 4 * 1024 * 1024)
		{
			UE_LOG(LogTemp, Log, TEXT("[URS TCP] Client backpressure disconnect (queued=%d)"), Client.WriteBuf.Num());
			Clients.RemoveAt(Index);
		}
	}
}

#include "Network/URSNetworkThread.h"
#include "Misc/CommandLine.h"
#include "HAL/PlatformTime.h"

URSNetworkThread::URSNetworkThread() {}
URSNetworkThread::~URSNetworkThread() { Stop(); }

void URSNetworkThread::Start(TArray<FRobotEndpoint> InEndpoints, int32 AdminPort,
	double InStateRateHz, double InCameraRateHz)
{
	Endpoints = MoveTemp(InEndpoints);
	StateRateHz = InStateRateHz;
	CameraRateHz = InCameraRateHz;

	// Open TCP listeners
	for (FRobotEndpoint& Ep : Endpoints)
	{
		// Port assigned by caller (10000 + robot index)
		// Listener.Bind is done by the transport component before Start()
	}

	// Admin listener
	if (AdminPort > 0)
	{
		AdminListener.Listen(AdminPort);
	}

	bRunning.store(true);
	Runnable = new FRunnableImpl(this);
	Thread = FRunnableThread::Create(Runnable, TEXT("URSNetworkThread"), 0,
		TPri_Normal);
}

void URSNetworkThread::Stop()
{
	bRunning.store(false);
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	if (Runnable)
	{
		delete Runnable;
		Runnable = nullptr;
	}
	for (FRobotEndpoint& Ep : Endpoints)
	{
		for (auto& C : Ep.Clients)
			C.Socket.Close();
		Ep.Listener.Close();
	}
	AdminListener.Close();
	for (auto& C : AdminClients)
		C.Socket.Close();
}

void URSNetworkThread::EnqueueCameraFrame(int32 RobotIdx, uint8 FrameType,
	const uint8* Data, int32 Len)
{
	if (RobotIdx < 0 || RobotIdx >= Endpoints.Num()) return;
	FRobotEndpoint::FCameraPacket Pkt;
	Pkt.FrameType = FrameType;
	Pkt.Payload.Append(Data, Len);
	Endpoints[RobotIdx].CameraQueue.Enqueue(MoveTemp(Pkt));
}

uint32 URSNetworkThread::FRunnableImpl::Run()
{
	const double Interval = Owner->StateRateHz > 0 ? 1.0 / Owner->StateRateHz : 0.001;
	while (Owner->bRunning.load(std::memory_order_acquire))
	{
		double T0 = FPlatformTime::Seconds();
		Owner->Tick();
		double Elapsed = FPlatformTime::Seconds() - T0;
		double SleepTime = Interval - Elapsed;
		if (SleepTime > 0.001)
			FPlatformProcess::Sleep(SleepTime);
	}
	return 0;
}

void URSNetworkThread::Tick()
{
	AcceptConnections();
	ReadFromClients();
	PublishStates();
	DrainCameraQueues();
	HandleAdmin();
	FlushWrites();
}

void URSNetworkThread::AcceptConnections()
{
	for (FRobotEndpoint& Ep : Endpoints)
	{
		while (Ep.Listener.HasNewConnection())
		{
			FRobotEndpoint::FClient NewClient;
			if (Ep.Listener.Accept(NewClient))
			{
				NewClient.bConnected = true;
				Ep.Clients.Add(MoveTemp(NewClient));
			}
		}
	}
	while (AdminListener.HasNewConnection())
	{
		FAdminClient NewClient;
		if (AdminListener.Accept(NewClient.Socket))
		{
			NewClient.bConnected = true;
			AdminClients.Add(MoveTemp(NewClient));
		}
	}
}

void URSNetworkThread::ReadFromClients()
{
	uint8 RecvBuf[65536];
	for (int32 Ri = 0; Ri < Endpoints.Num(); ++Ri)
	{
		FRobotEndpoint& Ep = Endpoints[Ri];
		for (int32 Ci = Ep.Clients.Num() - 1; Ci >= 0; --Ci)
		{
			auto& Client = Ep.Clients[Ci];
			while (true)
			{
				int32 N = Client.Socket.Recv(RecvBuf, sizeof(RecvBuf));
				if (N > 0)
					Client.ReadBuf.Append(RecvBuf, N);
				else if (N < 0)
				{
					Client.Socket.Close();
					Client.bConnected = false;
					break;
				}
				else
					break;  // no more data
			}

			// Parse complete frames from ReadBuf
			TArray<uint8> OutFrames;
			FrameClientRead(Client.Socket, Client.ReadBuf, OutFrames);
			(void)OutFrames;  // frames are parsed inline below

			// Parse frames from ReadBuf
			int32 Consumed = 0;
			while (Client.ReadBuf.Num() - Consumed >= 5)
			{
				const uint8* D = Client.ReadBuf.GetData() + Consumed;
				int32 FrameLen = (D[0] << 24) | (D[1] << 16) | (D[2] << 8) | D[3];
				if (FrameLen < 1 || FrameLen > 16 * 1024 * 1024)
				{
					// Bad frame — drop client
					Client.Socket.Close();
					Client.bConnected = false;
					break;
				}
				if (Client.ReadBuf.Num() - Consumed < 4 + FrameLen)
					break;  // incomplete

				uint8 FrameType = D[4];
				const uint8* Payload = D + 5;
				int32 PayloadLen = FrameLen - 1;

				if (FrameType == 0x00)  // JSON
				{
					ProcessClientData(Ri, Ci, Payload, PayloadLen);
				}

				Consumed += 4 + FrameLen;
			}
			if (Consumed > 0)
				Client.ReadBuf.RemoveAt(0, Consumed);

			// Remove disconnected clients
			if (!Client.bConnected)
				Ep.Clients.RemoveAt(Ci);
		}
	}
}

void URSNetworkThread::ProcessClientData(int32 RobotIdx, int32 ClientIdx,
	const uint8* Data, int32 Len)
{
	FRobotEndpoint& Ep = Endpoints[RobotIdx];

	// Check if this is controller params or a command
	if (URSJsonParser::IsControllerParams(Data, Len))
	{
		FGainSet Gains;
		if (URSJsonParser::ParseGainParams(Data, Len, Gains, Ep.Meta.ActuatorNames))
		{
			Gains.bValid = true;
			Ep.GainBuf->Back() = Gains;
			Ep.GainBuf->Publish();
		}
	}
	else
	{
		// Regular command: parse flat name→float map
		yyjson_doc* Doc = yyjson_read((const char*)Data, Len, YYJSON_READ_NOFLAG);
		if (!Doc) return;
		yyjson_val* Root = yyjson_doc_get_root(Doc);
		if (!Root || !yyjson_is_obj(Root)) { yyjson_doc_free(Doc); return; }

		FCommandSet Cmd;
		Cmd.bValid = true;
		Cmd.TimestampSec = FPlatformTime::Seconds();

		const int32 N = FMath::Min(Ep.Meta.ActuatorNames.Num(), URS_MAX_ACTUATORS);
		yyjson_obj_iter Iter;
		yyjson_obj_iter_init(Root, &Iter);
		yyjson_val* KeyVal;
		while ((KeyVal = yyjson_obj_iter_next(&Iter)))
		{
			yyjson_val* V = yyjson_obj_iter_get_val(KeyVal);
			if (!yyjson_is_num(V)) continue;
			const char* K = yyjson_get_str(KeyVal);
			FString FName(UTF8_TO_TCHAR(K));
			for (int32 i = 0; i < N; ++i)
			{
				if (Ep.Meta.ActuatorNames[i] == FName)
				{
					Cmd.Targets[i] = (float)yyjson_get_num(V);
					break;
				}
			}
		}
		yyjson_doc_free(Doc);

		Ep.CmdBuf->Back() = Cmd;
		Ep.CmdBuf->Publish();
	}
}

void URSNetworkThread::PublishStates()
{
	const double Now = FPlatformTime::Seconds();
	const double Interval = StateRateHz > 0 ? 1.0 / StateRateHz : 0.0;
	if (Interval <= 0 || Now - LastStateTime < Interval) return;
	LastStateTime = Now;

	for (FRobotEndpoint& Ep : Endpoints)
	{
		if (Ep.Clients.Num() == 0) continue;

		const FRobotSnapshot& Snap = Ep.StateBuf->Front();
		TArray<uint8> Json = URSJsonBuilder::BuildStateJson(Snap, Ep.Meta);

		for (auto& Client : Ep.Clients)
		{
			if (Client.bConnected)
				EnqueueFrame(Client.WriteBuf, 0x00, Json.GetData(), Json.Num());
		}
	}
}

void URSNetworkThread::DrainCameraQueues()
{
	for (FRobotEndpoint& Ep : Endpoints)
	{
		if (Ep.Clients.Num() == 0) continue;

		FRobotEndpoint::FCameraPacket Pkt;
		while (Ep.CameraQueue.Dequeue(Pkt))
		{
			for (auto& Client : Ep.Clients)
			{
				if (Client.bConnected)
					EnqueueFrame(Client.WriteBuf, Pkt.FrameType,
						Pkt.Payload.GetData(), Pkt.Payload.Num());
			}
		}
	}
}

void URSNetworkThread::FlushWrites()
{
	for (FRobotEndpoint& Ep : Endpoints)
	{
		for (int32 Ci = Ep.Clients.Num() - 1; Ci >= 0; --Ci)
		{
			auto& Client = Ep.Clients[Ci];
			if (Client.WriteBuf.Num() > 0)
			{
				int32 Sent = Client.Socket.Send(
					Client.WriteBuf.GetData(), Client.WriteBuf.Num());
				if (Sent < 0)
				{
					Client.Socket.Close();
					Client.bConnected = false;
					Ep.Clients.RemoveAt(Ci);
					continue;
				}
				if (Sent > 0)
					Client.WriteBuf.RemoveAt(0, Sent);
				// If WriteBuf still has data, keep it for next tick
				// (latest-frame-only: drop new state if buffer non-empty)
				if (Client.WriteBuf.Num() > 4 * 1024 * 1024)
				{
					// Too much backlog — drop oldest state frame
					Client.WriteBuf.Reset();
				}
			}
		}
	}

	// Admin flush
	for (int32 Ci = AdminClients.Num() - 1; Ci >= 0; --Ci)
	{
		auto& Client = AdminClients[Ci];
		if (Client.WriteBuf.Num() > 0)
		{
			int32 Sent = Client.Socket.Send(
				Client.WriteBuf.GetData(), Client.WriteBuf.Num());
			if (Sent < 0)
			{
				Client.Socket.Close();
				AdminClients.RemoveAt(Ci);
				continue;
			}
			if (Sent > 0)
				Client.WriteBuf.RemoveAt(0, Sent);
		}
	}
}

void URSNetworkThread::HandleAdmin()
{
	// TODO: admin command handling (set_pose, reset, lock_pose)
	// For now, just recv and discard
	uint8 RecvBuf[4096];
	for (int32 Ci = AdminClients.Num() - 1; Ci >= 0; --Ci)
	{
		auto& Client = AdminClients[Ci];
		int32 N = Client.Socket.Recv(RecvBuf, sizeof(RecvBuf));
		if (N < 0)
		{
			Client.Socket.Close();
			AdminClients.RemoveAt(Ci);
		}
	}
}

void URSNetworkThread::FrameClientRead(URSNonBlockingSocket& Sock,
	TArray<uint8>& ReadBuf, TArray<uint8>& OutFrames)
{
	// This is a no-op placeholder — frame parsing is done inline in ReadFromClients.
}

void URSNetworkThread::EnqueueFrame(TArray<uint8>& WriteBuf, uint8 Type,
	const uint8* Data, int32 Len)
{
	// Frame format: [4-byte BE length][1-byte type][payload]
	int32 FrameLen = 1 + Len;
	WriteBuf.Add((FrameLen >> 24) & 0xFF);
	WriteBuf.Add((FrameLen >> 16) & 0xFF);
	WriteBuf.Add((FrameLen >> 8) & 0xFF);
	WriteBuf.Add(FrameLen & 0xFF);
	WriteBuf.Add(Type);
	WriteBuf.Append(Data, Len);
}

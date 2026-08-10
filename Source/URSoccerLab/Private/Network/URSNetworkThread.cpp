#include "Network/URSNetworkThread.h"
#include <yyjson.h>
#include "HAL/PlatformTime.h"

URSNetworkThread::URSNetworkThread() {}
URSNetworkThread::~URSNetworkThread() { Stop(); }

void URSNetworkThread::Start(TArray<FRobotEndpoint>&& InEndpoints, int32 AdminPort,
	double InStateRateHz, double InCameraRateHz)
{
	Endpoints = MoveTemp(InEndpoints);
	StateRateHz = InStateRateHz;
	CameraRateHz = InCameraRateHz;
	bRunning.store(true);
	Runnable = new FRunnableImpl(this);
	Thread = FRunnableThread::Create(Runnable, TEXT("URSNetworkThread"), 0, TPri_Normal);
	UE_LOG(LogTemp, Log, TEXT("[NET] Started: %d endpoints, state=%.0f cam=%.0f"),
		Endpoints.Num(), StateRateHz, CameraRateHz);
}

void URSNetworkThread::Stop()
{
	bRunning.store(false);
	if (Thread) { Thread->WaitForCompletion(); delete Thread; Thread = nullptr; }
	if (Runnable) { delete Runnable; Runnable = nullptr; }
	for (FRobotEndpoint& Ep : Endpoints)
	{
		for (auto& C : Ep.Clients) C.Socket.Close();
		Ep.Listener.Close();
	}
	AdminListener.Close();
	for (auto& C : AdminClients) C.Socket.Close();
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

// ============================================================================
// Thread loop
// ============================================================================

uint32 URSNetworkThread::FRunnableImpl::Run()
{
	while (Owner->bRunning.load(std::memory_order_acquire))
	{
		Owner->AcceptConnections();
		Owner->ReadFromClients();
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
		while (Ep.Listener.HasNewConnection())
		{
			FRobotEndpoint::FClient NewClient;
			if (Ep.Listener.Accept(NewClient.Socket))
			{
				NewClient.bConnected = true;
				Ep.Clients.Add(MoveTemp(NewClient));
			}
		}
	}
}

// ============================================================================
// Read + parse frames (resilient — never disconnects on bad frame data)
// ============================================================================

void URSNetworkThread::ReadFromClients()
{
	uint8 RecvBuf[65536];
	for (int32 Ri = 0; Ri < Endpoints.Num(); ++Ri)
	{
		FRobotEndpoint& Ep = Endpoints[Ri];
		for (int32 Ci = Ep.Clients.Num() - 1; Ci >= 0; --Ci)
		{
			auto& Client = Ep.Clients[Ci];

			// Non-blocking recv
			for (;;)
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
					break;
			}

			if (!Client.bConnected)
			{
				Ep.Clients.RemoveAt(Ci);
				continue;
			}

			// Parse complete frames from ReadBuf
			int32 Consumed = 0;
			while (Client.ReadBuf.Num() - Consumed >= 5)
			{
				const uint8* D = Client.ReadBuf.GetData() + Consumed;
				int32 FrameLen = (D[0] << 24) | (D[1] << 16) | (D[2] << 8) | D[3];
				if (FrameLen < 1 || FrameLen > 16 * 1024 * 1024)
				{
					// Bad frame — skip 1 byte and resync (don't disconnect)
					++Consumed;
					continue;
				}
				if (Client.ReadBuf.Num() - Consumed < 4 + FrameLen)
					break; // incomplete — wait for more data

				uint8 FrameType = D[4];
				if (FrameType == 0x00) // JSON
					ProcessClientData(Ri, D + 5, FrameLen - 1);

				Consumed += 4 + FrameLen;
			}
			if (Consumed > 0)
				Client.ReadBuf.RemoveAt(0, Consumed);
		}
	}
}

void URSNetworkThread::ProcessClientData(int32 RobotIdx,
	const uint8* Data, int32 Len)
{
	FRobotEndpoint& Ep = Endpoints[RobotIdx];

	if (URSJsonParser::IsControllerParams(Data, Len))
	{
		FGainSet Gains;
		if (URSJsonParser::ParseGainParams(Data, Len, Gains, Ep.Meta.ActuatorNames))
		{
			Gains.bValid = true;
			if (Ep.GainBuf) { Ep.GainBuf->Back() = Gains; Ep.GainBuf->Publish(); }
		}
	}
	else
	{
		FCommandSet Cmd;
		Cmd.bValid = true;
		Cmd.TimestampSec = FPlatformTime::Seconds();

		yyjson_doc* Doc = yyjson_read((const char*)Data, Len, YYJSON_READ_NOFLAG);
		if (!Doc) return;
		yyjson_val* Root = yyjson_doc_get_root(Doc);
		if (!Root || !yyjson_is_obj(Root)) { yyjson_doc_free(Doc); return; }

		const int32 N = FMath::Min(Ep.Meta.ActuatorNames.Num(), URS_MAX_ACTUATORS);
		yyjson_obj_iter Iter;
		yyjson_obj_iter_init(Root, &Iter);
		yyjson_val* KeyVal;
		while ((KeyVal = yyjson_obj_iter_next(&Iter)))
		{
			yyjson_val* V = yyjson_obj_iter_get_val(KeyVal);
			if (!yyjson_is_num(V)) continue;
			const char* K = yyjson_get_str(KeyVal);
			if (!K) continue;
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

		if (Ep.CmdBuf) { Ep.CmdBuf->Back() = Cmd; Ep.CmdBuf->Publish(); }
	}
}

// ============================================================================
// Publish state (yyjson)
// ============================================================================

static double s_LastStatePublish = 0.0;

void URSNetworkThread::PublishStates()
{
	const double Now = FPlatformTime::Seconds();
	const double Interval = StateRateHz > 0 ? 1.0 / StateRateHz : 0.016;
	if (Now - s_LastStatePublish < Interval) return;
	s_LastStatePublish = Now;

	for (FRobotEndpoint& Ep : Endpoints)
	{
		if (Ep.Clients.Num() == 0) continue;
		if (!Ep.StateBuf) continue;

		const FRobotSnapshot& Snap = Ep.StateBuf->Front();
		TArray<uint8> Json = URSJsonBuilder::BuildStateJson(Snap, Ep.Meta);

		for (auto& Client : Ep.Clients)
			if (Client.bConnected)
				EnqueueFrame(Client.WriteBuf, 0x00, Json.GetData(), Json.Num());
	}
}

// ============================================================================
// Camera queue drain
// ============================================================================

void URSNetworkThread::DrainCameraQueues()
{
	for (FRobotEndpoint& Ep : Endpoints)
	{
		if (Ep.Clients.Num() == 0) continue;
		FRobotEndpoint::FCameraPacket Pkt;
		while (Ep.CameraQueue.Dequeue(Pkt))
		{
			for (auto& Client : Ep.Clients)
				if (Client.bConnected)
					EnqueueFrame(Client.WriteBuf, Pkt.FrameType,
						Pkt.Payload.GetData(), Pkt.Payload.Num());
		}
	}
}

// ============================================================================
// Flush TCP (non-blocking)
// ============================================================================

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
					Ep.Clients.RemoveAt(Ci);
					continue;
				}
				if (Sent > 0)
					Client.WriteBuf.RemoveAt(0, Sent);
				if (Client.WriteBuf.Num() > 4 * 1024 * 1024)
					Client.WriteBuf.Reset();
			}
		}
	}
}

void URSNetworkThread::HandleAdmin() {}

void URSNetworkThread::FrameClientRead(URSNonBlockingSocket&,
	TArray<uint8>&, TArray<uint8>&) {}

void URSNetworkThread::EnqueueFrame(TArray<uint8>& WriteBuf, uint8 Type,
	const uint8* Data, int32 Len)
{
	int32 FrameLen = 1 + Len;
	WriteBuf.Add((FrameLen >> 24) & 0xFF);
	WriteBuf.Add((FrameLen >> 16) & 0xFF);
	WriteBuf.Add((FrameLen >> 8) & 0xFF);
	WriteBuf.Add(FrameLen & 0xFF);
	WriteBuf.Add(Type);
	WriteBuf.Append(Data, Len);
}

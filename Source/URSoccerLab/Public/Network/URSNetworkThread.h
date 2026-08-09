#pragma once

// URSNetworkThread.h — dedicated network thread for TCP send/recv.
// Decoupled from both the physics thread and the render (game) thread.
// Runs at a configurable rate (default 60 Hz), independent of render FPS.

#include "CoreMinimal.h"
#include "Core/URSTripleBuffer.h"
#include "Core/URSBuffers.h"
#include "URSSnapshot.h"
#include "URSJson.h"
#include "Network/URSSocket.h"
#include "Containers/Queue.h"

class URSNetworkThread
{
public:
	struct FRobotEndpoint
	{
		FRobotEndpoint() = default;
		FRobotEndpoint(FRobotEndpoint&& Other)
			: ActorId(MoveTemp(Other.ActorId))
			, Listener(MoveTemp(Other.Listener))
			, Clients(MoveTemp(Other.Clients))
			, StateBuf(Other.StateBuf), CmdBuf(Other.CmdBuf), GainBuf(Other.GainBuf)
			, Meta(MoveTemp(Other.Meta))
		{}
		FRobotEndpoint& operator=(FRobotEndpoint&& Other)
		{
			ActorId = MoveTemp(Other.ActorId);
			Listener = MoveTemp(Other.Listener);
			Clients = MoveTemp(Other.Clients);
			StateBuf = Other.StateBuf; CmdBuf = Other.CmdBuf; GainBuf = Other.GainBuf;
			Meta = MoveTemp(Other.Meta);
			return *this;
		}
		FString ActorId;

		// TCP listener + accepted clients
		URSNonBlockingSocket Listener;
		struct FClient
		{
			URSNonBlockingSocket Socket;
			TArray<uint8> ReadBuf;
			TArray<uint8> WriteBuf;
			bool bConnected = false;
		};
		TArray<FClient> Clients;

		// Triple buffer handles (shared with physics core)
		URSTripleBuffer<FRobotSnapshot>* StateBuf = nullptr;  // read
		URSTripleBuffer<FCommandSet>*    CmdBuf   = nullptr;  // write
		URSTripleBuffer<FGainSet>*       GainBuf  = nullptr;  // write

		// Static metadata for JSON building
		FRobotMetadata Meta;

		// Camera frame queue (fed by game thread)
		struct FCameraPacket
		{
			TArray<uint8> Payload;  // v2 image message, ready to send
			uint8 FrameType = 0;
		};
		TQueue<FCameraPacket, EQueueMode::Mpsc> CameraQueue;
	};

	// Admin endpoint
	URSNonBlockingSocket AdminListener;
	struct FAdminClient {
		URSNonBlockingSocket Socket;
		TArray<uint8> ReadBuf;
		TArray<uint8> WriteBuf;
		bool bConnected = false;
	};
	TArray<FAdminClient> AdminClients;

	URSNetworkThread();
	~URSNetworkThread();

	void Start(TArray<FRobotEndpoint>&& InEndpoints, int32 AdminPort,
		double InStateRateHz, double InCameraRateHz);
	void Stop();
	bool IsRunning() const { return bRunning.load(); }

	void EnqueueCameraFrame(int32 RobotIdx, uint8 FrameType, const uint8* Data, int32 Len);

private:
	TArray<FRobotEndpoint> Endpoints;
	double StateRateHz = 60.0;
	double CameraRateHz = 30.0;
	double LastStateTime = 0.0;

	std::atomic<bool> bRunning{false};
	FRunnableThread* Thread = nullptr;

	class FRunnableImpl : public FRunnable
	{
		URSNetworkThread* Owner;
	public:
		FRunnableImpl(URSNetworkThread* In) : Owner(In) {}
		virtual uint32 Run() override;
		virtual void Stop() override { Owner->bRunning.store(false); }
		virtual bool Init() override { return true; }
	} *Runnable = nullptr;

	void Tick();
	void AcceptConnections();
	void ReadFromClients();
	void ProcessClientData(int32 RobotIdx, const uint8* Data, int32 Len);
	void PublishStates();
	void DrainCameraQueues();
	void FlushWrites();
	void HandleAdmin();

	// Frame protocol helpers
	static void FrameClientRead(URSNonBlockingSocket& Sock, TArray<uint8>& ReadBuf,
		TArray<uint8>& OutFrames);
	static void EnqueueFrame(TArray<uint8>& WriteBuf, uint8 Type, const uint8* Data, int32 Len);
};

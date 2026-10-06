#pragma once
#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Network/URSSocket.h"
#include "Network/URSNetworkService.h"
#include "Containers/Queue.h"
#include "Vision/URSVideoDeliveryGate.h"
// Owns TCP sockets exclusively. No UObjects or render/MuJoCo APIs.
class URSNetworkThread : public IURSNetworkService
{
public:
	URSNetworkThread();
	~URSNetworkThread();
	bool Start(TArray<FURSRobotChannel>&& Channels, const FURSNetworkConfig& InConfig) override;
	void Stop() override;
	bool IsRunning() const
	{
		return bRunning.load();
	}
	void EnqueueCameraFrame(const URSoccerLab::FEncodedCameraFrame& Frame) override;
	bool DequeueAdminRequest(URSoccerLab::FAdminMessage& Out) override;
	void EnqueueAdminReply(uint64 ClientId, const FString& Json) override;
	bool HasCameraSubscribers() const override
	{
		return bCameraSubscribers.load();
	}

private:
	struct FClient
	{
		URSNonBlockingSocket Socket;
		TArray<uint8> ReadBuf, WriteBuf;
		TArray<uint8> PendingCameraPayload, PendingDepthPayload;
		URSoccerLab::FVideoDeliveryGate VideoGate;
		bool bConnected = true;
		uint64 ClientId = 0;
	};
	struct FRobotEndpoint
	{
		FURSRobotChannel Channel;
		URSNonBlockingSocket Listener;
		TArray<FClient> Clients;
		FGainSet AccumulatedGains;
	};
	TArray<FRobotEndpoint> Endpoints;
	URSNonBlockingSocket AdminListener;
	TArray<FClient> AdminClients;
	uint64 NextClientId = 1;
	TQueue<URSoccerLab::FAdminMessage, EQueueMode::Spsc> AdminRequests;
	TQueue<URSoccerLab::FAdminMessage, EQueueMode::Spsc> AdminReplies;
	std::atomic<int32> PendingAdminRequests{0};
	// Latest frame per actor, bounded by endpoint count, independent of TCP framing.
	FCriticalSection CameraMutex;
	TMap<FString, URSoccerLab::FEncodedCameraFrame> CameraFrames;
	double StateRateHz = 60, LastStateTime = 0;
	FURSNetworkConfig Config;
	std::atomic<bool> bRunning{false};
	std::atomic<bool> bCameraSubscribers{false};
	FRunnableThread* Thread = nullptr;
	class FRunnableImpl : public FRunnable
	{
		URSNetworkThread* Owner;

	public:
		explicit FRunnableImpl(URSNetworkThread* In) : Owner(In)
		{
		}
		bool Init() override;
		uint32 Run() override;
		void Exit() override;
		void Stop() override
		{
			Owner->bRunning.store(false);
		}
	};
	FRunnableImpl* Runnable = nullptr;
	bool OpenListeners();
	void CloseSockets();
	void AcceptConnections();
	void ReadFromClients();
	void ReadClients(TArray<FClient>& Clients, int32 RobotIndex);
	void PublishStates();
	void DrainCameraQueues();
	void DrainAdminReplies();
	void FlushWrites();
	void FlushClients(TArray<FClient>& Clients);
};

#pragma once
#include "Core/URSRobotChannel.h"
#include "Protocol/URSMessageProtocol.h"
struct FURSNetworkConfig
{
	int32 RobotBasePort = 10000, AdminPort = 11000;
	double StateRateHz = 60;
};
// Game-thread-facing service contract. An adapter owns its socket worker.
class IURSNetworkService
{
public:
	virtual ~IURSNetworkService() = default;
	virtual bool Start(TArray<FURSRobotChannel>&& Channels, const FURSNetworkConfig& Config) = 0;
	virtual void Stop() = 0;
	virtual void EnqueueCameraFrame(const URSoccerLab::FEncodedCameraFrame& Frame) = 0;
	virtual bool DequeueAdminRequest(URSoccerLab::FAdminMessage& Out) = 0;
	virtual void EnqueueAdminReply(uint64 ClientId, const FString& Json) = 0;
	virtual bool HasCameraSubscribers() const = 0;
};

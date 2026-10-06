#pragma once
#include "Core/URSRobotChannel.h"
#include "Vision/URSImageEncoder.h"
namespace URSoccerLab
{
struct FAdminMessage
{
	uint64 ClientId = 0;
	FString Json;
};
struct FRobotMessage
{
	bool bControllerParams = false;
	FCommandSet Command;
	FGainSet Gains;
};
class URSOCCERLAB_API FMessageProtocol
{
public:
	static bool DecodeRobotMessage(const TArray<FString>& ActuatorNames, const uint8* Data, int32 Len,
	                               FRobotMessage& Out);
	static TArray<uint8> EncodeState(const FRobotSnapshot& State, const FRobotMetadata& Meta);
	static TArray<uint8> EncodeCamera(const FEncodedCameraFrame& Frame);
};
} // namespace URSoccerLab

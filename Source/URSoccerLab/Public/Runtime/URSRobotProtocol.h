#pragma once

#include "CoreMinimal.h"

namespace URSoccerLab
{
static constexpr int32 MinCommandPort = 10000;
static constexpr int32 DefaultCommandBasePort = 10000;
static constexpr int32 DefaultStatePort = 10100;
static constexpr int32 DefaultMetaPort = 10101;
static constexpr uint32 MotorCommandMagic = 0x4D535255u; // "URSM" in little-endian payload order.
static constexpr uint16 MotorCommandVersion = 1;

enum class EMotorCommandValidation : uint8
{
	Accepted,
	BadMagic,
	UnsupportedVersion,
	WrongMotorCount,
	NonFiniteMotorValue,
	StaleSequence,
	EmptyPayload
};

struct FRobotPortAssignment
{
	FString RobotName;
	int32 CommandPort = 0;
};

struct FRobotRuntimeConfig
{
	int32 CommandBasePort = DefaultCommandBasePort;
	int32 StatePort = DefaultStatePort;
	int32 MetaPort = DefaultMetaPort;
	double CommandTimeoutSec = 0.1;
	TArray<FString> RobotNames;

	FRobotRuntimeConfig();
};

struct FMotorCommand
{
	uint64 Sequence = 0;
	double StampSec = 0.0;
	TArray<float> Motors;
};

struct FMotorCommandParseResult
{
	EMotorCommandValidation Status = EMotorCommandValidation::EmptyPayload;
	FMotorCommand Command;

	bool IsAccepted() const { return Status == EMotorCommandValidation::Accepted; }
};

class FRobotProtocol
{
public:
	static TArray<FString> MakeDefaultRobotNames();
	static bool IsValidCommandBasePort(int32 BasePort, int32 RobotCount);
	static bool BuildPortAssignments(const FRobotRuntimeConfig& Config, TArray<FRobotPortAssignment>& OutAssignments);
	static FString BuildTcpBindEndpoint(int32 Port);

	static TArray<uint8> EncodeMotorCommand(const FMotorCommand& Command);
	static FMotorCommandParseResult DecodeMotorCommand(const TArrayView<const uint8> Payload, int32 ExpectedMotorCount);
	static const TCHAR* LexToString(EMotorCommandValidation Status);
};

class FMotorCommandBuffer
{
public:
	explicit FMotorCommandBuffer(int32 InExpectedMotorCount = 0, double InTimeoutSec = 0.1);

	void Configure(int32 InExpectedMotorCount, double InTimeoutSec);
	EMotorCommandValidation TryAccept(const FMotorCommand& Command, double NowSec);
	bool IsTimedOut(double NowSec) const;
	TArray<float> GetCommandOrZero(double NowSec) const;
	uint64 GetLastAcceptedSequence() const { return LastAcceptedSequence; }
	bool HasCommand() const { return bHasCommand; }

private:
	int32 ExpectedMotorCount = 0;
	double TimeoutSec = 0.1;
	double LastAcceptedTimeSec = 0.0;
	uint64 LastAcceptedSequence = 0;
	bool bHasCommand = false;
	TArray<float> LatestMotors;
};
} // namespace URSoccerLab

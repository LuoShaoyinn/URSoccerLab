#include "Runtime/URSRobotProtocol.h"

#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace URSoccerLab
{
FRobotRuntimeConfig::FRobotRuntimeConfig()
	: RobotNames(FRobotProtocol::MakeDefaultRobotNames())
{
}

TArray<FString> FRobotProtocol::MakeDefaultRobotNames()
{
	TArray<FString> Names;
	Names.Reserve(14);
	for (int32 Idx = 0; Idx < 7; ++Idx)
	{
		Names.Add(FString::Printf(TEXT("robot_rp%d"), Idx));
	}
	for (int32 Idx = 0; Idx < 7; ++Idx)
	{
		Names.Add(FString::Printf(TEXT("robot_bp%d"), Idx));
	}
	return Names;
}

bool FRobotProtocol::IsValidCommandBasePort(int32 BasePort, int32 RobotCount)
{
	return BasePort >= MinCommandPort && RobotCount >= 0 && BasePort + RobotCount - 1 <= 65535;
}

bool FRobotProtocol::BuildPortAssignments(const FRobotRuntimeConfig& Config, TArray<FRobotPortAssignment>& OutAssignments)
{
	OutAssignments.Reset();
	if (!IsValidCommandBasePort(Config.CommandBasePort, Config.RobotNames.Num()))
	{
		return false;
	}

	OutAssignments.Reserve(Config.RobotNames.Num());
	for (int32 Idx = 0; Idx < Config.RobotNames.Num(); ++Idx)
	{
		FRobotPortAssignment Assignment;
		Assignment.RobotName = Config.RobotNames[Idx];
		Assignment.CommandPort = Config.CommandBasePort + Idx;
		OutAssignments.Add(MoveTemp(Assignment));
	}
	return true;
}

FString FRobotProtocol::BuildTcpBindEndpoint(int32 Port)
{
	return FString::Printf(TEXT("tcp://0.0.0.0:%d"), Port);
}

TArray<uint8> FRobotProtocol::EncodeMotorCommand(const FMotorCommand& Command)
{
	TArray<uint8> Payload;
	FMemoryWriter Writer(Payload, true);

	uint32 Magic = MotorCommandMagic;
	uint16 Version = MotorCommandVersion;
	uint16 Flags = 0;
	uint32 NumMotors = static_cast<uint32>(Command.Motors.Num());

	Writer << Magic;
	Writer << Version;
	Writer << Flags;
	Writer << const_cast<uint64&>(Command.Sequence);
	Writer << const_cast<double&>(Command.StampSec);
	Writer << NumMotors;
	for (float Value : Command.Motors)
	{
		Writer << Value;
	}

	return Payload;
}

FMotorCommandParseResult FRobotProtocol::DecodeMotorCommand(const TArrayView<const uint8> Payload, int32 ExpectedMotorCount)
{
	FMotorCommandParseResult Result;
	if (Payload.Num() == 0)
	{
		return Result;
	}

	TArray<uint8> MutablePayload(Payload.GetData(), Payload.Num());
	FMemoryReader Reader(MutablePayload, true);

	uint32 Magic = 0;
	uint16 Version = 0;
	uint16 Flags = 0;
	uint32 NumMotors = 0;
	Reader << Magic;
	Reader << Version;
	Reader << Flags;
	Reader << Result.Command.Sequence;
	Reader << Result.Command.StampSec;
	Reader << NumMotors;

	if (Reader.IsError() || Magic != MotorCommandMagic)
	{
		Result.Status = EMotorCommandValidation::BadMagic;
		return Result;
	}
	if (Version != MotorCommandVersion)
	{
		Result.Status = EMotorCommandValidation::UnsupportedVersion;
		return Result;
	}
	if (static_cast<int32>(NumMotors) != ExpectedMotorCount)
	{
		Result.Status = EMotorCommandValidation::WrongMotorCount;
		return Result;
	}

	Result.Command.Motors.SetNum(static_cast<int32>(NumMotors));
	for (float& Value : Result.Command.Motors)
	{
		Reader << Value;
		if (Reader.IsError())
		{
			Result.Status = EMotorCommandValidation::WrongMotorCount;
			return Result;
		}
		if (!FMath::IsFinite(Value))
		{
			Result.Status = EMotorCommandValidation::NonFiniteMotorValue;
			return Result;
		}
	}

	Result.Status = EMotorCommandValidation::Accepted;
	return Result;
}

const TCHAR* FRobotProtocol::LexToString(EMotorCommandValidation Status)
{
	switch (Status)
	{
	case EMotorCommandValidation::Accepted:
		return TEXT("Accepted");
	case EMotorCommandValidation::BadMagic:
		return TEXT("BadMagic");
	case EMotorCommandValidation::UnsupportedVersion:
		return TEXT("UnsupportedVersion");
	case EMotorCommandValidation::WrongMotorCount:
		return TEXT("WrongMotorCount");
	case EMotorCommandValidation::NonFiniteMotorValue:
		return TEXT("NonFiniteMotorValue");
	case EMotorCommandValidation::StaleSequence:
		return TEXT("StaleSequence");
	case EMotorCommandValidation::EmptyPayload:
	default:
		return TEXT("EmptyPayload");
	}
}

FMotorCommandBuffer::FMotorCommandBuffer(int32 InExpectedMotorCount, double InTimeoutSec)
{
	Configure(InExpectedMotorCount, InTimeoutSec);
}

void FMotorCommandBuffer::Configure(int32 InExpectedMotorCount, double InTimeoutSec)
{
	ExpectedMotorCount = FMath::Max(0, InExpectedMotorCount);
	TimeoutSec = FMath::Max(0.0, InTimeoutSec);
	LatestMotors.Init(0.0f, ExpectedMotorCount);
	LastAcceptedSequence = 0;
	LastAcceptedTimeSec = 0.0;
	bHasCommand = false;
}

EMotorCommandValidation FMotorCommandBuffer::TryAccept(const FMotorCommand& Command, double NowSec)
{
	if (Command.Motors.Num() != ExpectedMotorCount)
	{
		return EMotorCommandValidation::WrongMotorCount;
	}
	if (bHasCommand && Command.Sequence <= LastAcceptedSequence)
	{
		return EMotorCommandValidation::StaleSequence;
	}
	for (float Value : Command.Motors)
	{
		if (!FMath::IsFinite(Value))
		{
			return EMotorCommandValidation::NonFiniteMotorValue;
		}
	}

	LatestMotors = Command.Motors;
	LastAcceptedSequence = Command.Sequence;
	LastAcceptedTimeSec = NowSec;
	bHasCommand = true;
	return EMotorCommandValidation::Accepted;
}

bool FMotorCommandBuffer::IsTimedOut(double NowSec) const
{
	return !bHasCommand || NowSec - LastAcceptedTimeSec > TimeoutSec;
}

TArray<float> FMotorCommandBuffer::GetCommandOrZero(double NowSec) const
{
	if (IsTimedOut(NowSec))
	{
		TArray<float> Zeros;
		Zeros.Init(0.0f, ExpectedMotorCount);
		return Zeros;
	}
	return LatestMotors;
}
} // namespace URSoccerLab

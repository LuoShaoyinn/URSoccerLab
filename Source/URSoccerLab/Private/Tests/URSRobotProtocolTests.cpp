#if WITH_DEV_AUTOMATION_TESTS

#include "Runtime/URSRobotProtocol.h"

#include "Misc/AutomationTest.h"
#include <limits>

using namespace URSoccerLab;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSRobotPortAssignmentTest,
	"URSoccerLab.Runtime.Protocol.DefaultRobotPorts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSRobotPortAssignmentTest::RunTest(const FString& Parameters)
{
	FRobotRuntimeConfig Config;
	TArray<FRobotPortAssignment> Assignments;
	TestTrue(TEXT("default port assignment builds"), FRobotProtocol::BuildPortAssignments(Config, Assignments));
	TestEqual(TEXT("default robot count"), Assignments.Num(), 14);
	TestEqual(TEXT("first robot"), Assignments[0].RobotName, FString(TEXT("robot_rp0")));
	TestEqual(TEXT("first port"), Assignments[0].CommandPort, 10000);
	TestEqual(TEXT("last robot"), Assignments.Last().RobotName, FString(TEXT("robot_bp6")));
	TestEqual(TEXT("last port"), Assignments.Last().CommandPort, 10013);

	Config.CommandBasePort = 9999;
	TestFalse(TEXT("base port below 10000 rejected"), FRobotProtocol::BuildPortAssignments(Config, Assignments));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSMotorCommandDecodeTest,
	"URSoccerLab.Runtime.Protocol.MotorCommandDecodeValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSMotorCommandDecodeTest::RunTest(const FString& Parameters)
{
	FMotorCommand Command;
	Command.Sequence = 7;
	Command.StampSec = 12.5;
	Command.Motors = { 0.1f, -0.2f, 0.3f };

	const TArray<uint8> Payload = FRobotProtocol::EncodeMotorCommand(Command);
	FMotorCommandParseResult Parsed = FRobotProtocol::DecodeMotorCommand(MakeArrayView(Payload), 3);
	TestEqual(TEXT("valid payload accepted"), static_cast<uint8>(Parsed.Status), static_cast<uint8>(EMotorCommandValidation::Accepted));
	TestEqual(TEXT("sequence decoded"), Parsed.Command.Sequence, static_cast<uint64>(7));
	TestEqual(TEXT("motor count decoded"), Parsed.Command.Motors.Num(), 3);
	TestEqual(TEXT("motor value decoded"), Parsed.Command.Motors[1], -0.2f);

	Parsed = FRobotProtocol::DecodeMotorCommand(MakeArrayView(Payload), 4);
	TestEqual(TEXT("wrong motor count rejected"), static_cast<uint8>(Parsed.Status), static_cast<uint8>(EMotorCommandValidation::WrongMotorCount));

	TArray<uint8> BadPayload = Payload;
	BadPayload[0] = 0;
	Parsed = FRobotProtocol::DecodeMotorCommand(MakeArrayView(BadPayload), 3);
	TestEqual(TEXT("bad magic rejected"), static_cast<uint8>(Parsed.Status), static_cast<uint8>(EMotorCommandValidation::BadMagic));

	Command.Motors[1] = std::numeric_limits<float>::infinity();
	Parsed = FRobotProtocol::DecodeMotorCommand(MakeArrayView(FRobotProtocol::EncodeMotorCommand(Command)), 3);
	TestEqual(TEXT("non-finite motor rejected"), static_cast<uint8>(Parsed.Status), static_cast<uint8>(EMotorCommandValidation::NonFiniteMotorValue));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSImportedComponentNameTest,
	"URSoccerLab.Runtime.Protocol.ImportedComponentNameNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSImportedComponentNameTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("generated blueprint prefix stripped"),
		FRobotProtocol::NormalizeImportedComponentName(TEXT("pi_plus_urlab_origin_camera_C_0_head_pitch_joint")),
		FString(TEXT("head_pitch_joint")));
	TestEqual(TEXT("normal joint name unchanged"),
		FRobotProtocol::NormalizeImportedComponentName(TEXT("l_hip_pitch_joint")),
		FString(TEXT("l_hip_pitch_joint")));
	TestEqual(TEXT("incomplete marker unchanged"),
		FRobotProtocol::NormalizeImportedComponentName(TEXT("robot_C_head_pitch_joint")),
		FString(TEXT("robot_C_head_pitch_joint")));
	TestEqual(TEXT("robot actor prefix stripped"),
		FRobotProtocol::NormalizeRobotComponentName(TEXT("robot_rp0_head_pitch_joint"), TEXT("robot_rp0")),
		FString(TEXT("head_pitch_joint")));
	TestEqual(TEXT("generated blueprint and robot prefixes stripped"),
		FRobotProtocol::NormalizeRobotComponentName(TEXT("pi_plus_urlab_origin_camera_C_0_head_pitch_joint"), TEXT("robot_rp0")),
		FString(TEXT("head_pitch_joint")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSMotorCommandBufferTest,
	"URSoccerLab.Runtime.Protocol.CommandBufferIsolationAndTimeout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSMotorCommandBufferTest::RunTest(const FString& Parameters)
{
	FMotorCommandBuffer RobotA(2, 0.1);
	FMotorCommandBuffer RobotB(2, 0.1);

	FMotorCommand CommandA;
	CommandA.Sequence = 10;
	CommandA.Motors = { 1.0f, 2.0f };
	TestEqual(TEXT("first command accepted"), static_cast<uint8>(RobotA.TryAccept(CommandA, 1.0)), static_cast<uint8>(EMotorCommandValidation::Accepted));

	FMotorCommand Stale = CommandA;
	Stale.Sequence = 10;
	Stale.Motors = { 3.0f, 4.0f };
	TestEqual(TEXT("stale command rejected"), static_cast<uint8>(RobotA.TryAccept(Stale, 1.01)), static_cast<uint8>(EMotorCommandValidation::StaleSequence));

	FMotorCommand WrongCount;
	WrongCount.Sequence = 11;
	WrongCount.Motors = { 1.0f };
	TestEqual(TEXT("wrong count rejected"), static_cast<uint8>(RobotA.TryAccept(WrongCount, 1.02)), static_cast<uint8>(EMotorCommandValidation::WrongMotorCount));

	const TArray<float> ActiveA = RobotA.GetCommandOrZero(1.05);
	TestEqual(TEXT("robot A keeps accepted value"), ActiveA[0], 1.0f);
	const TArray<float> ActiveB = RobotB.GetCommandOrZero(1.05);
	TestEqual(TEXT("robot B remains isolated"), ActiveB[0], 0.0f);

	TestFalse(TEXT("robot A not timed out before deadline"), RobotA.IsTimedOut(1.05));
	TestTrue(TEXT("robot A timed out after deadline"), RobotA.IsTimedOut(1.11));
	const TArray<float> TimedOutA = RobotA.GetCommandOrZero(1.11);
	TestEqual(TEXT("timed out robot returns zero"), TimedOutA[1], 0.0f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

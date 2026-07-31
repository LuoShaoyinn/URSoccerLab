#if WITH_DEV_AUTOMATION_TESTS

#include "Runtime/URSRobotNames.h"

#include "Misc/AutomationTest.h"

using namespace URSoccerLab;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSImportedComponentNameTest,
	"URSoccerLab.Runtime.RobotNames.ImportedComponentNameNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSImportedComponentNameTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("generated blueprint prefix stripped"),
		FRobotNames::NormalizeImportedComponentName(TEXT("pi_plus_stereo_camera_C_0_head_pitch_joint")),
		FString(TEXT("head_pitch_joint")));
	TestEqual(TEXT("normal joint name unchanged"),
		FRobotNames::NormalizeImportedComponentName(TEXT("l_hip_pitch_joint")),
		FString(TEXT("l_hip_pitch_joint")));
	TestEqual(TEXT("incomplete marker unchanged"),
		FRobotNames::NormalizeImportedComponentName(TEXT("robot_C_head_pitch_joint")),
		FString(TEXT("robot_C_head_pitch_joint")));
	TestEqual(TEXT("robot actor prefix stripped"),
		FRobotNames::NormalizeRobotComponentName(TEXT("robot_rp0_head_pitch_joint"), TEXT("robot_rp0")),
		FString(TEXT("head_pitch_joint")));
	TestEqual(TEXT("generated blueprint and robot prefixes stripped"),
		FRobotNames::NormalizeRobotComponentName(
			TEXT("pi_plus_stereo_camera_C_0_head_pitch_joint"), TEXT("robot_rp0")),
		FString(TEXT("head_pitch_joint")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#if WITH_DEV_AUTOMATION_TESTS

#include "Transport/URSTcpProtocol.h"

#include "Misc/AutomationTest.h"

using namespace URSoccerLab;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSRobotPortAssignmentTest,
	"URSoccerLab.Transport.TcpProtocol.DefaultRobotPorts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSRobotPortAssignmentTest::RunTest(const FString& Parameters)
{
	TArray<int32> Ports;
	TestTrue(TEXT("two robot ports build"),
		TcpProtocol::BuildRobotPorts(TcpProtocol::DefaultRobotBasePort, 2, Ports));
	TestEqual(TEXT("one port per robot"), Ports.Num(), 2);
	TestEqual(TEXT("first robot port"), Ports[0], 10000);
	TestEqual(TEXT("second robot port"), Ports[1], 10001);
	TestTrue(TEXT("default global admin port is valid"),
		TcpProtocol::IsValidPortLayout(
			TcpProtocol::DefaultRobotBasePort, 2, TcpProtocol::DefaultAdminPort));
	TestFalse(TEXT("global admin cannot overlap a robot port"),
		TcpProtocol::IsValidPortLayout(TcpProtocol::DefaultRobotBasePort, 2, 10001));
	TestFalse(TEXT("privileged robot base port rejected"),
		TcpProtocol::BuildRobotPorts(1023, 2, Ports));
	TestFalse(TEXT("overflowing robot port range rejected"),
		TcpProtocol::BuildRobotPorts(65535, 2, Ports));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

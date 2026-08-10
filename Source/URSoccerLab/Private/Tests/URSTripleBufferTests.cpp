#include "Core/URSTripleBuffer.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FURSTripleBufferTest,
	"URSoccerLab.Core.TripleBuffer.LatestValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FURSTripleBufferTest::RunTest(const FString& Parameters)
{
	URSTripleBuffer<int32> Buffer;
	TestEqual(TEXT("initial value"), Buffer.Front(), 0);

	Buffer.Back() = 1;
	Buffer.Publish();
	TestEqual(TEXT("first publication"), Buffer.Front(), 1);

	Buffer.Back() = 2;
	Buffer.Publish();
	Buffer.Back() = 3;
	Buffer.Publish();
	TestEqual(TEXT("intermediate values are dropped"), Buffer.Front(), 3);

	return true;
}

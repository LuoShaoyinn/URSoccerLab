#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Vision/URSVideoDeliveryGate.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSVideoDeliveryTest, "URSoccerLab.Vision.Av1Delivery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSVideoDeliveryTest::RunTest(const FString&)
{
 using namespace URSoccerLab;
 FVideoDeliveryGate Gate; TArray<uint8> Pending; const TArray<uint8> Bytes = {1,2,3};
 FEncodedCameraFrame Frame; Frame.bVideo = true; Frame.VideoEpoch = 1; Frame.Sequence = 10;
 TestFalse(TEXT("join skips delta"), Gate.Offer(Frame, Pending, Bytes));
 Frame.Sequence = 11; Frame.bKeyFrame = true;
 TestTrue(TEXT("join starts at key"), Gate.Offer(Frame, Pending, Bytes)); Pending.Empty();
 Frame.Sequence = 12; Frame.bKeyFrame = false;
 TestTrue(TEXT("contiguous delta accepted"), Gate.Offer(Frame, Pending, Bytes));
 Frame.Sequence = 13;
 TestFalse(TEXT("unsent packet replacement waits for key"), Gate.Offer(Frame, Pending, Bytes));
 TestTrue(TEXT("obsolete unsent packet cleared"), Pending.IsEmpty());
 Frame.Sequence = 14; Frame.bKeyFrame = true;
 TestTrue(TEXT("periodic key resumes"), Gate.Offer(Frame, Pending, Bytes)); Pending.Empty();
 Frame.Sequence = 16; Frame.bKeyFrame = false;
 TestFalse(TEXT("handoff sequence gap waits"), Gate.Offer(Frame, Pending, Bytes));
 Frame.Sequence = 0; Frame.VideoEpoch = 2;
 TestFalse(TEXT("recreated encoder delta waits"), Gate.Offer(Frame, Pending, Bytes));
 Frame.Sequence = 1; Frame.bKeyFrame = true;
 TestTrue(TEXT("recreated encoder key resumes"), Gate.Offer(Frame, Pending, Bytes));
 Frame.bVideo = false;
 TestTrue(TEXT("JPEG retains latest-frame behavior"), Gate.Offer(Frame, Pending, Bytes));
 return true;
}
#endif

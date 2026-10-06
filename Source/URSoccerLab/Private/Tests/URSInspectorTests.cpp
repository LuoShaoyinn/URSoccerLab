#if WITH_DEV_AUTOMATION_TESTS
#include "Inspector/URSInspectorProtocol.h"
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSInspectorProtocolTest, "URSoccerLab.Inspector.Protocol", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSInspectorProtocolTest::RunTest(const FString&)
{
 URSoccerLab::FInspectorPose Pose; FString Error;
 auto Decode = [&](const FString& Json) { return URSoccerLab::FInspectorProtocol::Decode(Json, Pose, Error); };
 TestTrue(TEXT("valid pose"), Decode(TEXT(R"({"version":1,"command":"set_camera","args":{"translation_m":[-4,0,2],"rotation_quat_xyzw":[0,0,0,2]}})")));
 TestEqual(TEXT("normalized"), Pose.Rotation.W, 1.0);
 TestFalse(TEXT("motor rejected"), Decode(TEXT(R"({"joint":1})")));
 TestFalse(TEXT("admin rejected"), Decode(TEXT(R"({"version":1,"command":"reset","args":{"actor_id":"robot_rp0"}})")));
 TestFalse(TEXT("bounds"), Decode(TEXT(R"({"version":1,"command":"set_camera","args":{"translation_m":[101,0,2],"rotation_quat_xyzw":[0,0,0,1]}})")));
 TestFalse(TEXT("zero quaternion"), Decode(TEXT(R"({"version":1,"command":"set_camera","args":{"translation_m":[0,0,2],"rotation_quat_xyzw":[0,0,0,0]}})")));
 TestFalse(TEXT("bad quaternion type"), Decode(TEXT(R"({"version":1,"command":"set_camera","args":{"translation_m":[0,0,2],"rotation_quat_xyzw":[false,0,0,1]}})")));
 TestFalse(TEXT("bad version"), Decode(TEXT(R"({"version":2,"command":"set_camera","args":{"translation_m":[0,0,2],"rotation_quat_xyzw":[0,0,0,1]}})")));
 TestFalse(TEXT("malformed"), Decode(TEXT("[")));
 return true;
}

#include "Inspector/URSInspectorNetwork.h"
#include "Network/URSTcpFraming.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSInspectorLifecycleTest, "URSoccerLab.Inspector.SocketLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSInspectorLifecycleTest::RunTest(const FString&)
{
 const int Port = 50000 + FPlatformProcess::GetCurrentProcessId()%1000;
 auto Service = CreateURSInspectorTcp();
 auto WaitEvent = [&](URSoccerLab::FInspectorEvent::EKind Kind, uint64& Id) {
  const double Deadline = FPlatformTime::Seconds()+3;
  URSoccerLab::FInspectorEvent Event;
  while (FPlatformTime::Seconds()<Deadline)
  {
   if (Service->Dequeue(Event) && Event.Kind == Kind) { Id = Event.Session; return true; }
   FPlatformProcess::Sleep(.001);
  }
  return false;
 };
 auto Connect = [&]() {
  auto* S = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
  auto* Socket = S->CreateSocket(NAME_Stream, TEXT("InspectorTest"), false);
  auto Address = S->CreateInternetAddr(); bool Valid;
  Address->SetIp(TEXT("127.0.0.1"), Valid); Address->SetPort(Port);
  if (!Socket || !Socket->Connect(*Address)) { if (Socket) S->DestroySocket(Socket); return static_cast<FSocket*>(nullptr); }
  Socket->SetNonBlocking(true); return Socket;
 };
 auto Request = [&](FSocket* Socket) {
  const ANSICHAR* Json = R"({"version":1,"command":"set_camera","args":{"translation_m":[0,0,2],"rotation_quat_xyzw":[0,0,0,1]}})";
  TArray<uint8> Bytes; URSoccerLab::TcpFraming::Append(Bytes, 0, reinterpret_cast<const uint8*>(Json), FCStringAnsi::Strlen(Json));
  int32 Sent = 0; return Socket->Send(Bytes.GetData(), Bytes.Num(), Sent) && Sent == Bytes.Num();
 };
 if (!TestTrue(TEXT("start without any renderer/physics"), Service->Start(Port))) return false;
 FSocket* First = Connect();
 if (!TestNotNull(TEXT("first connection"), First)) { Service->Stop(); return false; }
 uint64 Id = 0;
 TestTrue(TEXT("connected event"), WaitEvent(URSoccerLab::FInspectorEvent::EKind::Connected, Id));
 TestTrue(TEXT("request send"), Request(First));
 TestTrue(TEXT("typed pose without GT tick"), WaitEvent(URSoccerLab::FInspectorEvent::EKind::Pose, Id));
 // Stop while an acknowledged pose is pending. Restart must drop old queues.
 Service->Reply(Id, URSoccerLab::FInspectorProtocol::Reply(true));
 ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(First);
 Service->Stop();
 URSoccerLab::FInspectorEvent Event;
 TestFalse(TEXT("old events cleared"), Service->Dequeue(Event));
 TestTrue(TEXT("same adapter restart"), Service->Start(Port));
 FSocket* Second = Connect();
 if (!TestNotNull(TEXT("second connection"), Second)) { Service->Stop(); return false; }
 uint64 NewId = 0;
 TestTrue(TEXT("new connected event"), WaitEvent(URSoccerLab::FInspectorEvent::EKind::Connected, NewId));
 TestTrue(TEXT("stable identity not reused on adapter restart"), NewId != Id);
 TestTrue(TEXT("second request"), Request(Second));
 TestTrue(TEXT("second pose"), WaitEvent(URSoccerLab::FInspectorEvent::EKind::Pose, NewId));
 ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Second);
 TestTrue(TEXT("disconnect event"), WaitEvent(URSoccerLab::FInspectorEvent::EKind::Disconnected, NewId));
 Service->Stop(); return true;
}

#endif

#if WITH_DEV_AUTOMATION_TESTS
#include "Network/URSNetworkThread.h"
#include "Network/URSTcpFraming.h"
#include "Protocol/URSMessageProtocol.h"
#include "Vision/URSImageEncoder.h"
#include "Misc/AutomationTest.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "HAL/PlatformTime.h"

namespace URSNetworkIsolationTest
{
struct FPeer
{
	FSocket* Socket = nullptr;
	TArray<uint8> Received;
	~FPeer()
	{
		Close();
	}
	void Close()
	{
		if (Socket)
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
	bool Connect(int32 Port)
	{
		auto* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		Socket = Subsystem->CreateSocket(NAME_Stream, TEXT("URS-Test"), false);
		auto Address = Subsystem->CreateInternetAddr();
		bool bValid = false;
		Address->SetIp(TEXT("127.0.0.1"), bValid);
		Address->SetPort(Port);
		if (!Socket || !Socket->Connect(*Address))
			return false;
		Socket->SetNonBlocking(true);
		return true;
	}
	bool Send(const TArray<uint8>& Data)
	{
		int32 Offset = 0;
		const double Deadline = FPlatformTime::Seconds() + 3;
		while (Offset < Data.Num() && FPlatformTime::Seconds() < Deadline)
		{
			int32 Sent = 0;
			Socket->Send(Data.GetData() + Offset, Data.Num() - Offset, Sent);
			Offset += Sent;
			if (!Sent)
				FPlatformProcess::Sleep(0.001);
		}
		return Offset == Data.Num();
	}
	bool Frame(uint8 WantedType, TArray<uint8>& Payload)
	{
		const double Deadline = FPlatformTime::Seconds() + 3;
		while (FPlatformTime::Seconds() < Deadline)
		{
			uint8 Buffer[65536];
			int32 Count = 0;
			while (Socket->Recv(Buffer, sizeof(Buffer), Count) && Count > 0)
				Received.Append(Buffer, Count);
			while (Received.Num() >= 5)
			{
				const uint32 Length = URSoccerLab::TcpFraming::ReadLength(Received.GetData());
				if (Length < 1 || Length > URSoccerLab::TcpFraming::MaxFrameLength)
					return false;
				if (uint32(Received.Num()) < Length + 4)
					break;
				const bool bMatch = Received[4] == WantedType;
				if (bMatch)
				{
					Payload.Empty();
					Payload.Append(Received.GetData() + 5, Length - 1);
				}
				Received.RemoveAt(0, Length + 4);
				if (bMatch)
					return true;
			}
			FPlatformProcess::Sleep(0.001);
		}
		return false;
	}
};
TArray<uint8> JsonFrame(const ANSICHAR* Json)
{
	TArray<uint8> Bytes;
	URSoccerLab::TcpFraming::Append(Bytes, 0, reinterpret_cast<const uint8*>(Json), FCStringAnsi::Strlen(Json));
	return Bytes;
}
template <typename Predicate> bool Wait(Predicate Ready)
{
	const double Deadline = FPlatformTime::Seconds() + 3;
	while (FPlatformTime::Seconds() < Deadline)
	{
		if (Ready())
			return true;
		FPlatformProcess::Sleep(0.001);
	}
	return false;
}
} // namespace URSNetworkIsolationTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSImageProtocolIsolationTest,
                                 "URSoccerLab.NetworkIsolation.ImageEncodingAndWireCompatibility",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSImageProtocolIsolationTest::RunTest(const FString& Parameters)
{
	using namespace URSoccerLab;
	auto& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	FRawCameraImage Raw;
	Raw.Name = TEXT("left");
	Raw.Width = 2;
	Raw.Height = 1;
	Raw.Pixels = {FColor(1, 2, 3, 255), FColor(4, 5, 6, 255)};
	FEncodedCameraImage Image;
	TestTrue(TEXT("raw encoding without networking"), FImageEncoder::Encode(Raw, false, 85, Module, Image));
	TestEqual(TEXT("raw codec"), Image.Codec, uint8(0));
	FEncodedCameraFrame Frame;
	Frame.ActorId = TEXT("robot");
	Frame.Sequence = 0x12345678;
	Frame.SimTime = 1.5;
	Frame.Images.Add(Image);
	const auto Payload = FMessageProtocol::EncodeCamera(Frame);
	// Existing v2 image wire format, including native little-endian scalar fields.
	const uint8 Expected[] = {2,    1, 0,   0,   0x78, 0x56, 0x34, 0x12, 0, 0,   0, 0, 0, 0,  0xf8,
	                          0x3f, 4, 'l', 'e', 'f',  't',  0,    0,    0, 2,   0, 1, 0, 8,  0,
	                          0,    0, 8,   0,   0,    0,    3,    2,    1, 255, 6, 5, 4, 255};
	TestEqual(TEXT("v2 payload length unchanged"), Payload.Num(), int32(sizeof(Expected)));
	TestTrue(TEXT("v2 payload bytes unchanged"),
	         Payload.Num() == sizeof(Expected) && FMemory::Memcmp(Payload.GetData(), Expected, sizeof(Expected)) == 0);
	FEncodedCameraImage Jpeg;
	Raw.Width = 8;
	Raw.Height = 8;
	Raw.Pixels.Init(FColor(0, 0, 0, 0), 64);
	TestTrue(TEXT("black frame encodes"), FImageEncoder::Encode(Raw, true, 85, Module, Jpeg));
	TestEqual(TEXT("black frame uses JPEG"), Jpeg.Codec, uint8(1));
	auto Decoder = Module.CreateImageWrapper(EImageFormat::JPEG);
	TestTrue(TEXT("JPEG decodes independently"), Decoder->SetCompressed(Jpeg.Data.GetData(), Jpeg.Data.Num()));
	TestEqual(TEXT("decoded width"), Decoder->GetWidth(), int64(8));
	TestEqual(TEXT("decoded height"), Decoder->GetHeight(), int64(8));
	Raw.Pixels.RemoveAt(0);
	FEncodedCameraImage Invalid;
	TestFalse(TEXT("invalid pixel count rejected"), FImageEncoder::Encode(Raw, true, 85, Module, Invalid));
	FRobotMessage Message;
	const ANSICHAR Bad[] = "{\"motor\":1e100}";
	TestFalse(TEXT("float overflow command rejected"),
	          FMessageProtocol::DecodeRobotMessage({TEXT("motor")}, reinterpret_cast<const uint8*>(Bad),
	                                               sizeof(Bad) - 1, Message));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FURSNetworkWorkerIsolationTest,
                                 "URSoccerLab.NetworkIsolation.SocketWorkerAndMailboxes",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FURSNetworkWorkerIsolationTest::RunTest(const FString& Parameters)
{
	using namespace URSNetworkIsolationTest;
	FURSRobotChannel Channel;
	Channel.ActorId = TEXT("robot");
	Channel.Meta.ActuatorNames = {TEXT("motor")};
	Channel.StateBuf = MakeShared<URSTripleBuffer<FRobotSnapshot>, ESPMode::ThreadSafe>();
	Channel.CmdBuf = MakeShared<URSTripleBuffer<FCommandSet>, ESPMode::ThreadSafe>();
	Channel.GainBuf = MakeShared<URSTripleBuffer<FGainSet>, ESPMode::ThreadSafe>();
	FRobotSnapshot State{};
	State.SimTime = 42;
	Channel.StateBuf->PublishValue(State);
	TArray<FURSRobotChannel> Channels;
	Channels.Add(Channel);
	FURSNetworkConfig Config;
	Config.RobotBasePort = 48000 + FPlatformProcess::GetCurrentProcessId() % 1000;
	Config.AdminPort = Config.RobotBasePort + 1;
	URSNetworkThread Network;
	FURSNetworkConfig InvalidConfig = Config;
	InvalidConfig.AdminPort = InvalidConfig.RobotBasePort;
	TArray<FURSRobotChannel> InvalidChannels;
	InvalidChannels.Add(Channel);
	TestFalse(TEXT("adapter rejects overlapping listener ports"),
	          Network.Start(MoveTemp(InvalidChannels), InvalidConfig));
	if (!TestTrue(TEXT("start socket worker"), Network.Start(MoveTemp(Channels), Config)))
		return false;
	FPeer Robot, SecondRobot, Admin, SecondAdmin;
	if (!TestTrue(TEXT("robot connect"), Robot.Connect(Config.RobotBasePort)) ||
	    !TestTrue(TEXT("second robot client connect"), SecondRobot.Connect(Config.RobotBasePort)) ||
	    !TestTrue(TEXT("admin connect"), Admin.Connect(Config.AdminPort)) ||
	    !TestTrue(TEXT("second admin client connect"), SecondAdmin.Connect(Config.AdminPort)))
		return false;
	TestTrue(TEXT("camera subscriber demand published"), Wait([&] { return Network.HasCameraSubscribers(); }));
	TArray<uint8> Payload;
	TestTrue(TEXT("state delivered with no game-thread ticks"), Robot.Frame(0, Payload));
	TestTrue(TEXT("state serialization carries physics snapshot"),
	         FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num()).Get())
	             .Contains(TEXT("42")));
	auto Command = JsonFrame("{\"motor\":0.25}");
	TArray<uint8> Prefix;
	Prefix.Append(Command.GetData(), 2);
	TArray<uint8> Suffix;
	Suffix.Append(Command.GetData() + 2, Command.Num() - 2);
	TestTrue(TEXT("fragmented TCP header first part"), Robot.Send(Prefix));
	FPlatformProcess::Sleep(0.005);
	TestTrue(TEXT("fragmented TCP header rest"), Robot.Send(Suffix));
	TestTrue(TEXT("command reaches physics mailbox"), Wait([&] { return Channel.CmdBuf->HasNew(); }));
	TestEqual(TEXT("decoded actuator target"), Channel.CmdBuf->Front().Targets[0], 0.25f);
	auto Kp = JsonFrame("{\"kp\":{\"motor\":12}}");
	auto Kv = JsonFrame("{\"kv\":{\"motor\":3}}");
	Kp.Append(Kv);
	TestTrue(TEXT("coalesced gain frames sent"), Robot.Send(Kp));
	TestTrue(TEXT("partial gain updates preserved"), Wait([&] {
		         const auto& Gains = Channel.GainBuf->Front();
		         return Gains.bHasKp[0] && Gains.bHasKv[0];
	         }));
	TestEqual(TEXT("kp preserved"), Channel.GainBuf->Front().Kp[0], 12.0);
	TestEqual(TEXT("kv preserved"), Channel.GainBuf->Front().Kv[0], 3.0);
	TestTrue(TEXT("admin request sent"), Admin.Send(JsonFrame("{\"command\":\"get_pose\"}")));
	URSoccerLab::FAdminMessage Request;
	TestTrue(TEXT("admin reaches mailbox without game-thread ticks"),
	         Wait([&] { return Network.DequeueAdminRequest(Request); }));
	TestTrue(TEXT("admin connection identity set"), Request.ClientId != 0);
	Network.EnqueueAdminReply(Request.ClientId, TEXT("{\"ok\":true}"));
	TestTrue(TEXT("admin reply delivered by worker"), Admin.Frame(0, Payload));
	TestTrue(TEXT("second admin request sent"), SecondAdmin.Send(JsonFrame("{\"command\":\"reset\"}")));
	URSoccerLab::FAdminMessage SecondRequest;
	TestTrue(TEXT("second admin mailbox delivered"), Wait([&] { return Network.DequeueAdminRequest(SecondRequest); }));
	TestTrue(TEXT("connection identities isolated"), Request.ClientId != SecondRequest.ClientId);
	Network.EnqueueAdminReply(SecondRequest.ClientId, TEXT("{\"ok\":false}"));
	TestTrue(TEXT("reply routed to second admin"), SecondAdmin.Frame(0, Payload));
	URSoccerLab::FEncodedCameraFrame Frame;
	Frame.ActorId = TEXT("robot");
	Frame.Sequence = 123;
	URSoccerLab::FEncodedCameraImage Image;
	Image.Name = TEXT("left");
	Image.Width = 1;
	Image.Height = 1;
	Image.RawLength = 4;
	Image.Data = {1, 2, 3, 255};
	Frame.Images.Add(Image);
	const auto Expected = URSoccerLab::FMessageProtocol::EncodeCamera(Frame);
	Network.EnqueueCameraFrame(Frame);
	TestTrue(TEXT("camera delivered independently of physics/render"), Robot.Frame(1, Payload));
	TestTrue(TEXT("camera bytes preserved"), Payload == Expected);
	TestTrue(TEXT("camera broadcast to second robot client"), SecondRobot.Frame(1, Payload));
	TestTrue(TEXT("second client bytes preserved"), Payload == Expected);
	Robot.Close();
	SecondRobot.Close();
	FPeer Slow;
	TestTrue(TEXT("slow video client connects"), Slow.Connect(Config.RobotBasePort));
	int32 ReceiveSize = 0;
	Slow.Socket->SetReceiveBufferSize(4096, ReceiveSize);
	Frame.Images[0].Width = 512;
	Frame.Images[0].Height = 512;
	Frame.Images[0].Data.Init(0, 1024 * 1024);
	Frame.Images[0].RawLength = 1024 * 1024;
	// A stalled reader must not accumulate every image or monopolize the worker.
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Frame.Sequence = Index;
		Network.EnqueueCameraFrame(Frame);
		FPlatformProcess::Sleep(0.005);
	}
	TestTrue(TEXT("slow reader can still submit commands"), Slow.Send(JsonFrame("{\"motor\":0.75}")));
	TestTrue(TEXT("slow reader does not block command mailbox"), Wait([&] { return Channel.CmdBuf->HasNew(); }));
	TestEqual(TEXT("slow reader command applied"), Channel.CmdBuf->Front().Targets[0], 0.75f);
	TestTrue(TEXT("admin unaffected by stalled video"), Admin.Send(JsonFrame("{\"command\":\"get_pose\"}")));
	TestTrue(TEXT("admin mailbox unaffected by stalled video"),
	         Wait([&] { return Network.DequeueAdminRequest(Request); }));
	Slow.Close();
	TestTrue(TEXT("graceful close clears subscriber demand"), Wait([&] { return !Network.HasCameraSubscribers(); }));
	Network.Stop();
	TestFalse(TEXT("worker stopped"), Network.IsRunning());
	Channels.Add(Channel);
	TestTrue(TEXT("service restarts"), Network.Start(MoveTemp(Channels), Config));
	FPeer Reconnected;
	TestTrue(TEXT("robot reconnects after service restart"), Reconnected.Connect(Config.RobotBasePort));
	TestTrue(TEXT("state after restart"), Reconnected.Frame(0, Payload));
	Network.Stop();
	return true;
}
#endif

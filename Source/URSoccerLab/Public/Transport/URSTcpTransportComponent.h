#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "URSTcpTransportComponent.generated.h"

class UURSRobotCoreComponent;
class FSocket;

namespace URSProtocol
{
	static constexpr uint8 Type_JSON = 0x00;
	static constexpr uint8 Type_Camera = 0x01;

	static constexpr uint8 CameraCodec_Raw = 0x00;
	static constexpr uint8 CameraCodec_JPEG = 0x01;

	static constexpr int32 DefaultRobotBasePort = 10000;
	static constexpr int32 DefaultAdminPort = 11000;
}

UCLASS(ClassGroup = (URSoccerLab), meta = (BlueprintSpawnableComponent))
class URSOCCERLAB_API UURSTcpTransportComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UURSTcpTransportComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab")
	bool bAutoStart = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 RobotBasePort = URSProtocol::DefaultRobotBasePort;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 AdminPort = URSProtocol::DefaultAdminPort;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1.0", ClampMax = "1000.0"))
	double StateRateHz = 60.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1.0", ClampMax = "120.0"))
	double CameraRateHz = 15.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab")
	FString CameraCompress = TEXT("jpeg");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1", ClampMax = "100"))
	int32 JpegQuality = 85;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	struct FTcpClient
	{
		FSocket* Socket = nullptr;
		TArray<uint8> ReadBuffer;
	};

	struct FRobotListener
	{
		FString ActorId;
		FSocket* ListenerSocket = nullptr;
		TArray<FTcpClient> Clients;
	};

	TWeakObjectPtr<UURSRobotCoreComponent> Core;

	FRobotListener AdminListener;
	TArray<FRobotListener> RobotListeners;

	double LastStateTimeSec = 0.0;
	double LastCameraTimeSec = 0.0;
	TArray<FString> LastKnownRobots;

	bool StartTransport();
	void StopTransport();
	void RebuildListeners();

	void AcceptNewConnections(FRobotListener& Listener);
	void ReadFromClients(FRobotListener& Listener);
	void ProcessReadBuffer(FRobotListener& Listener, int32 ClientIdx);

	void SendToClients(FRobotListener& Listener, uint8 FrameType, const uint8* PayloadData, int32 PayloadSize);
	bool SendFrame(FSocket* Sock, uint8 FrameType, const uint8* PayloadData, int32 PayloadSize);

	void ProcessCommand(const FString& ActorId, const FString& JsonStr);
	void ProcessAdminRequest(FSocket* Sock, const FString& JsonStr);

	void TickStatePublish();
	void TickCameraPublish();

	FString BuildStateJson(const FString& ActorId);

	void CloseSocket(FSocket* Sock);
	void CloseListener(FRobotListener& Listener);
};

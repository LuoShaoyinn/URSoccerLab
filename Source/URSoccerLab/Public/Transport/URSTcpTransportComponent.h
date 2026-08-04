#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Queue.h"
#include "Scene/URSSceneConfig.h"
#include "Transport/URSTcpProtocol.h"
#include "URSTcpTransportComponent.generated.h"

class UURSRobotCoreComponent;
class UURSDisplayClusterCameraBinderComponent;
class FSocket;
class IImageWrapperModule;

UCLASS(ClassGroup = (URSoccerLab), meta = (BlueprintSpawnableComponent))
class URSOCCERLAB_API UURSTcpTransportComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UURSTcpTransportComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab")
	bool bAutoStart = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 RobotBasePort = URSoccerLab::TcpProtocol::DefaultRobotBasePort;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 AdminPort = URSoccerLab::TcpProtocol::DefaultAdminPort;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1.0", ClampMax = "1000.0"))
	double StateRateHz = 60.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1.0", ClampMax = "120.0"))
	double CameraRateHz = 30.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab")
	FString CameraCompress = TEXT("jpeg");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1", ClampMax = "100"))
	int32 JpegQuality = 85;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1.0", ClampMax = "120.0"))
	double DepthRateHz = 15.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab")
	FString DepthCompress = TEXT("zlib_u16_mm");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "65536", ClampMax = "67108864"))
	int32 MaxSendQueueBytes = 4 * 1024 * 1024;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	struct FTcpClient
	{
		FSocket* Socket = nullptr;
		TArray<uint8> ReadBuffer;
		TArray<uint8> WriteBuffer;
	};

	struct FRobotListener
	{
		FString ActorId;
		FSocket* ListenerSocket = nullptr;
		TArray<FTcpClient> Clients;
		uint32 NextRgbSequence = 0;
		uint32 NextDepthSequence = 0;
		uint64 Generation = 0;
		uint64 LastNDisplayRgbSequence = 0;
		bool bRgbEncodeInFlight = false;
		bool bDepthEncodeInFlight = false;
	};

	struct FCompletedVisionPacket
	{
		FString ActorId;
		uint64 ListenerGeneration = 0;
		uint8 FrameType = 0;
		bool bSuccess = false;
		TArray<uint8> Payload;
		uint32 EntryCount = 0;
		uint64 ImagePayloadBytes = 0;
		double EncodeSeconds = 0.0;
	};

	struct FAsyncVisionState
	{
		TQueue<FCompletedVisionPacket, EQueueMode::Mpsc> CompletedPackets;
		TAtomic<bool> bAcceptResults{true};
	};

	TWeakObjectPtr<UURSRobotCoreComponent> Core;
	TWeakObjectPtr<UURSDisplayClusterCameraBinderComponent> NDisplayBinder;

	FRobotListener AdminListener;
	TArray<FRobotListener> RobotListeners;

	double LastStateTimeSec = 0.0;
	double NextRgbTimeSec = 0.0;
	double NextDepthTimeSec = 0.0;
	TArray<FString> LastKnownRobots;
	URSoccerLab::FURSVisionConfig VisionConfig;
	// Deterministic Gaussian noise generator shared across robot publishes.
	FRandomStream NoiseRng;
	uint64 ListenerGenerationCounter = 0;
	TSharedPtr<FAsyncVisionState, ESPMode::ThreadSafe> AsyncVisionState;
	IImageWrapperModule* ImageWrapperModule = nullptr;

	bool bLogCameraStats = false;
	double CameraStatsWindowStartSec = 0.0;
	double CameraStatsPublishSec = 0.0;
	double CameraStatsEncodeSec = 0.0;
	uint64 CameraStatsTickCount = 0;
	uint64 CameraStatsMessageCount = 0;
	uint64 CameraStatsEntryCount = 0;
	uint64 CameraStatsPayloadBytes = 0;

	bool StartTransport();
	void StopTransport();
	void RebuildListeners();

	/** Called by the Core when its robot endpoint set changes (e.g. after
	 *  post-compile rebuild), so listeners are (re)opened for the real robot
	 *  set instead of the empty set seen during BeginPlay. */
	UFUNCTION()
	void OnRobotsChanged();

	void AcceptNewConnections(FRobotListener& Listener);
	void ReadFromClients(FRobotListener& Listener);
	void FlushAllWrites();

	void SendToClients(FRobotListener& Listener, uint8 FrameType, const uint8* PayloadData, int32 PayloadSize);
	void EnqueueFrame(FTcpClient& Client, uint8 FrameType, const uint8* PayloadData, int32 PayloadSize);
	bool FlushClientWrites(FTcpClient& Client);

	void ProcessCommand(const FString& ActorId, const FString& JsonStr);
	void ProcessAdminRequest(FTcpClient& Client, const FString& JsonStr);

	void TickStatePublish();
	void TickCameraPublish();
	void DrainCompletedVisionPackets();

	FString BuildStateJson(const FString& ActorId);

	void CloseSocket(FSocket* Sock);
	void CloseListener(FRobotListener& Listener);
};

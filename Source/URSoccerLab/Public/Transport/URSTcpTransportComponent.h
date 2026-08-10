#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Queue.h"
#include "Scene/URSSceneConfig.h"
#include "Transport/URSTcpProtocol.h"
#include "Network/URSNetworkThread.h"
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab") bool bAutoStart = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1024")) int32 RobotBasePort = URSoccerLab::TcpProtocol::DefaultRobotBasePort;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1024")) int32 AdminPort = URSoccerLab::TcpProtocol::DefaultAdminPort;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1.0")) double StateRateHz = 60.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1.0", ClampMax = "120.0")) double CameraRateHz = 30.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab") FString CameraCompress = TEXT("jpeg");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1", ClampMax = "100")) int32 JpegQuality = 85;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab", meta = (ClampMin = "1.0", ClampMax = "120.0")) double DepthRateHz = 15.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "URSoccerLab") FString DepthCompress = TEXT("zlib_u16_mm");

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	TWeakObjectPtr<UURSRobotCoreComponent> Core;
	TWeakObjectPtr<UURSDisplayClusterCameraBinderComponent> NDisplayBinder;

	// Network thread (handles all robot TCP: state, commands, camera frames)
	URSNetworkThread* NetThread = nullptr;

	// Admin listener stays on game thread (SetPose/Reset need CallbackMutex)
	FSocket* AdminListenerSock = nullptr;
	struct FAdminClient {
		FSocket* Socket = nullptr;
		TArray<uint8> ReadBuf;
		TArray<uint8> WriteBuf;
	};
	TArray<FAdminClient> AdminClients;

	// Camera encoding state (game thread)
	URSoccerLab::FURSVisionConfig VisionConfig;
	IImageWrapperModule* ImageWrapperModule = nullptr;
	double NextRgbTimeSec = 0.0;
	double NextDepthTimeSec = 0.0;

	struct FCompletedVisionPacket {
		int32 RobotIdx = 0;
		uint8 FrameType = 0;
		TArray<uint8> Payload;
	};
	TQueue<FCompletedVisionPacket, EQueueMode::Mpsc> CompletedVisionPackets;
	std::atomic<bool> bVisionAccept{true};

	// Per-robot camera state for the game thread
	struct FCameraState {
		FString ActorId;
		bool bRgbReadbackRequested = false;
		uint64 LastNDisplayRgbSequence = 0;
	};
	TArray<FCameraState> CameraStates;

	// Admin queue: network thread → game thread
	struct FAdminRequest {
		TArray<uint8> Json;
		int32 ClientIdx = -1;
	};
	TQueue<FAdminRequest, EQueueMode::Mpsc> AdminRequestQueue;

	bool StartTransport();
	void StopTransport();
	void RebuildNetworkThread();
	void TickCameraCapture();
	void DrainCompletedVision();
	void TickAdmin();

	UFUNCTION() void OnRobotsChanged();

	void ProcessAdminJson(FAdminClient& Client, const FString& JsonStr);
	void CloseSocket(FSocket* Sock);
};

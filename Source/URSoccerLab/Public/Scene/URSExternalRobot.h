#pragma once

#include "CoreMinimal.h"
#include "MuJoCo/Core/MjArticulation.h"
#include "URSExternalRobot.generated.h"

class AURSExternalRobot;

class UStaticMesh;

namespace URSoccerLab
{
struct FExternalRobotVisual
{
	FString File, SiteName;
	FVector Scale = FVector::OneVector;
	bool bQuaternionPose = true;
};
struct FExternalRobotPackage
{
	FString Id, PhysicsXml, BaseBody, HeadBody, LeftCamera, RightCamera;
	double DefaultBaseHeightM = 0;
	TArray<FExternalRobotVisual> Visuals;
	TArray<FString> Warnings;
	mutable TMap<FString, TWeakObjectPtr<UStaticMesh>> LoadedMeshes;
	// Temporary compiled model resolves MJCF defaults, frames and orientations.
	mjModel *Model = nullptr;
	~FExternalRobotPackage();
};
class URSOCCERLAB_API FExternalRobotLoader
{
  public:
	static TSharedPtr<FExternalRobotPackage> Load(const FString &ManifestPath, FString &Error);
	static bool Populate(AURSExternalRobot *Robot, const FExternalRobotPackage &Package, FString &Error);
};
} // namespace URSoccerLab

/** Generic external articulation; physics is compiled directly from supplied MJCF. */
UCLASS()
class URSOCCERLAB_API AURSExternalRobot : public AMjArticulation
{
	GENERATED_BODY()
  public:
	FString PhysicsXml;
	FString BaseBodyName, HeadBodyName, LeftCameraName, RightCameraName;
	virtual void Setup(mjSpec *Spec, mjVFS *VFS) override;
};

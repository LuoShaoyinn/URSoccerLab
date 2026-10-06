#pragma once
#include "CoreMinimal.h"
namespace URSoccerLab
{
struct FInspectorPose { FVector Position; FQuat Rotation; };
struct FInspectorEvent
{
 enum class EKind { Connected, Disconnected, Pose } Kind;
 uint64 Session = 0;
 FInspectorPose Pose;
};
class URSOCCERLAB_API FInspectorProtocol
{
public:
 static bool Decode(const FString& Json, FInspectorPose& Pose, FString& Error);
 static FString Reply(bool bOk, const FString& Error = FString());
};
}

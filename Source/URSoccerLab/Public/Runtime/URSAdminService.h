#pragma once
#include "CoreMinimal.h"
class UURSRobotCoreComponent;
namespace URSoccerLab
{
// Executes on the game thread. No sockets, connection IDs, or transport framing.
class URSOCCERLAB_API FAdminService
{
public:
	static FString Execute(UURSRobotCoreComponent* Core, const FString& Json);
};
} // namespace URSoccerLab

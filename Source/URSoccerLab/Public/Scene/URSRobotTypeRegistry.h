#pragma once

#include "CoreMinimal.h"

namespace URSoccerLab
{
struct URSOCCERLAB_API FURSRobotType
{
	FString Name;
	FString BlueprintAssetPath;
	double DefaultBaseHeightM = 0.0;
	// Fixed runtime robot types define every non-root MuJoCo joint explicitly.
	// Reset rejects incomplete definitions instead of silently falling back to zero.
	TMap<FString, float> DefaultJointPositions;
};

class URSOCCERLAB_API FURSRobotTypeRegistry
{
public:
	static FURSRobotTypeRegistry& Get();

	void Register(const FURSRobotType& Type);
	void RegisterDefaultTypes();
	const FURSRobotType* Find(const FString& Name) const;
	TArray<FString> GetRegisteredNames() const;

private:
	FURSRobotTypeRegistry() = default;
	TMap<FString, FURSRobotType> Types;
	bool bDefaultsRegistered = false;
};
} // namespace URSoccerLab

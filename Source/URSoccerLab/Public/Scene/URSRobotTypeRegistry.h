#pragma once

#include "CoreMinimal.h"

namespace URSoccerLab
{
struct FURSRobotType
{
	FString Name;
	FString BlueprintAssetPath;
	double DefaultBaseHeightM = 0.0;
};

class FURSRobotTypeRegistry
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

#pragma once

#include "CoreMinimal.h"

namespace URSoccerLab
{
struct URSOCCERLAB_API FURSObjectType
{
	FString Name;
	FString BlueprintAssetPath;
	double DefaultBaseHeightM = 0.0;
};

class URSOCCERLAB_API FURSObjectTypeRegistry
{
public:
	static FURSObjectTypeRegistry& Get();

	void Register(const FURSObjectType& Type);
	void RegisterDefaultTypes();
	const FURSObjectType* Find(const FString& Name) const;

private:
	FURSObjectTypeRegistry() = default;
	TMap<FString, FURSObjectType> Types;
	bool bDefaultsRegistered = false;
};
} // namespace URSoccerLab

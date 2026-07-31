#include "Scene/URSObjectTypeRegistry.h"

namespace URSoccerLab
{
FURSObjectTypeRegistry& FURSObjectTypeRegistry::Get()
{
	static FURSObjectTypeRegistry Instance;
	return Instance;
}

void FURSObjectTypeRegistry::Register(const FURSObjectType& Type)
{
	if (!Type.Name.IsEmpty())
	{
		Types.Add(Type.Name, Type);
	}
}

void FURSObjectTypeRegistry::RegisterDefaultTypes()
{
	if (bDefaultsRegistered)
	{
		return;
	}
	bDefaultsRegistered = true;

	FURSObjectType SoccerBall;
	SoccerBall.Name = TEXT("soccer_ball");
	SoccerBall.BlueprintAssetPath =
		TEXT("/Game/URSoccerLab/Objects/soccer_ball/soccer_ball.soccer_ball");
	SoccerBall.DefaultBaseHeightM = 0.075;
	Register(SoccerBall);
}

const FURSObjectType* FURSObjectTypeRegistry::Find(const FString& Name) const
{
	return Types.Find(Name);
}
} // namespace URSoccerLab

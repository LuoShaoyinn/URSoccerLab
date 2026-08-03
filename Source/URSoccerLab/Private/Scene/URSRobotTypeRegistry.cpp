#include "Scene/URSRobotTypeRegistry.h"

namespace URSoccerLab
{
FURSRobotTypeRegistry& FURSRobotTypeRegistry::Get()
{
	static FURSRobotTypeRegistry Instance;
	return Instance;
}

void FURSRobotTypeRegistry::Register(const FURSRobotType& Type)
{
	if (Type.Name.IsEmpty())
	{
		return;
	}
	Types.Add(Type.Name, Type);
}

void FURSRobotTypeRegistry::RegisterDefaultTypes()
{
	if (bDefaultsRegistered)
	{
		return;
	}
	bDefaultsRegistered = true;

	FURSRobotType PiPlus;
	PiPlus.Name = TEXT("pi_plus");
	PiPlus.BlueprintAssetPath = TEXT("/Game/URSoccerLab/Robots/pi_plus/pi_plus.pi_plus");
	PiPlus.DefaultBaseHeightM = 0.3762;
	Register(PiPlus);

	FURSRobotType Mos9;
	Mos9.Name = TEXT("mos9");
	Mos9.BlueprintAssetPath = TEXT("/Game/URSoccerLab/Robots/mos9/mos9.mos9");
	Mos9.DefaultBaseHeightM = 0.45;
	Register(Mos9);
}

const FURSRobotType* FURSRobotTypeRegistry::Find(const FString& Name) const
{
	return Types.Find(Name);
}

TArray<FString> FURSRobotTypeRegistry::GetRegisteredNames() const
{
	TArray<FString> Names;
	Types.GetKeys(Names);
	return Names;
}
} // namespace URSoccerLab

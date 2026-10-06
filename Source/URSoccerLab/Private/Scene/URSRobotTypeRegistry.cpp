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

	// Actual robot types are declared by external scene manifests.

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

#include "Runtime/URSRobotNames.h"

namespace URSoccerLab
{
FString FRobotNames::NormalizeImportedComponentName(const FString& Name)
{
	const int32 ClassMarker = Name.Find(TEXT("_C_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (ClassMarker == INDEX_NONE)
	{
		return Name;
	}

	int32 Index = ClassMarker + 3;
	while (Index < Name.Len() && FChar::IsDigit(Name[Index]))
	{
		++Index;
	}
	if (Index < Name.Len() && Name[Index] == TEXT('_'))
	{
		return Name.RightChop(Index + 1);
	}

	return Name;
}

FString FRobotNames::NormalizeRobotComponentName(const FString& Name, const FString& RobotName)
{
	FString CleanName = NormalizeImportedComponentName(Name);
	const FString RobotPrefix = RobotName + TEXT("_");
	if (!RobotName.IsEmpty() && CleanName.StartsWith(RobotPrefix, ESearchCase::CaseSensitive))
	{
		CleanName.RightChopInline(RobotPrefix.Len());
	}
	return CleanName;
}
} // namespace URSoccerLab

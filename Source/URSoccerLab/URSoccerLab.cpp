// Copyright Epic Games, Inc. All Rights Reserved.

#include "URSoccerLab.h"
#include "Modules/ModuleManager.h"
#include "Scene/URSRobotTypeRegistry.h"

class FURSoccerLabModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		URSoccerLab::FURSRobotTypeRegistry::Get().RegisterDefaultTypes();
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FURSoccerLabModule, URSoccerLab, "URSoccerLab");

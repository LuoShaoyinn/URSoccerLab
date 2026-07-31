// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class URSoccerLab : ModuleRules
{
	public URSoccerLab(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "URLab" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"JsonUtilities",
			"Sockets",
			"ImageWrapper",
			"DisplayCluster",
			"DisplayClusterProjection",
			"RenderCore",
			"RHI",
			"Slate",
			"SlateCore"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}

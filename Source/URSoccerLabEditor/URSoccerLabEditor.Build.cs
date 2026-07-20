// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class URSoccerLabEditor : ModuleRules
{
	public URSoccerLabEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"URLab",
			"URLabEditor",
			"URSoccerLab"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"AssetRegistry",
			"AssetTools",
			"UnrealEd"
		});
	}
}

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

			// yyjson — vendored single-file JSON library (much faster than UE Json)
			string yyjsonDir = System.IO.Path.Combine(ModuleDirectory, "ThirdParty", "yyjson");
			PublicIncludePaths.Add(System.IO.Path.Combine(yyjsonDir, "include"));
		}
	}

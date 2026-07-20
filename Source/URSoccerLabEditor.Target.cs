// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class URSoccerLabEditorTarget : TargetRules
{
	public URSoccerLabEditorTarget( TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		bUseUnityBuild = false;
		ExtraModuleNames.Add("URSoccerLab");
		ExtraModuleNames.Add("URSoccerLabEditor");
	}
}

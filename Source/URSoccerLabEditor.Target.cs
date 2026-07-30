using UnrealBuildTool;

public class URSoccerLabEditorTarget : TargetRules
{
	public URSoccerLabEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("URSoccerLab");
	}
}

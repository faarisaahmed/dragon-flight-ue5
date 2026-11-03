using UnrealBuildTool;

public class DragonFlightEditorTarget : TargetRules
{
	public DragonFlightEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "DragonFlight", "DragonFlightEditor" });
	}
}

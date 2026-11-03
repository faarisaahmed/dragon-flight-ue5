using UnrealBuildTool;

public class DragonFlightTarget : TargetRules
{
	public DragonFlightTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("DragonFlight");
	}
}

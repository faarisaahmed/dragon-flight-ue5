using UnrealBuildTool;

public class DragonFlight : ModuleRules
{
	public DragonFlight(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput",
			"Landscape", "Water", "Niagara", "UMG", "Slate", "SlateCore",
			"Json", "JsonUtilities", "RenderCore", "RHI", "PhysicsCore",
			"DeveloperSettings", "ImageWrapper"
		});
	}
}

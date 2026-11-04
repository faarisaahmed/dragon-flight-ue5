using UnrealBuildTool;

public class DragonFlight : ModuleRules
{
	public DragonFlight(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Each .cpp keeps small helpers in an anonymous namespace; unity builds would collide them.
		bUseUnity = false;
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput",
			"Landscape", "Water", "Niagara", "UMG", "Slate", "SlateCore",
			"Json", "JsonUtilities", "RenderCore", "RHI", "PhysicsCore",
			"DeveloperSettings", "ImageWrapper", "ProceduralMeshComponent"
		});
	}
}

using UnrealBuildTool;

public class DragonFlightEditor : ModuleRules
{
	public DragonFlightEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Each .cpp keeps small helpers in an anonymous namespace; unity builds would collide them.
		bUseUnity = false;
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "DragonFlight"
		});
		PrivateDependencyModuleNames.AddRange(new string[] {
			"UnrealEd", "Landscape", "LandscapeEditor", "Water", "AssetTools", "AssetRegistry",
			"MaterialEditor", "Json", "JsonUtilities", "ImageWrapper", "Foliage",
			"LevelEditor", "EditorScriptingUtilities", "Slate", "SlateCore", "ToolMenus",
			"MeshDescription", "StaticMeshDescription", "MeshUtilitiesCommon", "RenderCore", "RHI"
		});
	}
}

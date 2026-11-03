using UnrealBuildTool;

public class DragonFlightEditor : ModuleRules
{
	public DragonFlightEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "DragonFlight"
		});
		PrivateDependencyModuleNames.AddRange(new string[] {
			"UnrealEd", "Landscape", "LandscapeEditor", "Water", "AssetTools", "AssetRegistry",
			"MaterialEditor", "Json", "JsonUtilities", "ImageWrapper", "Foliage",
			"LevelEditor", "EditorScriptingUtilities", "Slate", "SlateCore", "ToolMenus",
			"MeshDescription", "StaticMeshDescription", "MeshUtilitiesCommon"
		});
	}
}

#pragma once

#include "CoreMinimal.h"

class UTexture2D;
class UMaterial;
class UMaterialInterface;
class UStaticMesh;

struct FDragonBuildOptions
{
	/** Heightmap vertices per side: 4033 (laptop / Mac), 8129 (default), 16321 (max). 0 = pick for this machine. */
	int32 Res = 0;
	bool bTextures = true;
	bool bModels = true;
	bool bLandscape = true;
	bool bWater = true;
	/** Also place Water-plugin bodies (ocean, lakes, rivers). Their meshes need the interactive editor. */
	bool bWaterPlugin = false;
	bool bRivers = true;
	bool bSave = true;
	/** Only (re)import the dragon; leave textures, models and the map alone. */
	bool bDragonOnly = false;
	/** Build a Nanite representation of the landscape. At 8129^2 this needs ~16 GB in one go: off by default (24 GB machines thrash). */
	bool bLandscapeNanite = false;
	/** Only (re)import the village kit. */
	bool bVillageOnly = false;
};

/**
 * Builds the whole playable world from files on disk, no hand work:
 *
 *   Data/Downloads/Textures  -> /Game/World/Textures   (Tools/fetch_assets.py)
 *   Data/Downloads/Models    -> /Game/World/Models     (glTF, Nanite on)
 *   code                     -> /Game/World/Materials  (landscape, foliage, rock, plasma)
 *   Data/World/Heightmap_N   -> Landscape, 45 km square (Tools/WorldBake)
 *   Data/World/World.json    -> ocean, lakes, rivers, spawn
 *   -> /Game/Maps/Archipelago
 *
 * Run it from the editor (Tools > Dragon Flight > Build Archipelago, or the
 * console: Dragon.BuildArchipelago 8129) or headless:
 *   UnrealEditor-Cmd DragonFlight.uproject -run=BuildArchipelago -Res=8129
 */
class DRAGONFLIGHTEDITOR_API FDragonArchipelagoBuilder
{
public:
	static bool Build(const FDragonBuildOptions& Options);

	// Steps, public so they can be re-run on their own.
	static void ImportGroundTextures();
	static UMaterial* CreateLandscapeMaterial();
	static void CreateMasterMaterials();
	static TMap<FName, TArray<TSoftObjectPtr<UStaticMesh>>> ImportModels();
	/** Data/Dragon/SK_Dragon.fbx (+ Textures/) -> /Game/Dragon. */
	static bool ImportDragon();
	/** The Norse building kit: Data/Village/<kind>_<variant>.glb -> /Game/World/Village/<name>/ (Nanite). */
	static int32 ImportVillage();
	static bool BuildLevel(const FDragonBuildOptions& Options, const TMap<FName, TArray<TSoftObjectPtr<UStaticMesh>>>& Meshes);

	static UTexture2D* ImportTexture(const FString& File, const FString& PackagePath, const FString& AssetName, bool bNormal, bool bLinear);
	static FString DownloadsDir();
};

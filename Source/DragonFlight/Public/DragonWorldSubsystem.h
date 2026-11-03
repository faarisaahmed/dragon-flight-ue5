#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "DragonWorldSubsystem.generated.h"

USTRUCT(BlueprintType)
struct FDragonIsland
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FString Name;
	/** Centre in UE world space (cm), Z = summit height at centre. */
	UPROPERTY(BlueprintReadOnly) FVector Centre = FVector::ZeroVector;
	/** Rough radius (cm). */
	UPROPERTY(BlueprintReadOnly) double Radius = 0.0;
	/** Authored peak height (cm). */
	UPROPERTY(BlueprintReadOnly) double Peak = 0.0;
};

USTRUCT(BlueprintType)
struct FDragonRiver
{
	GENERATED_BODY()
	/** Points in UE space (cm), and the channel width at each (cm). */
	UPROPERTY(BlueprintReadOnly) TArray<FVector> Points;
	UPROPERTY(BlueprintReadOnly) TArray<float> Widths;
};

USTRUCT(BlueprintType)
struct FDragonLake
{
	GENERATED_BODY()
	/** Water surface height (cm). */
	UPROPERTY(BlueprintReadOnly) double Level = 0.0;
	/** Bounds of the lake grid in UE XY (cm). */
	UPROPERTY(BlueprintReadOnly) FBox2D Bounds = FBox2D(ForceInit);
};

/**
 * Everything WorldBake exported about the archipelago (Data/World/World.json),
 * plus a fast CPU height lookup from the smallest baked heightmap — the same
 * job terrainHeight() did twenty times a frame in the web game.
 */
UCLASS()
class DRAGONFLIGHT_API UDragonWorldSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	static FString DataDir();

	/** Loads World.json (+ a heightmap for HeightAt). Safe to call repeatedly. */
	bool Load();

	UFUNCTION(BlueprintPure, Category = "Dragon|World")
	bool IsLoaded() const { return bLoaded; }

	/** World edge length in cm (45 km square). */
	UFUNCTION(BlueprintPure, Category = "Dragon|World")
	double GetTerrainSize() const { return TerrainSize; }

	UFUNCTION(BlueprintPure, Category = "Dragon|World")
	FVector GetSpawn() const { return Spawn; }

	UFUNCTION(BlueprintPure, Category = "Dragon|World")
	FVector GetHome() const { return Home; }

	/** Ground height (cm) at a UE world XY, bilinear from the baked heightmap. Sea floor is negative. */
	UFUNCTION(BlueprintPure, Category = "Dragon|World")
	double HeightAt(double X, double Y) const;

	/** The named island nearest to a point, and the distance to its centre (cm). */
	UFUNCTION(BlueprintPure, Category = "Dragon|World")
	FString NearestIsland(const FVector& Where, double& OutDistance) const;

	const TArray<FDragonIsland>& GetIslands() const { return Islands; }
	const TArray<FDragonRiver>& GetRivers() const { return Rivers; }
	const TArray<FDragonLake>& GetLakes() const { return Lakes; }

	/** Raw JSON for anything that wants more (village plots, glacier fronts, pads). */
	TSharedPtr<class FJsonObject> GetJson() const { return Json; }

private:
	bool LoadHeightmap();

	bool bLoaded = false;
	double TerrainSize = 4500000.0;
	double SeaLevel = 0.0;
	FVector Spawn = FVector::ZeroVector;
	FVector Home = FVector::ZeroVector;
	TArray<FDragonIsland> Islands;
	TArray<FDragonRiver> Rivers;
	TArray<FDragonLake> Lakes;
	TSharedPtr<class FJsonObject> Json;

	// Heightmap for CPU lookups
	TArray<uint16> Heights;
	int32 HeightRes = 0;
	double ZScalePercent = 400.0;
};

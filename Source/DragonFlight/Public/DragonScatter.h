#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DragonScatter.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;

UENUM(BlueprintType)
enum class EDragonScatterKind : uint8
{
	Tree,      // conifers, by forest mask
	Sapling,   // young trees at wood edges and in open turf
	DeadWood,  // stumps and fallen trunks in the woods
	Rock,      // boulders on scree, slopes and shores
	Cliff,     // big rock faces on steep ground
	Ground,    // grass, ferns, shrubs, flowers near the camera
	Stone,     // small stones in the turf near the camera
	Shrub,     // juniper, heather and fern clumps, seen from the air
	Iceberg    // drift ice off the arctic islands (rock and cliff shapes in M_Ice); added at runtime
};

/** One scatter layer: which meshes, how dense, how far, where. */
USTRUCT(BlueprintType)
struct FDragonScatterLayer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FName Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) EDragonScatterKind Kind = EDragonScatterKind::Tree;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<TSoftObjectPtr<UStaticMesh>> Meshes;
	/** Candidates per hectare before the mask/slope rules thin them out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float PerHectare = 120.f;
	/** Tile edge in metres; smaller for dense close layers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float TileM = 256.f;
	/** Draw/stream radius in metres at Max quality (scaled down on lower presets). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float RadiusM = 6000.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector2D ScaleRange = FVector2D(0.8f, 1.3f);
	/** Sink into the ground (metres) so bases never float on slopes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float SinkM = 0.3f;
	/** Tilt toward the slope normal, 0 = upright (trees), 1 = flush (rocks). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float AlignToSlope = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bCastShadow = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bCollide = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Seed = 1;
};

/**
 * Streams vegetation and rocks around the camera, the way the web game's
 * forest.js / grassfield.js did: the world is cut into tiles, each tile is
 * filled deterministically (same seed, same trees, every visit) from the baked
 * masks (Data/World/Masks_*.png: forest, vegetation, ice, water, slope, pad),
 * and tiles beyond the radius are dropped. Generation is time-sliced so it
 * never hitches, and all of it is hierarchical instanced meshes (Nanite where
 * the meshes allow).
 */
UCLASS()
class DRAGONFLIGHT_API ADragonScatter : public AActor
{
	GENERATED_BODY()

public:
	ADragonScatter();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter") TArray<FDragonScatterLayer> ScatterLayers;

	/** Milliseconds per frame the generator may spend. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter") float BudgetMs = 3.f;

	/** Multiplies every radius; set from the graphics preset (foliage quality). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter") float DistanceScale = 1.f;

	/** Fills ScatterLayers with sensible defaults from meshes found under /Game/World/Models/<group>/. Editor builder calls this. */
	void SetupDefaultLayers(const TMap<FName, TArray<TSoftObjectPtr<UStaticMesh>>>& MeshesByGroup);

	/** Tiles still waiting to be generated. */
	bool IsBusy() const;

	/** Forest cover 0..1 at a world point (cm); loads the masks if needed. */
	float ForestAt(double X, double Y);

private:
	struct FTileKey
	{
		int32 Layer, X, Y;
		bool operator==(const FTileKey& O) const { return Layer == O.Layer && X == O.X && Y == O.Y; }
		friend uint32 GetTypeHash(const FTileKey& K) { return HashCombine(HashCombine(::GetTypeHash(K.Layer), ::GetTypeHash(K.X)), ::GetTypeHash(K.Y)); }
	};
	struct FTile
	{
		// One HISM per mesh variant in the layer.
		TArray<TObjectPtr<UInstancedStaticMeshComponent>> Components;
	};

	bool LoadMasks();
	FVector4f SampleMask(double XM, double YM) const;      // R forest, G veg, B ice, A water
	FVector SampleMask2(double XM, double YM) const;      // slope, firn, pad
	FVector SampleMask3(double XM, double YM) const;      // woodland, volcanic, moorland (biomes.py)
	float GroundM(double XM, double YM) const;
	FVector GroundNormal(double XM, double YM) const;
	bool Accept(const FDragonScatterLayer& L, double XM, double YM, float H, float Rand, float& OutScaleMul) const;
	void BuildTile(int32 LayerIndex, int32 TX, int32 TY);
	void DropTile(const FTileKey& Key);
	FVector ViewPointM() const;
	float QualityScale() const;

	TMap<FTileKey, FTile> Tiles;
	TArray<FTileKey> Queue;
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMesh>> LoadedMeshes; // keeps soft refs resident (UPROPERTY so GC sees them)
	TArray<TArray<UStaticMesh*>> LayerMeshes;
	/** Per layer, per mesh: a wave-worn coast piece (else inland). Empty when the layer doesn't mix them. */
	TArray<TArray<bool>> ShoreMesh;
	const class UDragonWorldSubsystem* WorldSub = nullptr;

	TArray<uint8> Mask;   // RGBA
	TArray<uint8> Mask2;  // RGB
	TArray<uint8> Mask3;  // RGB biomes
	int32 MaskRes = 0;
	double WorldSizeM = 45000.0;
	FVector LastPlanM = FVector(1e12);
	float PlanTimer = 0.f;
	float StatTimer = 5.f;

	UPROPERTY(Transient) TArray<TObjectPtr<UInstancedStaticMeshComponent>> Pool;
	UPROPERTY(Transient) TObjectPtr<class UMaterialInterface> IceMaterial;
};

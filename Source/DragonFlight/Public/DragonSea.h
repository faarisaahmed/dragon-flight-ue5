#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DragonSea.generated.h"

class UStaticMeshComponent;
class UMaterialInterface;

/**
 * The sea, and the lakes. One flat water surface at sea level reaching past the
 * horizon, plus one at each lake's level over its bounds; the terrain hides
 * whatever lies outside the real shoreline, exactly as in the web game. Shaded
 * with UE's Single Layer Water model (M_Sea / M_Lake, built by the builder):
 * absorption and scattering by depth, refraction, animated wave normals.
 *
 * Used instead of the Water plugin's ocean, whose mesh is only built by the
 * interactive editor and never appears in a scripted build.
 */
UCLASS()
class DRAGONFLIGHT_API ADragonSea : public AActor
{
	GENERATED_BODY()

public:
	ADragonSea();
	virtual void BeginPlay() override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sea") TObjectPtr<UMaterialInterface> SeaMaterial;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sea") TObjectPtr<UMaterialInterface> LakeMaterial;

	/** Half-width of the sea surface, cm (well past the horizon from 1 km up). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sea") double SeaHalfExtent = 25000000.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sea") bool bLakes = true;

	UPROPERTY(VisibleAnywhere, Category = "Sea") TObjectPtr<UStaticMeshComponent> Surface;

private:
	bool BuildLake(const class UDragonWorldSubsystem& W, const struct FDragonLake& L, int32 Index);
	UPROPERTY(Transient) TArray<TObjectPtr<class UProceduralMeshComponent>> LakeSurfaces;
};

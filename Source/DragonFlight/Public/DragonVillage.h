#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DragonVillage.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;

/**
 * The villages: every house plot the web game's world bake recorded (World.json
 * "village": position, levelled height, yaw, kind, scale), built from the Norse
 * building kit imported under /Game/World/Village/ (one or more meshes per kind,
 * named <kind>_<variant>, e.g. cottage_a). One instanced component per mesh.
 *
 * Spawned by the game mode at start; it reads the plots from UDragonWorldSubsystem.
 */
UCLASS()
class DRAGONFLIGHT_API ADragonVillage : public AActor
{
	GENERATED_BODY()

public:
	ADragonVillage();
	virtual void BeginPlay() override;

	/** Added to each plot's yaw so the kit's front faces where the web game's houses faced (degrees). */
	UPROPERTY(EditAnywhere, Category = "Village") float YawOffsetDeg = 90.f;
	/** Houses sit this far into their plot, so footings never float on a slight slope (m). */
	UPROPERTY(EditAnywhere, Category = "Village") float SinkM = 0.35f;
	/** Where the kit lives. */
	UPROPERTY(EditAnywhere, Category = "Village") FString KitPath = TEXT("/Game/World/Village");

	int32 NumPlaced() const { return Placed; }

private:
	TMap<FString, TArray<UStaticMesh*>> LoadKit() const;

	UPROPERTY(VisibleAnywhere, Category = "Village") TObjectPtr<USceneComponent> Root;
	UPROPERTY(Transient) TArray<TObjectPtr<UInstancedStaticMeshComponent>> Parts;
	int32 Placed = 0;
};

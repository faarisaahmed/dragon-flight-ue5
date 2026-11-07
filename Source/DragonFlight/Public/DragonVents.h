#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DragonVents.generated.h"

/**
 * Steam from the volcanic isles (World.json "biomes"): fumaroles on the young lava,
 * each breathing a plume that rises and blows away with the wind. Only the vents within a
 * few kilometres of the camera puff, through ADragonFX. Made by the game mode at start.
 */
UCLASS()
class DRAGONFLIGHT_API ADragonVents : public AActor
{
	GENERATED_BODY()

public:
	ADragonVents();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	struct FVent { FVector At; float Strength; float Timer; };
	TArray<FVent> Vents;
	uint32 Seed = 777;
	float Rand() { Seed = Seed * 1664525u + 1013904223u; return ((Seed >> 8) & 0xFFFF) / 65535.f; }
};

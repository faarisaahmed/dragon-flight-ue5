#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DragonSurfSpray.generated.h"

/**
 * Swell bursting on the rocks: around the camera it finds where deep water meets a cliff or a
 * skerry (from the heightmap) and, every few seconds at each, throws spray up the face and a
 * drift of salt mist off it, bigger the rougher the sea. Through ADragonFX; made by the game
 * mode at start. The white water on the sea itself is the sea material's (coast.py field).
 */
UCLASS()
class DRAGONFLIGHT_API ADragonSurfSpray : public AActor
{
	GENERATED_BODY()

public:
	ADragonSurfSpray();
	virtual void Tick(float DeltaSeconds) override;

private:
	struct FSpot { FVector At; FVector Out; float Timer; float Size; };
	TArray<FSpot> Spots;
	FVector LastSearch = FVector(1e12);
	float SearchTimer = 0.f;
	uint32 Seed = 4242;
	float Rand() { Seed = Seed * 1664525u + 1013904223u; return ((Seed >> 8) & 0xFFFF) / 65535.f; }
	void Search(const FVector& Cam);
	void Burst(const FSpot& S, float Sea);
};

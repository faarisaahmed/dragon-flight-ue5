#pragma once

#include "CoreMinimal.h"
#include "Components/AudioComponent.h"
#include <atomic>
#include "DragonSfx.generated.h"

class USoundWaveProcedural;
class USoundAttenuation;

/**
 * Sound effects synthesised at runtime, like the wind (no sound files to license or ship).
 * One-shots are rendered once into PCM (a few variants each) and played through a fresh
 * procedural wave at a place in the world, with distance attenuation and air absorption.
 *
 *   Plasma shot  a charged crackle and a shriek that rises then falls away as the bolt leaves
 *   Blast        crack, a deep boom with a long rumbling tail, debris patter (or a splash)
 *   Thunder      a near crack or a distant roll, timed to the lightning by distance
 */
namespace DragonSfx
{
	void PlasmaShot(const UObject* WorldContext, const FVector& At);
	void PlasmaBlast(const UObject* WorldContext, const FVector& At, bool bWater);
	/** Distance in cm decides crack vs roll and the delay is the caller's (sound travels 343 m/s). */
	void Thunder(const UObject* WorldContext, const FVector& At, float DistanceCm);
	/** One wingbeat's push of air: Strength 0..1, Seconds = length of the downstroke. */
	void Wingbeat(const UObject* WorldContext, const FVector& At, float Strength, float Seconds);
	/** The dragon's weight coming down: touchdown, a hard step. Strength 0..1. */
	void Thud(const UObject* WorldContext, const FVector& At, float Strength);
	/** Water struck: a slap and falling spray. Strength 0..1. */
	void Splash(const UObject* WorldContext, const FVector& At, float Strength);
}

/**
 * The weather and the sea you hear: rain on the air (and on the dragon), surf on the
 * shore when you are near and low, and a soft sea wash over open water. Synthesised live;
 * the game thread sets the levels.
 */
UCLASS(ClassGroup = (Dragon), meta = (BlueprintSpawnableComponent))
class DRAGONFLIGHT_API UDragonAmbience : public UAudioComponent
{
	GENERATED_BODY()

public:
	UDragonAmbience();
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UPROPERTY(EditAnywhere, Category = "Ambience") float MasterVolume = 0.8f;
	/** The pawn: how hard a wingtip, claws or the belly are cutting the water this frame (0..1). */
	void SetSkim(float V) { if (State) State->Skim.store(FMath::Clamp(V, 0.f, 1.f), std::memory_order_relaxed); }

private:
	struct FState
	{
		std::atomic<float> Rain{ 0.f };    // 0..1
		std::atomic<float> Surf{ 0.f };    // 0..1
		std::atomic<float> Sea{ 0.f };     // 0..1 open-water wash
		std::atomic<float> Storm{ 0.f };   // 0..1 wind roar under the rain
		std::atomic<float> Skim{ 0.f };    // 0..1 something cutting through the water close by
		float SkimHpL = 0, SkimHpR = 0, SkimLpL = 0, SkimLpR = 0, SmSkim = 0;
		// audio thread
		float RainLpL = 0, RainLpR = 0, RainHpL = 0, RainHpR = 0;
		float SurfLp1L = 0, SurfLp1R = 0, SurfLp2L = 0, SurfLp2R = 0;
		float SeaLpL = 0, SeaLpR = 0, BrownL = 0, BrownR = 0;
		float SmRain = 0, SmSurf = 0, SmSea = 0, SmStorm = 0;
		double Phase = 0.0;
		float WaveEnv = 0.f, WaveTarget = 0.f; int32 WaveTimer = 0;
		float DropL = 0, DropR = 0;
		uint32 Seed = 98765;
	};
	TSharedPtr<FState, ESPMode::ThreadSafe> State;
	UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> Wave;
	float ProbeTimer = 0.f;
	float ShoreNear = 0.f;
};

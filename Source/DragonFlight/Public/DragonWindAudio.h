#pragma once

#include "CoreMinimal.h"
#include "Components/AudioComponent.h"
#include <atomic>
#include "DragonWindAudio.generated.h"

class USoundWaveProcedural;

/**
 * Wind past the dragon's head, synthesised live — no sound files. Two layers of
 * filtered noise: a low rush whose loudness and brightness follow airspeed, and
 * a thin whistle that only comes in near flat out. Slow random gusts on top.
 * SetSpeed() each frame from the game thread; the audio thread reads atomics.
 */
UCLASS(ClassGroup = (Dragon), meta = (BlueprintSpawnableComponent))
class DRAGONFLIGHT_API UDragonWindAudio : public UAudioComponent
{
	GENERATED_BODY()

public:
	UDragonWindAudio();
	virtual void BeginPlay() override;

	/** 0..1 of flat out (335 m/s), and whether he's on the ground. */
	void SetFlight(float SpeedT, float TurboT, bool bGrounded);

	UPROPERTY(EditAnywhere, Category = "Wind") float MasterVolume = 0.9f;

private:
	struct FState
	{
		std::atomic<float> Speed{ 0.f };
		std::atomic<float> Turbo{ 0.f };
		// audio-thread only
		float Lp1L = 0, Lp1R = 0, Lp2L = 0, Lp2R = 0, HpL = 0, HpR = 0, BpL = 0, BpR = 0, BpL2 = 0, BpR2 = 0;
		float Gust = 0, GustTarget = 0, GustTimer = 0, SmoothSpeed = 0;
		uint32 Seed = 22222;
	};
	TSharedPtr<FState, ESPMode::ThreadSafe> State;

	UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> Wave;
};

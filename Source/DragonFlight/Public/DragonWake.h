#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DragonWake.generated.h"

class UTextureRenderTarget2D;
class UMaterialInstanceDynamic;

/**
 * The water's own response to the dragon: a real-time wave-equation simulation on the GPU,
 * in a window that travels with the camera (768 x 768 cells of 0.5 m, 384 m across).
 *
 *     h(t+dt) = [2 h(t) - h(t-dt) + (c dt / dx)^2 * laplacian(h)] * damping  + sources
 *
 * Anything that touches the sea pushes it through Disturb(): a wingtip slicing it (a moving
 * source, so a V-shaped wake falls out of the equation), feet and tail dragging, the
 * downwash under the wings, a plasma bolt, the dragon swimming. Ripples then run out at
 * their own speed, reflect nothing (the window's edge absorbs), and fade. A third channel
 * carries the foam churned up at each source; it stays where it was made and decays.
 *
 * M_Sea reads the result (WakeTex, WakeCentre, WakeSize) for its normal and foam.
 * Ping-pong render targets, RGBA16F: R = h(t)/4 + 0.5, G = h(t-dt)/4 + 0.5, B = foam (the
 * material write is clamped to 0..1, so heights are stored scaled and offset).
 */
UCLASS()
class DRAGONFLIGHT_API ADragonWake : public AActor
{
	GENERATED_BODY()

public:
	ADragonWake();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** One per world, made on first use. */
	static ADragonWake* Get(const UObject* WorldContext);

	/**
	 * Press the water at a point (cm, world), as an object in it would. RadiusCm: its size; Depth:
	 * the surface level it holds there, metres (negative = pressed down); Foam: 0..1 per second of
	 * white water. Hold it there frame after frame (a wingtip, a swimmer) or once (a blow).
	 * Calls within a frame accumulate; the strongest few are applied each simulation step.
	 */
	void Disturb(const FVector& AtCm, float RadiusCm, float Depth, float Foam);

	UPROPERTY(EditAnywhere, Category = "Wake") int32 Cells = 768;
	UPROPERTY(EditAnywhere, Category = "Wake") float CellM = 0.5f;
	/** Ripple speed, m/s (2-3 m/s is the speed of metre-scale ripples on deep water). */
	UPROPERTY(EditAnywhere, Category = "Wake") float WaveSpeed = 2.6f;
	UPROPERTY(EditAnywhere, Category = "Wake") float Damping = 0.9965f;
	UPROPERTY(EditAnywhere, Category = "Wake") float FoamDecay = 0.9955f;

private:
	struct FSource { FVector2D AtCm; float RadiusCm; float Strength; float Foam; };
	TArray<FSource> Pending;
	UPROPERTY(Transient) TObjectPtr<UTextureRenderTarget2D> RT[2];
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> SimMID;
	int32 Cur = 0;
	FVector2D CentreCm = FVector2D::ZeroVector;
	FVector2D PendingShift = FVector2D::ZeroVector;   // recentring not yet applied by a step
	bool bCentred = false;
	float Accum = 0.f;
	float DiagT = 0.f;
	float Idle = 99.f;          // seconds since anything disturbed the water
	bool bBound = false;
	void Step(const FVector2D& OffsetUV, const TArray<FSource>& Src, float StepDt);
	void BindSea();
};

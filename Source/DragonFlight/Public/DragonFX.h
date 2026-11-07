#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DragonFX.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UPointLightComponent;
class UDecalComponent;

/** How a sprite is drawn: glowing (additive light, fire, plasma) or smoke/spray (soft, lit by the sky). */
UENUM()
enum class EDragonPuff : uint8
{
	Glow,
	Smoke,
	Spray
};

/** One camera-facing sprite: where it goes, how it grows and fades. */
struct FDragonPuff
{
	EDragonPuff Kind = EDragonPuff::Glow;
	FVector Pos = FVector::ZeroVector;     // cm
	FVector Vel = FVector::ZeroVector;     // cm/s
	float Drag = 0.f;                       // 1/s
	float Gravity = 0.f;                    // cm/s^2, + down
	float Buoyancy = 0.f;                   // cm/s^2, + up
	float Life = 1.f, Age = 0.f;
	float Size0 = 100.f, Size1 = 100.f;     // diameter, cm
	float GrowPow = 0.5f;                   // size follows age^GrowPow
	FLinearColor Colour0 = FLinearColor::White, Colour1 = FLinearColor::White;   // over life
	float Intensity = 1.f;                  // glow brightness / smoke opacity
	float FadeIn = 0.05f;                   // share of life spent appearing
	float Stretch = 0.f;                    // streaks: length along velocity per (cm/s), 0 = round
	float Spin = 0.f;
	FVector Axis = FVector::ZeroVector;     // fixed streak direction (lightning), with LengthCm
	float LengthCm = 0.f;
};

/**
 * Effects without authored assets: plasma bolts' trails, blasts, fire, smoke, embers,
 * spray, scorch marks and flash lights, all from a handful of materials the builder
 * makes (M_FxGlow, M_FxSmoke, M_PlasmaCore, M_Scorch). Sprites are pooled plane meshes
 * turned to face the camera every frame; there are never more than a few hundred.
 *
 * One per world, made on first use: ADragonFX::Get(this).
 */
UCLASS()
class DRAGONFLIGHT_API ADragonFX : public AActor
{
	GENERATED_BODY()

public:
	ADragonFX();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	static ADragonFX* Get(const UObject* WorldContext);

	void Spawn(const FDragonPuff& P);

	/** A light that flashes and dies away, colour moving from C0 to C1. */
	void Flash(const FVector& At, float Candelas, float RadiusCm, float Life, const FLinearColor& C0, const FLinearColor& C1);

	/** A burn mark on the ground, fading over a minute. */
	void Scorch(const FVector& At, const FVector& Normal, float RadiusCm);

	/** The plasma blast: flash, fireball, plasma burst, fire, smoke, embers; spray and steam on water. */
	void PlasmaBlast(const FVector& At, const FVector& Normal, bool bWater, float RadiusCm);

	/** Glowing trail behind a bolt between two points (called each frame by the bolt). */
	void PlasmaTrail(const FVector& From, const FVector& To, const FVector& Vel);

	/** A lightning stroke from the cloud base down to the ground or sea at To. */
	void LightningBolt(const FVector& Cloud, const FVector& To, float WidthCm);

	/** Muzzle flash at the dragon's mouth. */
	void PlasmaMuzzle(const FVector& At, const FVector& Dir);

	UMaterialInterface* GetCoreMaterial() const { return CoreMat; }

	/** Max live sprites (oldest smoke goes first). */
	UPROPERTY(EditAnywhere, Category = "FX") int32 MaxPuffs = 700;

private:
	struct FLive
	{
		FDragonPuff P;
		int32 Slot = -1;
	};
	struct FFlash
	{
		TObjectPtr<UPointLightComponent> Light;
		float Age = 0.f, Life = 1.f, Peak = 0.f;
		FLinearColor C0, C1;
	};
	struct FMark
	{
		TObjectPtr<UDecalComponent> Decal;
		TObjectPtr<UMaterialInstanceDynamic> MID;
		float Age = 0.f;
	};

	int32 Acquire(EDragonPuff Kind);
	void Release(int32 Slot);

	TArray<FLive> Live;
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> Slots;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> SlotMIDs;
	TArray<uint8> SlotKind;
	TArray<int32> Free;
	TArray<FFlash> Flashes;
	TArray<FMark> Marks;

	UPROPERTY(Transient) TObjectPtr<UStaticMesh> Plane;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> GlowMat;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> SmokeMat;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> CoreMat;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> ScorchMat;
	UPROPERTY(Transient) TArray<TObjectPtr<UPointLightComponent>> LightPool;
	UPROPERTY(Transient) TArray<TObjectPtr<UDecalComponent>> DecalKeep;

	uint32 Seed = 12345;
	float Rand() { Seed = Seed * 1664525u + 1013904223u; return ((Seed >> 8) & 0xFFFF) / 65535.f; }
	FVector RandDir() { const float Z = Rand() * 2.f - 1.f, A = Rand() * 2.f * PI, R = FMath::Sqrt(1.f - Z * Z); return FVector(R * FMath::Cos(A), R * FMath::Sin(A), Z); }
};

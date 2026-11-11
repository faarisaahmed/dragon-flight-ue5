#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DragonPlasma.generated.h"

class UStaticMeshComponent;
class UPointLightComponent;
class UNiagaraSystem;

/**
 * A plasma blast (plasma.js): 600 m/s plus the dragon's own speed along the
 * shot, 1800 m range, swept in <= 6 m sub-steps so nothing is tunnelled, and a
 * 26 m blast where it lands.
 *
 * The look is an electric arc's: a white-hot heart inside a violet-blue sheath that
 * stretches with speed, a glowing wake, a flickering light; where it lands, a flash,
 * a plasma burst, fire, smoke, embers and a scorch mark (or spray and steam on water),
 * with a shriek as it leaves and a boom where it lands (ADragonFX, DragonSfx).
 */
UCLASS()
class DRAGONFLIGHT_API ADragonPlasma : public AActor
{
	GENERATED_BODY()

public:
	ADragonPlasma();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	void Launch(const FVector& VelocityCmPerS);

	UPROPERTY(VisibleAnywhere, Category = "Plasma") TObjectPtr<UStaticMeshComponent> Core;
	UPROPERTY(VisibleAnywhere, Category = "Plasma") TObjectPtr<UPointLightComponent> Glow;
	/** The sheath around the core, stretched along the flight. */
	UPROPERTY(VisibleAnywhere, Category = "Plasma") TObjectPtr<UStaticMeshComponent> Sheath;

	UPROPERTY(EditAnywhere, Category = "Plasma") float RangeM = 1800.f;
	UPROPERTY(EditAnywhere, Category = "Plasma") float BlastRadiusM = 26.f;
	UPROPERTY(EditAnywhere, Category = "Plasma") FLinearColor Colour = FLinearColor(0.45f, 0.55f, 1.0f);
	UPROPERTY(EditAnywhere, Category = "Plasma") TObjectPtr<UNiagaraSystem> BlastFX;

private:
	void Explode(const FVector& AtCm, const FVector& Normal = FVector::UpVector, bool bWater = false);
	UPROPERTY(Transient) TObjectPtr<class UMaterialInstanceDynamic> CoreMID;
	UPROPERTY(Transient) TObjectPtr<class UMaterialInstanceDynamic> SheathMID;
	float Age = 0.f;

	FVector Velocity = FVector::ZeroVector;
	float Travelled = 0.f;
	float SkimT = 0.f;
	/** The water surface under a point (cm): a lake's, or the sea's 0. */
	float WaterAt(const FVector& Cm) const;
	bool bExploded = false;
	float FlashT = 0.f;
};

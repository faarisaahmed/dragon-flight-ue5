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
 * 26 m blast where it lands. Self-contained: an emissive core and a light; a
 * Niagara burst is used if one exists at /Game/World/FX/NS_PlasmaBlast.
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

	UPROPERTY(EditAnywhere, Category = "Plasma") float RangeM = 1800.f;
	UPROPERTY(EditAnywhere, Category = "Plasma") float BlastRadiusM = 26.f;
	UPROPERTY(EditAnywhere, Category = "Plasma") FLinearColor Colour = FLinearColor(0.45f, 0.55f, 1.0f);
	UPROPERTY(EditAnywhere, Category = "Plasma") TObjectPtr<UNiagaraSystem> BlastFX;

private:
	void Explode(const FVector& AtCm);

	FVector Velocity = FVector::ZeroVector;
	float Travelled = 0.f;
	bool bExploded = false;
	float FlashT = 0.f;
};

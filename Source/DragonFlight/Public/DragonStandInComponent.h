#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "DragonFlightModel.h"
#include "DragonStandInComponent.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;

/**
 * A life-size placeholder dragon built from engine primitives — 8.6 m nose to
 * tail, 15.1 m across the wings — animated procedurally from the flight state
 * the same way the web game's wings.js / flightrig.js / gait.js did it: a slow
 * heavy wingbeat with flap-glide at cruise, wings that fold on the ground, a
 * tail that rudders into turns, a head that leans into them, and a walk cycle.
 *
 * It exists so the game is playable before a real dragon is imported; once
 * ADragonPawn::DragonMesh has a skeletal mesh this hides itself.
 */
UCLASS(ClassGroup = (Dragon), meta = (BlueprintSpawnableComponent))
class DRAGONFLIGHT_API UDragonStandInComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UDragonStandInComponent();

	virtual void BeginPlay() override;

	/** Returns the body heave in metres (added to the mesh, never the collision). */
	float Animate(float Dt, const FDragonFlightState& FS, bool bGrounded, float WalkSpeed, bool bFlaring);

	void SetStandInVisible(bool bVisible);
	bool IsStandInVisible() const { return bShown; }

	UPROPERTY(EditAnywhere, Category = "StandIn") FLinearColor Hide = FLinearColor(0.018f, 0.019f, 0.024f);
	UPROPERTY(EditAnywhere, Category = "StandIn") FLinearColor Membrane = FLinearColor(0.03f, 0.028f, 0.03f);
	UPROPERTY(EditAnywhere, Category = "StandIn") FLinearColor Eye = FLinearColor(0.9f, 0.55f, 0.08f);

private:
	void Build();
	UStaticMeshComponent* Part(const TCHAR* Name, UStaticMesh* Mesh, USceneComponent* Parent, const FVector& LocM, const FVector& SizeM, const FRotator& Rot = FRotator::ZeroRotator, UMaterialInstanceDynamic* Mat = nullptr);
	USceneComponent* Joint(const TCHAR* Name, USceneComponent* Parent, const FVector& LocM);

	bool bBuilt = false;
	bool bShown = true;

	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> Parts;
	UPROPERTY(Transient) TObjectPtr<USceneComponent> WingL;
	UPROPERTY(Transient) TObjectPtr<USceneComponent> WingR;
	UPROPERTY(Transient) TObjectPtr<USceneComponent> ElbowL;
	UPROPERTY(Transient) TObjectPtr<USceneComponent> ElbowR;
	UPROPERTY(Transient) TObjectPtr<USceneComponent> NeckRoot;
	UPROPERTY(Transient) TObjectPtr<USceneComponent> HeadJoint;
	UPROPERTY(Transient) TArray<TObjectPtr<USceneComponent>> Tail;
	UPROPERTY(Transient) TArray<TObjectPtr<USceneComponent>> Legs; // FL, FR, BL, BR
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> HideMat;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> MembraneMat;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> EyeMat;

	// Animation state
	float BeatPhase = 0.f;
	float Beats = 0.f;
	float GlideLeft = 0.f;
	float GlideK = 0.f;
	float NextGlideAfter = 4.f;
	float FoldT = 0.f;
	float GaitPhase = 0.f;
	float TailPhase = 0.f;
	float Turn = 0.f;
};

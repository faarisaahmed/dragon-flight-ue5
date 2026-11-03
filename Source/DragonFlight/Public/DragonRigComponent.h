#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DragonFlightModel.h"
#include "DragonRigComponent.generated.h"

class UPoseableMeshComponent;

/** One procedural joint: a bone, and the local axis each motion turns it about. */
USTRUCT(BlueprintType)
struct FDragonRigJoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) FName Bone;
	/** Axis the main motion (flap / sway / swing) turns about, in the DRAGON'S BODY space: X forward, Y right, Z up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector Axis = FVector(1, 0, 0);
	/** Axis for the secondary motion (fold / sweep / pitch), body space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector Axis2 = FVector(0, 0, 1);
	/** Multiplier on the motion; flip to -1 for the mirrored side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float Sign = 1.f;

	int32 Index = INDEX_NONE;
	FVector LocalAxis = FVector(1, 0, 0);
	FVector LocalAxis2 = FVector(0, 0, 1);
};

/**
 * Animates a real skeletal dragon procedurally from the flight state — the same
 * job wings.js / flightrig.js / gait.js did in the web game, and the same
 * motions UDragonStandInComponent shows on the primitive stand-in.
 *
 * Bone names and axes are data, so any dragon can be wired up in the Details
 * panel. The defaults match the rig Tools/DragonModel/build_dragon.py makes.
 * Axes are given in the dragon's body frame (X forward, Y right, Z up) and
 * converted into each bone's own frame from the reference pose, so they do not
 * depend on how the modelling package oriented its bones. Rotations are applied
 * on top of the reference pose.
 */
UCLASS(ClassGroup = (Dragon), meta = (BlueprintSpawnableComponent))
class DRAGONFLIGHT_API UDragonRigComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDragonRigComponent();

	void Bind(UPoseableMeshComponent* InMesh);
	bool IsBound() const { return Mesh != nullptr && RefPose.Num() > 0; }
	float Animate(float Dt, const FDragonFlightState& FS, bool bGrounded, float WalkSpeed, bool bFlaring);

	UPROPERTY(EditAnywhere, Category = "Rig|Wings") FDragonRigJoint ShoulderL;
	UPROPERTY(EditAnywhere, Category = "Rig|Wings") FDragonRigJoint ShoulderR;
	UPROPERTY(EditAnywhere, Category = "Rig|Wings") FDragonRigJoint ElbowL;
	UPROPERTY(EditAnywhere, Category = "Rig|Wings") FDragonRigJoint ElbowR;
	UPROPERTY(EditAnywhere, Category = "Rig|Wings") FDragonRigJoint WristL;
	UPROPERTY(EditAnywhere, Category = "Rig|Wings") FDragonRigJoint WristR;
	UPROPERTY(EditAnywhere, Category = "Rig|Body") TArray<FDragonRigJoint> Neck;
	UPROPERTY(EditAnywhere, Category = "Rig|Body") FDragonRigJoint Head;
	UPROPERTY(EditAnywhere, Category = "Rig|Body") TArray<FDragonRigJoint> TailChain;
	/** Front left, front right, back left, back right. */
	UPROPERTY(EditAnywhere, Category = "Rig|Legs") TArray<FDragonRigJoint> Legs;

private:
	void Resolve(FDragonRigJoint& J) const;
	void Rotate(const FDragonRigJoint& J, float Deg1, float Deg2 = 0.f);

	UPROPERTY(Transient) TObjectPtr<UPoseableMeshComponent> Mesh;
	TArray<FTransform> RefPose;
	TArray<FTransform> RefCS;
	FQuat BodyToComponent = FQuat::Identity;

	float BeatPhase = 0.f, Beats = 0.f, GlideLeft = 0.f, GlideK = 0.f, NextGlideAfter = 4.f;
	float FoldT = 0.f, GaitPhase = 0.f, TailPhase = 0.f, Turn = 0.f;
};

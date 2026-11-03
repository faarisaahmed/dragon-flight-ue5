#include "DragonRigComponent.h"
#include "DragonFlight.h"
#include "Components/PoseableMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "ReferenceSkeleton.h"

namespace
{
	float Smooth(float X, float A, float B)
	{
		const float T = FMath::Clamp((X - A) / (B - A), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}

	FDragonRigJoint J(const TCHAR* Bone, FVector Axis, FVector Axis2, float Sign = 1.f)
	{
		FDragonRigJoint R;
		R.Bone = Bone; R.Axis = Axis; R.Axis2 = Axis2; R.Sign = Sign;
		return R;
	}
}

UDragonRigComponent::UDragonRigComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	// Defaults: bone names from build_dragon.py; axes in body space
	// (X forward, Y right, Z up): wings flap about X and sweep about Z, the
	// tail and neck sway about Z and pitch about Y, legs swing about Y.
	ShoulderL = J(TEXT("wing_shoulder_l"), FVector(1, 0, 0), FVector(0, 0, 1), 1.f);
	ShoulderR = J(TEXT("wing_shoulder_r"), FVector(1, 0, 0), FVector(0, 0, 1), -1.f);
	ElbowL = J(TEXT("wing_elbow_l"), FVector(1, 0, 0), FVector(0, 0, 1), 1.f);
	ElbowR = J(TEXT("wing_elbow_r"), FVector(1, 0, 0), FVector(0, 0, 1), -1.f);
	WristL = J(TEXT("wing_wrist_l"), FVector(1, 0, 0), FVector(0, 0, 1), 1.f);
	WristR = J(TEXT("wing_wrist_r"), FVector(1, 0, 0), FVector(0, 0, 1), -1.f);
	for (int32 i = 1; i <= 4; ++i) Neck.Add(J(*FString::Printf(TEXT("neck_%02d"), i), FVector(0, 0, 1), FVector(0, 1, 0)));
	Head = J(TEXT("head"), FVector(0, 0, 1), FVector(0, 1, 0));
	for (int32 i = 1; i <= 12; ++i) TailChain.Add(J(*FString::Printf(TEXT("tail_%02d"), i), FVector(0, 0, 1), FVector(0, 1, 0)));
	Legs.Add(J(TEXT("upperarm_l"), FVector(0, 1, 0), FVector(1, 0, 0)));
	Legs.Add(J(TEXT("upperarm_r"), FVector(0, 1, 0), FVector(1, 0, 0)));
	Legs.Add(J(TEXT("thigh_l"), FVector(0, 1, 0), FVector(1, 0, 0)));
	Legs.Add(J(TEXT("thigh_r"), FVector(0, 1, 0), FVector(1, 0, 0)));
}

void UDragonRigComponent::Resolve(FDragonRigJoint& Jt) const
{
	Jt.Index = Mesh ? Mesh->GetBoneIndex(Jt.Bone) : INDEX_NONE;
	if (Jt.Index != INDEX_NONE && RefCS.IsValidIndex(Jt.Index))
	{
		const FQuat BoneCS = RefCS[Jt.Index].GetRotation();
		Jt.LocalAxis = BoneCS.UnrotateVector(BodyToComponent.RotateVector(Jt.Axis.GetSafeNormal()));
		Jt.LocalAxis2 = BoneCS.UnrotateVector(BodyToComponent.RotateVector(Jt.Axis2.GetSafeNormal()));
	}
	if (Jt.Index == INDEX_NONE && !Jt.Bone.IsNone())
	{
		UE_LOG(LogDragon, Verbose, TEXT("Rig: bone %s not found"), *Jt.Bone.ToString());
	}
}

void UDragonRigComponent::Bind(UPoseableMeshComponent* InMesh)
{
	Mesh = InMesh;
	RefPose.Reset();
	if (!Mesh || !Mesh->GetSkinnedAsset()) return;
	const FReferenceSkeleton& Ref = Mesh->GetSkinnedAsset()->GetRefSkeleton();
	RefPose = Ref.GetRefBonePose();
	// Reference pose in component space, to turn body-space axes into bone-local ones.
	RefCS.SetNum(RefPose.Num());
	for (int32 i = 0; i < RefPose.Num(); ++i)
	{
		const int32 Parent = Ref.GetParentIndex(i);
		RefCS[i] = Parent == INDEX_NONE ? RefPose[i] : RefPose[i] * RefCS[Parent];
	}
	// Body frame -> mesh component frame (the mesh may be yawed to face +X).
	BodyToComponent = Mesh->GetRelativeRotation().Quaternion().Inverse();
	for (FDragonRigJoint* Jt : { &ShoulderL, &ShoulderR, &ElbowL, &ElbowR, &WristL, &WristR, &Head }) Resolve(*Jt);
	for (FDragonRigJoint& Jt : Neck) Resolve(Jt);
	for (FDragonRigJoint& Jt : TailChain) Resolve(Jt);
	for (FDragonRigJoint& Jt : Legs) Resolve(Jt);
}

void UDragonRigComponent::Rotate(const FDragonRigJoint& Jt, float Deg1, float Deg2)
{
	// VERIFY(5.5): BoneSpaceTransforms is believed public (GENERATED_UCLASS_BODY) and MarkRefreshTransformDirty exported;
	// if not, use Mesh->SetBoneRotationByName(Bone, Rot, EBoneSpaces::ComponentSpace) with RefCS-derived rotations.
	if (Jt.Index == INDEX_NONE || !RefPose.IsValidIndex(Jt.Index) || !Mesh->BoneSpaceTransforms.IsValidIndex(Jt.Index)) return;
	const FQuat Q1(Jt.LocalAxis, FMath::DegreesToRadians(Deg1 * Jt.Sign));
	const FQuat Q2(Jt.LocalAxis2, FMath::DegreesToRadians(Deg2 * Jt.Sign));
	FTransform T = RefPose[Jt.Index];
	T.SetRotation(T.GetRotation() * Q2 * Q1);
	Mesh->BoneSpaceTransforms[Jt.Index] = T;
}

float UDragonRigComponent::Animate(float Dt, const FDragonFlightState& FS, bool bGrounded, float WalkSpeed, bool bFlaring)
{
	if (!IsBound()) return 0.f;

	// Same motion model as the stand-in (wings.js / flightrig.js / gait.js).
	FoldT = FMath::FInterpTo(FoldT, (bGrounded && !bFlaring) ? 1.f : 0.f, Dt, bGrounded ? 2.2f : 4.f);
	const float SlowK = 1.f - Smooth(FS.SpeedT, 0.04f, 0.32f);
	const float FastK = Smooth(FS.SpeedT, 0.4f, 0.85f);
	const float ClimbK = FMath::Max(0.f, FS.Climb);
	const bool bCanGlide = SlowK < 0.25f && FS.Climb < 0.15f && FMath::Abs(FS.Roll) < 0.1f && FS.Hang < 0.1f && !bGrounded;
	if (GlideLeft > 0.f) GlideLeft -= Dt;
	else if (bCanGlide && Beats >= NextGlideAfter)
	{
		GlideLeft = 1.2f + FMath::FRand() * 1.6f + 1.2f * FastK;
		Beats = 0.f;
		NextGlideAfter = 3.f + FMath::FRand() * 2.f;
	}
	if (!bCanGlide) GlideLeft = 0.f;
	GlideK = FMath::FInterpTo(GlideK, GlideLeft > 0.f ? 1.f : 0.f, Dt, 3.f);

	float Freq = FMath::Lerp(FMath::Lerp(0.72f, 0.6f, FastK), 0.95f, SlowK) + 0.22f * ClimbK;
	Freq *= (1.f - 0.85f * FS.Hang) * (1.f - 0.7f * FMath::Abs(FS.Roll)) * (1.f - 0.75f * GlideK);
	Freq = FMath::Max(Freq, 0.12f);
	const float Prev = BeatPhase;
	BeatPhase += Freq * Dt * 2.f * PI;
	if (FMath::FloorToInt(BeatPhase / (2.f * PI)) != FMath::FloorToInt(Prev / (2.f * PI))) Beats += 1.f;

	const float Amp = (FMath::Lerp(0.42f, 0.85f, SlowK) + 0.3f * ClimbK) * (1.f - GlideK * 0.92f) * (1.f - FoldT);
	const float Wave = FMath::Sin(BeatPhase);
	const float Lag1 = FMath::Sin(BeatPhase - 0.9f);
	const float Lag2 = FMath::Sin(BeatPhase - 1.6f);
	const float Dihedral = FMath::Lerp(6.f, 2.f, FastK) - 10.f * FS.Hang;
	const float Sweep = FMath::Lerp(-8.f, 22.f, FastK) + 10.f * FMath::Abs(FS.Knife);

	const float Sh = FMath::RadiansToDegrees(Wave * Amp) + Dihedral + 40.f * FoldT;
	const float El = FMath::RadiansToDegrees(Lag1 * Amp * 0.6f);
	const float Wr = FMath::RadiansToDegrees(Lag2 * Amp * 0.4f);
	Rotate(ShoulderL, Sh + 12.f * FS.Knife, Sweep + 78.f * FoldT);
	Rotate(ShoulderR, Sh - 12.f * FS.Knife, Sweep + 78.f * FoldT);
	Rotate(ElbowL, El, 150.f * FoldT);
	Rotate(ElbowR, El, 150.f * FoldT);
	Rotate(WristL, Wr, 120.f * FoldT);
	Rotate(WristR, Wr, 120.f * FoldT);

	Turn = FMath::FInterpTo(Turn, FS.Turn, Dt, 3.f);
	TailPhase += Dt * (bGrounded ? 1.4f + FMath::Abs(WalkSpeed) * 0.3f : 0.9f);
	for (int32 i = 0; i < TailChain.Num(); ++i)
	{
		const float T = (float)i / FMath::Max(1, TailChain.Num() - 1);
		const float Sway = FMath::Sin(TailPhase - T * 2.2f) * (bGrounded ? 4.f : 2.f) + Turn * 5.f;
		Rotate(TailChain[i], Sway, -FS.Climb * 2.5f + (bGrounded ? 1.2f : 0.f));
	}
	for (int32 i = 0; i < Neck.Num(); ++i)
	{
		Rotate(Neck[i], -Turn * 14.f / Neck.Num(), ((bGrounded ? 8.f : -6.f) + FS.Climb * 8.f) / Neck.Num());
	}
	Rotate(Head, -Turn * 10.f, bGrounded ? -12.f : 4.f - FS.Climb * 6.f);

	const float V = FMath::Abs(WalkSpeed);
	const float StrideHz = V < 3.f ? 0.75f + 0.42f * V : V < 9.f ? 1.55f + 0.16f * V : 2.3f + 0.07f * V;
	if (bGrounded) GaitPhase += Dt * StrideHz * 2.f * PI;
	const float Swing = bGrounded ? FMath::Clamp(V / 6.f, 0.f, 1.f) * 28.f : 0.f;
	static const float Offsets[4] = { 0.f, PI, PI, 0.f };
	for (int32 i = 0; i < Legs.Num() && i < 4; ++i)
	{
		const float Tuck = bGrounded ? 0.f : (i < 2 ? 55.f : 70.f);
		Rotate(Legs[i], -FMath::Sin(GaitPhase + Offsets[i]) * Swing - Tuck);
	}

	Mesh->MarkRefreshTransformDirty();
	return bGrounded ? 0.f : -Wave * FMath::Lerp(0.07f, 0.24f, SlowK) * (Amp / 0.6f);
}

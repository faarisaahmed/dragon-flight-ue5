#include "DragonStandInComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	constexpr float M = 100.f;
	float Smooth(float X, float A, float B)
	{
		const float T = FMath::Clamp((X - A) / (B - A), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}
}

UDragonStandInComponent::UDragonStandInComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UDragonStandInComponent::BeginPlay()
{
	Super::BeginPlay();
	if (!bBuilt) Build();
}

USceneComponent* UDragonStandInComponent::Joint(const TCHAR* Name, USceneComponent* Parent, const FVector& LocM)
{
	UObject* Outer = GetOwner() ? (UObject*)GetOwner() : (UObject*)this;
	USceneComponent* J = NewObject<USceneComponent>(Outer, MakeUniqueObjectName(Outer, USceneComponent::StaticClass(), FName(Name)), RF_Transient);
	J->SetupAttachment(Parent);
	J->SetRelativeLocation(LocM * M);
	J->RegisterComponent();
	return J;
}

UStaticMeshComponent* UDragonStandInComponent::Part(const TCHAR* Name, UStaticMesh* Mesh, USceneComponent* Parent, const FVector& LocM, const FVector& SizeM, const FRotator& Rot, UMaterialInstanceDynamic* Mat)
{
	UObject* Outer = GetOwner() ? (UObject*)GetOwner() : (UObject*)this;
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Outer, MakeUniqueObjectName(Outer, UStaticMeshComponent::StaticClass(), FName(Name)), RF_Transient);
	C->SetStaticMesh(Mesh);
	C->SetupAttachment(Parent);
	C->SetRelativeLocation(LocM * M);
	C->SetRelativeRotation(Rot);
	C->SetRelativeScale3D(SizeM); // engine basic shapes are 1 m across
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetGenerateOverlapEvents(false);
	C->bCastDynamicShadow = true;
	C->SetMaterial(0, Mat ? Mat : HideMat.Get());
	C->RegisterComponent();
	Parts.Add(C);
	return C;
}

void UDragonStandInComponent::Build()
{
	if (!GetOwner()) return;
	bBuilt = true;

	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cyl = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UStaticMesh* Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Sphere || !Cube || !Cyl || !Cone) return;

	HideMat = UMaterialInstanceDynamic::Create(Base, this);
	MembraneMat = UMaterialInstanceDynamic::Create(Base, this);
	EyeMat = UMaterialInstanceDynamic::Create(Base, this);
	if (HideMat) HideMat->SetVectorParameterValue(TEXT("Color"), Hide);
	if (MembraneMat) MembraneMat->SetVectorParameterValue(TEXT("Color"), Membrane);
	if (EyeMat) EyeMat->SetVectorParameterValue(TEXT("Color"), Eye);

	// Mesh space: +X forward, +Z up, feet on Z = 0. Sizes in metres.
	Part(TEXT("Body"), Sphere, this, FVector(-0.2f, 0, 1.45f), FVector(2.9f, 1.25f, 1.15f));
	Part(TEXT("Chest"), Sphere, this, FVector(0.9f, 0, 1.6f), FVector(1.7f, 1.2f, 1.2f));
	Part(TEXT("Haunch"), Sphere, this, FVector(-1.2f, 0, 1.35f), FVector(1.5f, 1.25f, 1.05f));

	NeckRoot = Joint(TEXT("NeckRoot"), this, FVector(1.6f, 0, 1.85f));
	Part(TEXT("Neck1"), Sphere, NeckRoot, FVector(0.35f, 0, 0.15f), FVector(1.0f, 0.75f, 0.75f));
	Part(TEXT("Neck2"), Sphere, NeckRoot, FVector(0.85f, 0, 0.35f), FVector(0.9f, 0.6f, 0.62f));
	HeadJoint = Joint(TEXT("Head"), NeckRoot, FVector(1.35f, 0, 0.5f));
	Part(TEXT("Skull"), Sphere, HeadJoint, FVector(0.3f, 0, 0.05f), FVector(0.95f, 0.7f, 0.55f));
	Part(TEXT("Snout"), Sphere, HeadJoint, FVector(0.75f, 0, -0.02f), FVector(0.7f, 0.48f, 0.36f));
	Part(TEXT("Jaw"), Sphere, HeadJoint, FVector(0.55f, 0, -0.17f), FVector(0.85f, 0.45f, 0.2f));
	Part(TEXT("EyeL"), Sphere, HeadJoint, FVector(0.52f, -0.27f, 0.14f), FVector(0.11f), FRotator::ZeroRotator, EyeMat);
	Part(TEXT("EyeR"), Sphere, HeadJoint, FVector(0.52f, 0.27f, 0.14f), FVector(0.11f), FRotator::ZeroRotator, EyeMat);
	Part(TEXT("HornL"), Cone, HeadJoint, FVector(0.05f, -0.22f, 0.32f), FVector(0.12f, 0.12f, 0.45f), FRotator(-60.f, 0, -15.f));
	Part(TEXT("HornR"), Cone, HeadJoint, FVector(0.05f, 0.22f, 0.32f), FVector(0.12f, 0.12f, 0.45f), FRotator(-60.f, 0, 15.f));

	// Tail: a chain of shrinking segments, each a joint so it can curl.
	USceneComponent* Prev = this;
	FVector At(-1.9f, 0, 1.35f);
	for (int32 i = 0; i < 9; ++i)
	{
		const float T = i / 8.f;
		USceneComponent* J = Joint(*FString::Printf(TEXT("Tail%d"), i), Prev, Prev == this ? At : FVector(-0.48f, 0, -0.03f));
		const float W = FMath::Lerp(0.75f, 0.14f, T);
		Part(*FString::Printf(TEXT("TailSeg%d"), i), Sphere, J, FVector(-0.24f, 0, 0), FVector(0.7f, W, W * 0.9f));
		Tail.Add(J);
		Prev = J;
	}
	Part(TEXT("TailFinL"), Cube, Prev, FVector(-0.35f, -0.3f, 0), FVector(0.55f, 0.5f, 0.03f), FRotator(0, 25.f, 0), MembraneMat);
	Part(TEXT("TailFinR"), Cube, Prev, FVector(-0.35f, 0.3f, 0), FVector(0.55f, 0.5f, 0.03f), FRotator(0, -25.f, 0), MembraneMat);

	// Legs: hip joints, the leg hanging down from each.
	const FVector Hips[4] = { FVector(0.95f, -0.42f, 1.15f), FVector(0.95f, 0.42f, 1.15f), FVector(-1.1f, -0.45f, 1.1f), FVector(-1.1f, 0.45f, 1.1f) };
	for (int32 i = 0; i < 4; ++i)
	{
		USceneComponent* J = Joint(*FString::Printf(TEXT("Hip%d"), i), this, Hips[i]);
		Part(*FString::Printf(TEXT("Thigh%d"), i), Cyl, J, FVector(0, 0, -0.35f), FVector(0.32f, 0.32f, 0.75f));
		Part(*FString::Printf(TEXT("Shin%d"), i), Cyl, J, FVector(0.05f, 0, -0.85f), FVector(0.2f, 0.2f, 0.6f));
		Part(*FString::Printf(TEXT("Foot%d"), i), Sphere, J, FVector(0.15f, 0, -1.08f), FVector(0.42f, 0.28f, 0.14f));
		Legs.Add(J);
	}

	// Wings: shoulder -> inner panel and arm, elbow -> outer panel and finger.
	auto BuildWing = [&](float Side, TObjectPtr<USceneComponent>& Shoulder, TObjectPtr<USceneComponent>& Elbow)
	{
		const TCHAR* S = Side < 0 ? TEXT("L") : TEXT("R");
		Shoulder = Joint(*FString::Printf(TEXT("Shoulder%s"), S), this, FVector(0.75f, 0.45f * Side, 2.0f));
		Part(*FString::Printf(TEXT("Arm%s"), S), Cyl, Shoulder, FVector(0.1f, 1.6f * Side, 0.05f), FVector(0.16f, 0.16f, 3.2f), FRotator(0, 0, 90.f));
		Part(*FString::Printf(TEXT("Inner%s"), S), Cube, Shoulder, FVector(-0.75f, 1.6f * Side, 0), FVector(1.7f, 3.2f, 0.03f), FRotator::ZeroRotator, MembraneMat);
		Elbow = Joint(*FString::Printf(TEXT("Elbow%s"), S), Shoulder, FVector(0.1f, 3.2f * Side, 0.05f));
		Part(*FString::Printf(TEXT("Finger%s"), S), Cyl, Elbow, FVector(-0.1f, 2.05f * Side, 0), FVector(0.1f, 0.1f, 4.1f), FRotator(0, 0, 90.f));
		Part(*FString::Printf(TEXT("Outer%s"), S), Cube, Elbow, FVector(-0.75f, 1.9f * Side, 0), FVector(1.35f, 3.9f, 0.025f), FRotator(0, 8.f * Side, 0), MembraneMat);
		Part(*FString::Printf(TEXT("Tip%s"), S), Cube, Elbow, FVector(-0.45f, 3.95f * Side, 0), FVector(0.6f, 0.5f, 0.025f), FRotator(0, 25.f * Side, 0), MembraneMat);
	};
	BuildWing(-1.f, WingL, ElbowL);
	BuildWing(1.f, WingR, ElbowR);
}

void UDragonStandInComponent::SetStandInVisible(bool bVisible)
{
	bShown = bVisible;
	SetVisibility(bVisible, true);
}

float UDragonStandInComponent::Animate(float Dt, const FDragonFlightState& FS, bool bGrounded, float WalkSpeed, bool bFlaring)
{
	if (!bBuilt || !WingL || !WingR) return 0.f;

	// --- Fold (ground) ----------------------------------------------------------
	FoldT = FMath::FInterpTo(FoldT, (bGrounded && !bFlaring) ? 1.f : 0.f, Dt, bGrounded ? 2.2f : 4.f);

	// --- Wingbeat (wings.js) ------------------------------------------------------
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
	const float PrevPhase = BeatPhase;
	BeatPhase += Freq * Dt * 2.f * PI;
	if (FMath::FloorToInt(BeatPhase / (2.f * PI)) != FMath::FloorToInt(PrevPhase / (2.f * PI))) Beats += 1.f;

	const float Amp = (FMath::Lerp(0.42f, 0.85f, SlowK) + 0.3f * ClimbK) * (1.f - GlideK * 0.92f) * (1.f - FoldT);
	const float Wave = FMath::Sin(BeatPhase);
	const float Elbow = FMath::Sin(BeatPhase - 0.9f); // the hand lags the arm

	// Degrees. Shoulder roll lifts the wing; elbow continues the stroke.
	const float Dihedral = FMath::Lerp(6.f, 2.f, FastK) - 10.f * FS.Hang;
	const float ShoulderRoll = FMath::RadiansToDegrees(Wave * Amp) + Dihedral;
	const float ElbowRoll = FMath::RadiansToDegrees(Elbow * Amp * 0.6f);
	// Sweep back when fast (a thrown spear), forward and cupped when slow.
	const float Sweep = FMath::Lerp(-8.f, 22.f, FastK) + 10.f * FMath::Abs(FS.Knife);
	// Folded on the ground: arms swept back along the flanks and raised.
	const float FoldSweep = 78.f * FoldT, FoldElbow = 150.f * FoldT, FoldRaise = 40.f * FoldT;
	// Knife edge / bank: the low wing tucks a little.
	const float KnifeTuck = 12.f * FS.Knife;

	// UE: roll about X raises/lowers a wing; yaw about Z sweeps it.
	WingL->SetRelativeRotation(FRotator(0.f, -(Sweep + FoldSweep), -(ShoulderRoll + FoldRaise) + KnifeTuck));
	WingR->SetRelativeRotation(FRotator(0.f, (Sweep + FoldSweep), (ShoulderRoll + FoldRaise) + KnifeTuck));
	ElbowL->SetRelativeRotation(FRotator(0.f, -FoldElbow, -ElbowRoll));
	ElbowR->SetRelativeRotation(FRotator(0.f, FoldElbow, ElbowRoll));

	// --- Body heave (rises on the downstroke) --------------------------------
	const float Heave = bGrounded ? 0.f : -Wave * FMath::Lerp(0.07f, 0.24f, SlowK) * (Amp / 0.6f);

	// --- Tail and neck (flightrig.js) ------------------------------------------
	Turn = FMath::FInterpTo(Turn, FS.Turn, Dt, 3.f);
	TailPhase += Dt * (bGrounded ? 1.4f + FMath::Abs(WalkSpeed) * 0.3f : 0.9f);
	for (int32 i = 0; i < Tail.Num(); ++i)
	{
		const float T = (float)i / FMath::Max(1, Tail.Num() - 1);
		const float Sway = FMath::Sin(TailPhase - T * 2.2f) * (bGrounded ? 5.f : 2.5f);
		const float Rudder = Turn * 7.f; // tail swings out of the turn, a rudder
		const float Elevator = -FS.Climb * 3.f + (bGrounded ? 1.5f : 0.f);
		Tail[i]->SetRelativeRotation(FRotator(Elevator, Sway + Rudder, 0.f));
	}
	if (NeckRoot) NeckRoot->SetRelativeRotation(FRotator(bGrounded ? 8.f : -6.f + FS.Climb * 8.f, -Turn * 14.f, 0.f));
	if (HeadJoint) HeadJoint->SetRelativeRotation(FRotator(bGrounded ? -12.f : 4.f - FS.Climb * 6.f, -Turn * 10.f, -Turn * 10.f));

	// --- Legs (gait.js, simplified) -------------------------------------------
	const float V = FMath::Abs(WalkSpeed);
	const float StrideHz = V < 3.f ? 0.75f + 0.42f * V : V < 9.f ? 1.55f + 0.16f * V : 2.3f + 0.07f * V;
	GaitPhase += Dt * StrideHz * 2.f * PI * (bGrounded ? 1.f : 0.f);
	const float Swing = bGrounded ? FMath::Clamp(V / 6.f, 0.f, 1.f) * 28.f : 0.f;
	// Walk pattern: FL, BR together; FR, BL together. Gallop bunches the pairs.
	const float Offsets[4] = { 0.f, PI, PI, 0.f };
	for (int32 i = 0; i < Legs.Num(); ++i)
	{
		const float Tuck = bGrounded ? 0.f : (i < 2 ? 55.f : 70.f); // legs tucked in flight
		const float P = -(FMath::Sin(GaitPhase + Offsets[i]) * Swing) - Tuck * (1.f - FoldT * 0.f);
		Legs[i]->SetRelativeRotation(FRotator(P, 0.f, 0.f));
	}
	return Heave;
}

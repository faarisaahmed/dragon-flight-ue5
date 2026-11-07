#include "DragonPlasma.h"
#include "DragonFX.h"
#include "DragonSfx.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/OverlapResult.h"
#include "UObject/ConstructorHelpers.h"

namespace { constexpr float M = 100.f; }

ADragonPlasma::ADragonPlasma()
{
	PrimaryActorTick.bCanEverTick = true;
	InitialLifeSpan = 8.f;

	Core = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Core"));
	RootComponent = Core;
	Core->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Core->SetCastShadow(false);
	Core->SetRelativeScale3D(FVector(1.7f, 1.5f, 1.5f)); // core ~0.8 m across the flight, a little longer along it
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded()) Core->SetStaticMesh(Sphere.Object);

	Sheath = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Sheath"));
	Sheath->SetupAttachment(Core);
	Sheath->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sheath->SetCastShadow(false);
	if (Sphere.Succeeded()) Sheath->SetStaticMesh(Sphere.Object);

	Glow = CreateDefaultSubobject<UPointLightComponent>(TEXT("Glow"));
	Glow->SetupAttachment(Core);
	Glow->SetIntensityUnits(ELightUnits::Candelas);
	Glow->SetIntensity(60000.f);
	Glow->SetAttenuationRadius(7000.f);
	Glow->SetCastShadows(false);
	Glow->SetLightColor(Colour);
}

void ADragonPlasma::BeginPlay()
{
	Super::BeginPlay();
	ADragonFX* FX = ADragonFX::Get(this);
	UMaterialInterface* Mat = FX ? FX->GetCoreMaterial() : nullptr;
	if (!Mat) Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/World/Materials/M_PlasmaCore.M_PlasmaCore"));
	if (Mat)
	{
		CoreMID = UMaterialInstanceDynamic::Create(Mat, this);
		CoreMID->SetScalarParameterValue(TEXT("Intensity"), 10.f);
		CoreMID->SetScalarParameterValue(TEXT("Hot"), 1.f);
		Core->SetMaterial(0, CoreMID);
		SheathMID = UMaterialInstanceDynamic::Create(Mat, this);
		SheathMID->SetScalarParameterValue(TEXT("Intensity"), 3.5f);
		SheathMID->SetScalarParameterValue(TEXT("Hot"), 0.f);
		Sheath->SetMaterial(0, SheathMID);
	}
	Glow->SetLightColor(Colour);
}

void ADragonPlasma::Launch(const FVector& VelocityCmPerS)
{
	Velocity = VelocityCmPerS;
	SetActorRotation(Velocity.Rotation());
	if (ADragonFX* FX = ADragonFX::Get(this)) FX->PlasmaMuzzle(GetActorLocation(), Velocity.GetSafeNormal());
	DragonSfx::PlasmaShot(this, GetActorLocation());
}

void ADragonPlasma::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;
	if (bExploded)
	{
		FlashT += DeltaSeconds;
		Core->SetVisibility(false, true);
		Glow->SetVisibility(false);
		if (FlashT > 0.2f) Destroy();
		return;
	}

	// The sheath stretches along the flight and the whole bolt flickers like an arc.
	const float Flick = 0.82f + 0.18f * FMath::Sin(Age * 61.f) * FMath::Sin(Age * 23.f + 0.7f);
	// (relative to the core's scale) a 4.5 m sheath trailing back from it
	Sheath->SetRelativeScale3D(FVector(1.75f, 1.35f, 1.35f) * (0.95f + 0.1f * Flick));
	Sheath->SetRelativeLocation(FVector(-45.f, 0.f, 0.f));
	Glow->SetIntensity(60000.f * Flick);
	if (CoreMID) CoreMID->SetScalarParameterValue(TEXT("Fade"), Flick);

	const FVector Step = Velocity * DeltaSeconds;
	const float Len = Step.Size();
	const int32 N = FMath::Max(1, FMath::CeilToInt(Len / (6.f * M)));
	FVector At = GetActorLocation();
	const FVector Start = At;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(Plasma), false, this);
	if (AActor* O = GetOwner()) Q.AddIgnoredActor(O);
	for (int32 i = 0; i < N; ++i)
	{
		const FVector Next = At + Step / N;
		FHitResult Hit;
		if (GetWorld()->LineTraceSingleByChannel(Hit, At, Next, ECC_Visibility, Q))
		{
			if (ADragonFX* FX = ADragonFX::Get(this)) FX->PlasmaTrail(Start, Hit.ImpactPoint, Velocity);
			Explode(Hit.ImpactPoint, Hit.ImpactNormal, false);
			return;
		}
		// The sea surface (z = 0) counts even where there is no collision.
		if (Next.Z <= 0.f && At.Z > 0.f)
		{
			const float T = At.Z / (At.Z - Next.Z);
			const FVector W = FMath::Lerp(At, Next, T);
			if (ADragonFX* FX = ADragonFX::Get(this)) FX->PlasmaTrail(Start, W, Velocity);
			Explode(W, FVector::UpVector, true);
			return;
		}
		At = Next;
	}
	SetActorLocation(At);
	if (ADragonFX* FX = ADragonFX::Get(this)) FX->PlasmaTrail(Start, At, Velocity);
	Travelled += Len;
	if (Travelled > RangeM * M) Explode(At, FVector::UpVector, false);
}

void ADragonPlasma::Explode(const FVector& AtCm, const FVector& Normal, bool bWater)
{
	bExploded = true;
	SetActorLocation(AtCm);
	SetLifeSpan(1.f);
	// Spent in the air at the end of its range: a smaller burst with no fire, smoke or scorch.
	const bool bAir = Travelled > RangeM * M;
	if (ADragonFX* FX = ADragonFX::Get(this))
	{
		if (bAir) FX->PlasmaBlast(AtCm, FVector::UpVector, true, BlastRadiusM * M * 0.35f);
		else FX->PlasmaBlast(AtCm, Normal, bWater, BlastRadiusM * M * 0.5f);
	}
	DragonSfx::PlasmaBlast(this, AtCm, bWater || bAir);
	// Anything physics-simulating in the blast gets pushed (crates, debris, later: targets).
	TArray<FOverlapResult> Overlaps;
	FCollisionShape Shape = FCollisionShape::MakeSphere(BlastRadiusM * M);
	if (GetWorld()->OverlapMultiByObjectType(Overlaps, AtCm, FQuat::Identity, FCollisionObjectQueryParams(ECC_PhysicsBody), Shape))
	{
		for (const FOverlapResult& O : Overlaps)
		{
			if (UPrimitiveComponent* P = O.GetComponent())
			{
				if (P->IsSimulatingPhysics()) P->AddRadialImpulse(AtCm, BlastRadiusM * M, 2.0e5f, RIF_Linear, true);
			}
		}
	}
}

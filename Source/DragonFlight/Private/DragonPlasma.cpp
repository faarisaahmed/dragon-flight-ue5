#include "DragonPlasma.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
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
	Core->SetRelativeScale3D(FVector(1.24f)); // core radius 0.62 m
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded()) Core->SetStaticMesh(Sphere.Object);

	Glow = CreateDefaultSubobject<UPointLightComponent>(TEXT("Glow"));
	Glow->SetupAttachment(Core);
	Glow->SetIntensityUnits(ELightUnits::Candelas);
	Glow->SetIntensity(40000.f);
	Glow->SetAttenuationRadius(6000.f);
	Glow->SetCastShadows(false);
	Glow->SetLightColor(Colour);
}

void ADragonPlasma::BeginPlay()
{
	Super::BeginPlay();
	// Emissive core: the project's plasma material if the builder made one, else a bright basic shape.
	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/World/Materials/M_Plasma.M_Plasma"));
	if (Mat) Core->SetMaterial(0, Mat);
	else if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
	{
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
		MID->SetVectorParameterValue(TEXT("Color"), Colour * 20.f);
		Core->SetMaterial(0, MID);
	}
	if (!BlastFX) BlastFX = LoadObject<UNiagaraSystem>(nullptr, TEXT("/Game/World/FX/NS_PlasmaBlast.NS_PlasmaBlast"));
	Glow->SetLightColor(Colour);
}

void ADragonPlasma::Launch(const FVector& VelocityCmPerS)
{
	Velocity = VelocityCmPerS;
}

void ADragonPlasma::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bExploded)
	{
		FlashT += DeltaSeconds;
		Glow->SetIntensity(FMath::Max(0.f, 600000.f * (1.f - FlashT / 0.6f)));
		Core->SetRelativeScale3D(FVector(1.24f + FlashT * 30.f));
		Core->SetVisibility(FlashT < 0.15f);
		if (FlashT > 0.6f) Destroy();
		return;
	}

	const FVector Step = Velocity * DeltaSeconds;
	const float Len = Step.Size();
	const int32 N = FMath::Max(1, FMath::CeilToInt(Len / (6.f * M)));
	FVector At = GetActorLocation();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(Plasma), false, this);
	if (AActor* O = GetOwner()) Q.AddIgnoredActor(O);
	for (int32 i = 0; i < N; ++i)
	{
		const FVector Next = At + Step / N;
		FHitResult Hit;
		if (GetWorld()->LineTraceSingleByChannel(Hit, At, Next, ECC_Visibility, Q))
		{
			Explode(Hit.ImpactPoint);
			return;
		}
		// The sea surface (z = 0) counts even where there is no collision.
		if (Next.Z <= 0.f && At.Z > 0.f)
		{
			const float T = At.Z / (At.Z - Next.Z);
			Explode(FMath::Lerp(At, Next, T));
			return;
		}
		At = Next;
	}
	SetActorLocation(At);
	Travelled += Len;
	if (Travelled > RangeM * M) Explode(At);
}

void ADragonPlasma::Explode(const FVector& AtCm)
{
	bExploded = true;
	SetActorLocation(AtCm);
	SetLifeSpan(1.f);
	if (BlastFX)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(), BlastFX, AtCm, FRotator::ZeroRotator, FVector(1.f), true, true);
	}
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

#include "DragonSea.h"
#include "DragonFlight.h"
#include "DragonWorldSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

namespace
{
	UStaticMesh* PlaneMesh()
	{
		static UStaticMesh* Plane = nullptr;
		if (!Plane) Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
		return Plane;
	}

	void SetupWaterComponent(UStaticMeshComponent* C)
	{
		C->SetStaticMesh(PlaneMesh());
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->bAffectDistanceFieldLighting = false;
		C->SetMobility(EComponentMobility::Static);
	}
}

ADragonSea::ADragonSea()
{
	PrimaryActorTick.bCanEverTick = false;
	Surface = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Surface"));
	RootComponent = Surface;
	SetupWaterComponent(Surface);
	// The engine plane is 100 x 100 cm.
	Surface->SetRelativeScale3D(FVector(SeaHalfExtent / 50.0, SeaHalfExtent / 50.0, 1.0));
	Surface->SetBoundsScale(1.f);

}

void ADragonSea::BeginPlay()
{
	Super::BeginPlay();
	SetActorLocation(FVector(0, 0, 0));
	Surface->SetWorldScale3D(FVector(SeaHalfExtent / 50.0, SeaHalfExtent / 50.0, 1.0));
	if (!SeaMaterial) SeaMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/World/Materials/M_Sea.M_Sea"));
	if (!LakeMaterial) LakeMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/World/Materials/M_Lake.M_Lake"));
	if (SeaMaterial) Surface->SetMaterial(0, SeaMaterial);

	if (!bLakes) return;
	const UDragonWorldSubsystem* W = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	if (!W) return;
	int32 N = 0;
	for (const FDragonLake& L : W->GetLakes())
	{
		if (!L.Bounds.bIsValid) continue;
		if (BuildLake(*W, L, N)) ++N;
	}
	UE_LOG(LogDragon, Log, TEXT("Sea: surface + %d lakes"), N);
}

bool ADragonSea::BuildLake(const UDragonWorldSubsystem& W, const FDragonLake& L, int32 Index)
{
	// The lake's true outline: flood the basin from its deepest point over every cell
	// whose ground lies below the water level. Cells are half the bake's grid so the
	// shore follows the terrain; each quad overhangs a little and the ground hides it.
	const double Step = 1250.0; // cm
	const FVector2D Min = L.Bounds.Min - FVector2D(Step * 2), Max = L.Bounds.Max + FVector2D(Step * 2);
	const int32 NX = FMath::Clamp(FMath::CeilToInt((Max.X - Min.X) / Step), 2, 512);
	const int32 NY = FMath::Clamp(FMath::CeilToInt((Max.Y - Min.Y) / Step), 2, 512);
	TArray<double> G; G.SetNumUninitialized(NX * NY);
	int32 Deep = -1; double DeepZ = TNumericLimits<double>::Max();
	for (int32 y = 0; y < NY; ++y)
		for (int32 x = 0; x < NX; ++x)
		{
			const double Z = W.HeightAt(Min.X + (x + 0.5) * Step, Min.Y + (y + 0.5) * Step);
			G[y * NX + x] = Z;
			const bool bInside = x >= 2 && y >= 2 && x < NX - 2 && y < NY - 2;
			if (bInside && Z < DeepZ) { DeepZ = Z; Deep = y * NX + x; }
		}
	if (Deep < 0 || DeepZ >= L.Level) return false;

	TArray<uint8> Wet; Wet.SetNumZeroed(NX * NY);
	TArray<int32> Stack; Stack.Add(Deep); Wet[Deep] = 1;
	int32 Count = 0;
	while (Stack.Num())
	{
		const int32 I = Stack.Pop(EAllowShrinking::No); ++Count;
		const int32 x = I % NX, y = I / NX;
		const int32 Nb[4] = { x > 0 ? I - 1 : -1, x < NX - 1 ? I + 1 : -1, y > 0 ? I - NX : -1, y < NY - 1 ? I + NX : -1 };
		for (int32 J : Nb)
			if (J >= 0 && !Wet[J] && G[J] < L.Level + 50.0) { Wet[J] = 1; Stack.Add(J); }
		if (Count > 200000) break;
	}

	TArray<FVector> V; TArray<int32> T; TArray<FVector> Nm; TArray<FVector2D> UV; TArray<FColor> Col; TArray<FProcMeshTangent> Tan;
	const double Pad = Step * 0.6; // overhang under the shore
	for (int32 y = 0; y < NY; ++y)
		for (int32 x = 0; x < NX; ++x)
		{
			if (!Wet[y * NX + x]) continue;
			const double cx = Min.X + (x + 0.5) * Step, cy = Min.Y + (y + 0.5) * Step;
			const int32 B = V.Num();
			V.Add(FVector(cx - Pad, cy - Pad, 0)); V.Add(FVector(cx + Pad, cy - Pad, 0));
			V.Add(FVector(cx + Pad, cy + Pad, 0)); V.Add(FVector(cx - Pad, cy + Pad, 0));
			T.Append({ B, B + 2, B + 1, B, B + 3, B + 2, B, B + 1, B + 2, B, B + 2, B + 3 }); // both faces
			for (int k = 0; k < 4; ++k) { Nm.Add(FVector::UpVector); UV.Add(FVector2D(V[B + k].X, V[B + k].Y) / 10000.0); Tan.Add(FProcMeshTangent(1, 0, 0)); }
		}
	if (T.Num() == 0) return false;
	UProceduralMeshComponent* C = NewObject<UProceduralMeshComponent>(this, *FString::Printf(TEXT("Lake%d"), Index));
	C->SetupAttachment(RootComponent);
	C->SetAbsolute(true, true, true);
	C->RegisterComponent();
	C->SetWorldLocationAndRotation(FVector(0, 0, L.Level), FRotator::ZeroRotator);
	C->SetWorldScale3D(FVector::OneVector);
	C->CreateMeshSection(0, V, T, Nm, UV, Col, Tan, false);
	C->SetCastShadow(false);
	if (LakeMaterial) C->SetMaterial(0, LakeMaterial);
	LakeSurfaces.Add(C);
	return true;
}

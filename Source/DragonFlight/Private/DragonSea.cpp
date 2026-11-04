#include "DragonSea.h"
#include "DragonFlight.h"
#include "DragonWorldSubsystem.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

ADragonSea::ADragonSea()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	Grid = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Grid"));
	Grid->SetupAttachment(Root);
	Frame = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Frame"));
	Frame->SetupAttachment(Root);
	for (UProceduralMeshComponent* C : { Grid.Get(), Frame.Get() })
	{
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->bAffectDistanceFieldLighting = false;
		C->bUseAsyncCooking = true;
	}
}

void ADragonSea::BuildSeaMeshes()
{
	const double H = GridHalfM * 100.0;
	TArray<FVector> V; TArray<int32> T; TArray<FVector> N; TArray<FVector2D> UV; TArray<FColor> C; TArray<FProcMeshTangent> Tg;

	// Wave grid: N x N quads, spacing finer toward the middle (|u|^1.6 warp).
	const int32 Q = FMath::Clamp(GridQuads, 16, 1024);
	auto Warp = [](double U) { return FMath::Sign(U) * FMath::Pow(FMath::Abs(U), 1.6); };
	V.Reserve((Q + 1) * (Q + 1));
	for (int32 y = 0; y <= Q; ++y)
		for (int32 x = 0; x <= Q; ++x)
		{
			const double U = Warp(x * 2.0 / Q - 1.0), W = Warp(y * 2.0 / Q - 1.0);
			V.Add(FVector(U * H, W * H, 0.0));
			N.Add(FVector::UpVector); UV.Add(FVector2D(U, W)); Tg.Add(FProcMeshTangent(1, 0, 0));
		}
	T.Reserve(Q * Q * 6);
	for (int32 y = 0; y < Q; ++y)
		for (int32 x = 0; x < Q; ++x)
		{
			const int32 A = y * (Q + 1) + x, B = A + 1, Cc = A + (Q + 1), D = Cc + 1;
			T.Append({ A, Cc, B, B, Cc, D });
		}
	Grid->CreateMeshSection(0, V, T, N, UV, C, Tg, false);
	// Waves lift it a couple of metres: keep it from being culled at the rim.
	Grid->SetBoundsScale(1.5f);

	// Frame: the flat sea from the grid's rim out to the horizon (a square ring).
	V.Reset(); T.Reset(); N.Reset(); UV.Reset(); Tg.Reset();
	const double F = FarM * 100.0;
	const FVector Pts[8] = { {-F,-F,0}, {F,-F,0}, {F,F,0}, {-F,F,0}, {-H,-H,0}, {H,-H,0}, {H,H,0}, {-H,H,0} };
	for (const FVector& P : Pts) { V.Add(P); N.Add(FVector::UpVector); UV.Add(FVector2D(P.X / F, P.Y / F)); Tg.Add(FProcMeshTangent(1, 0, 0)); }
	// Four trapezoids between outer (0..3) and inner (4..7) squares.
	for (int32 i = 0; i < 4; ++i)
	{
		const int32 o0 = i, o1 = (i + 1) % 4, i0 = 4 + i, i1 = 4 + (i + 1) % 4;
		T.Append({ o0, i0, o1, o1, i0, i1 });
	}
	Frame->CreateMeshSection(0, V, T, N, UV, C, Tg, false);
}

void ADragonSea::BeginPlay()
{
	Super::BeginPlay();
	if (!SeaMaterial) SeaMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/World/Materials/M_Sea.M_Sea"));
	if (!LakeMaterial) LakeMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/World/Materials/M_Lake.M_Lake"));
	BuildSeaMeshes();
	if (SeaMaterial) { Grid->SetMaterial(0, SeaMaterial); Frame->SetMaterial(0, SeaMaterial); }
	SetActorLocation(FVector::ZeroVector);

	if (!bLakes) return;
	const UDragonWorldSubsystem* W = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	if (!W) return;
	int32 N = 0;
	for (const FDragonLake& L : W->GetLakes())
	{
		if (!L.Bounds.bIsValid) continue;
		if (BuildLake(*W, L, N)) ++N;
	}
	UE_LOG(LogDragon, Display, TEXT("Sea: wave grid %d^2, %d lakes"), GridQuads, N);
}

void ADragonSea::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// Follow the camera in coarse steps; the waves live in world space, so they don't move.
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager) return;
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	const double S = SnapM * 100.0;
	const FVector At(FMath::RoundToDouble(Cam.X / S) * S, FMath::RoundToDouble(Cam.Y / S) * S, 0.0);
	if (!At.Equals(GetActorLocation(), 1.0)) SetActorLocation(At);
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
	C->SetAbsolute(true, true, true);
	C->SetupAttachment(RootComponent);
	C->RegisterComponent();
	C->SetWorldLocationAndRotation(FVector(0, 0, L.Level), FRotator::ZeroRotator);
	C->SetWorldScale3D(FVector::OneVector);
	C->CreateMeshSection(0, V, T, Nm, UV, Col, Tan, false);
	C->SetCastShadow(false);
	if (LakeMaterial) C->SetMaterial(0, LakeMaterial);
	LakeSurfaces.Add(C);
	return true;
}

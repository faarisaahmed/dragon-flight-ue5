#include "DragonSea.h"
#include "DragonFlight.h"
#include "DragonWorldSubsystem.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "DragonSky.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "EngineUtils.h"

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
	if (SeaMaterial)
	{
		// One instance for both, so the weather can raise the sea (WaveAmplitude, FoamAmount).
		SeaMID = UMaterialInstanceDynamic::Create(SeaMaterial, this);
		Grid->SetMaterial(0, SeaMID); Frame->SetMaterial(0, SeaMID);
	}
	SetActorLocation(FVector::ZeroVector);

	const UDragonWorldSubsystem* W = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	if (!W) return;
	if (bRivers)
	{
		if (!RiverMaterial) RiverMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/World/Materials/M_River.M_River"));
		const int32 NR = BuildRivers(*W);
		UE_LOG(LogDragon, Display, TEXT("Sea: %d rivers"), NR);
	}
	if (!bLakes) return;
	int32 N = 0;
	for (const FDragonLake& L : W->GetLakes())
	{
		if (!L.Bounds.bIsValid) continue;
		if (BuildLake(*W, L, N)) ++N;
	}
	UE_LOG(LogDragon, Display, TEXT("Sea: wave grid %d^2, %d lakes"), GridQuads, N);
}

ADragonSea* ADragonSea::Get(const UObject* Ctx)
{
	UWorld* W = Ctx ? Ctx->GetWorld() : nullptr;
	if (!W) return nullptr;
	TActorIterator<ADragonSea> It(W);
	return It ? *It : nullptr;
}

float ADragonSea::SurfaceM(double Xm, double Ym) const
{
	// Mirrors M_Sea's displacement (DragonArchipelagoBuilder.cpp GWaves / "Swell").
	static const float Wv[12][4] = {
		{ 0.883f,  0.469f, 110.0f, 0.550f }, { 0.695f,  0.719f,  73.0f, 0.360f }, { 0.999f,  0.052f,  52.0f, 0.260f },
		{ 0.375f,  0.927f,  37.0f, 0.180f }, { 0.927f, -0.375f,  27.0f, 0.120f }, { 0.788f,  0.616f,  19.0f, 0.085f },
		{ 0.974f,  0.225f,  13.5f, 0.060f }, { 0.035f,  0.999f,   9.6f, 0.040f }, { 0.743f, -0.669f,   6.8f, 0.028f },
		{ 0.530f,  0.848f,   4.9f, 0.020f }, { 0.993f, -0.122f,   3.4f, 0.014f }, { -0.309f, 0.951f,   2.4f, 0.010f } };
	const UWorld* World = GetWorld();
	if (!World) return 0.f;
	double Fade = SeaState;
	if (const UGameInstance* GI = World->GetGameInstance())
		if (const UDragonWorldSubsystem* WS = GI->GetSubsystem<UDragonWorldSubsystem>())
			Fade *= FMath::Clamp((-WS->HeightAt(Xm * 100.0, Ym * 100.0) / 100.0 - 0.5) / 7.0, 0.0, 1.0);
	if (Fade <= 0.0) return 0.f;
	const double T = World->GetTimeSeconds();
	double H = 0.0;
	for (int32 i = 0; i < 12; ++i)
	{
		const double Dx = Wv[i][0], Dy = Wv[i][1], L = Wv[i][2];
		const double Group = 0.65 + 0.35 * FMath::Sin((Xm * Dy - Ym * Dx) * (UE_DOUBLE_TWO_PI / (L * 7.3)) + i * 1.7) * FMath::Sin((Xm * Dx + Ym * Dy) * (UE_DOUBLE_TWO_PI / (L * 11.0)) + i * 2.3);
		const double K = UE_DOUBLE_TWO_PI / L, C = FMath::Sqrt(9.81 / K);
		H += Wv[i][3] * Fade * Group * FMath::Sin(K * (Dx * Xm + Dy * Ym - C * T) + i * 0.9);
	}
	return (float)H;
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

	// Sea state follows the wind: a calm glassy sea on a clear day, white horses in a storm.
	if (SeaMID)
	{
		float Wind = 1.f;
		if (const ADragonSky* Sky = ADragonSky::Get(this)) Wind = Sky->GetWeatherState().Wind;
		const float Target = FMath::Clamp(0.45f + 0.55f * Wind, 0.35f, 2.2f);
		SeaState = FMath::FInterpTo(SeaState, Target, DeltaSeconds, 0.05f);
		SeaMID->SetScalarParameterValue(TEXT("WaveAmplitude"), SeaState);
		SeaMID->SetScalarParameterValue(TEXT("FoamAmount"), FMath::Clamp(0.6f + 0.5f * (SeaState - 0.8f) * 2.f, 0.5f, 2.0f));
	}
}

bool ADragonSea::BuildLake(const UDragonWorldSubsystem& W, const FDragonLake& L, int32 Index)
{
	if (L.Wet.Num() == L.W * L.H && L.W > 0)
	{
		// lakes2: the wet cells were found on the final terrain by a priority flood, so the
		// outline is exact; each cell overhangs a little under the shore.
		TArray<FVector> V; TArray<int32> T; TArray<FVector> Nm; TArray<FVector2D> UV; TArray<FColor> Col; TArray<FProcMeshTangent> Tan;
		const double C = L.Cell, Pad = C * 0.75;
		for (int32 y = 0; y < L.H; ++y)
		{
			// One quad per run of wet cells along the row.
			int32 x = 0;
			while (x < L.W)
			{
				if (!L.Wet[y * L.W + x]) { ++x; continue; }
				const int32 x0 = x;
				while (x < L.W && L.Wet[y * L.W + x]) ++x;
				const double cx0 = L.Bounds.Min.X + (x0 + 0.5) * C, cx1 = L.Bounds.Min.X + (x - 0.5) * C;
				const double cy = L.Bounds.Min.Y + (y + 0.5) * C;
				const int32 B = V.Num();
				V.Add(FVector(cx0 - Pad, cy - Pad, 0)); V.Add(FVector(cx1 + Pad, cy - Pad, 0));
				V.Add(FVector(cx1 + Pad, cy + Pad, 0)); V.Add(FVector(cx0 - Pad, cy + Pad, 0));
				T.Append({ B, B + 2, B + 1, B, B + 3, B + 2 });
				for (int k = 0; k < 4; ++k) { Nm.Add(FVector::UpVector); UV.Add(FVector2D(V[B + k].X, V[B + k].Y) / 10000.0); Tan.Add(FProcMeshTangent(1, 0, 0)); }
			}
		}
		if (T.Num() == 0) return false;
		UProceduralMeshComponent* Cm = NewObject<UProceduralMeshComponent>(this, *FString::Printf(TEXT("Lake%d"), Index));
		Cm->SetAbsolute(true, true, true);
		Cm->SetupAttachment(RootComponent);
		Cm->RegisterComponent();
		Cm->SetWorldLocationAndRotation(FVector(0, 0, L.Level), FRotator::ZeroRotator);
		Cm->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Cm->CreateMeshSection(0, V, T, Nm, UV, Col, Tan, false);
		Cm->SetCastShadow(false);
		if (LakeMaterial) Cm->SetMaterial(0, LakeMaterial);
		LakeSurfaces.Add(Cm);
		return true;
	}
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

int32 ADragonSea::BuildRivers(const UDragonWorldSubsystem& W)
{
	// Each river is a ribbon at its water surface, a little wider than the channel so the
	// banks swallow its edges; UVs in metres (across, along) carry the flow, and vertex
	// colour R marks where the bed steepens into rapids.
	TArray<FVector> V; TArray<int32> T; TArray<FVector> Nm; TArray<FVector2D> UV; TArray<FColor> Col; TArray<FProcMeshTangent> Tan;
	int32 Count = 0;
	for (const FDragonRiver& R : W.GetRivers())
	{
		const int32 NP = R.Points.Num();
		if (NP < 2 || R.Widths.Num() != NP) continue;
		// The web game's river levels predate the erosion and relief passes: lay each point on the
		// ground as it is now (a hand below the bank, in its channel), and never let it run uphill.
		TArray<FVector> Pts = R.Points;
		for (int32 i = 0; i < NP; ++i)
		{
			const double G = W.HeightAt(Pts[i].X, Pts[i].Y);
			Pts[i].Z = FMath::Clamp(Pts[i].Z, G - 80.0, G + 25.0);
			if (i > 0) Pts[i].Z = FMath::Min(Pts[i].Z, Pts[i - 1].Z);
		}
		if (Pts.Last().Z < -50.0) Pts.Last().Z = FMath::Max(Pts.Last().Z, -50.0);
		// Catmull-Rom, 4 steps per span, so bends are curves.
		TArray<FVector> P; TArray<float> Wd;
		for (int32 i = 0; i < NP - 1; ++i)
		{
			const FVector& P0 = Pts[FMath::Max(i - 1, 0)]; const FVector& P1 = Pts[i];
			const FVector& P2 = Pts[i + 1]; const FVector& P3 = Pts[FMath::Min(i + 2, NP - 1)];
			for (int32 k = 0; k < 4; ++k)
			{
				const float t = k / 4.f, t2 = t * t, t3 = t2 * t;
				FVector Q = 0.5f * ((2.f * P1) + (-P0 + P2) * t + (2.f * P0 - 5.f * P1 + 4.f * P2 - P3) * t2 + (-P0 + 3.f * P1 - 3.f * P2 + P3) * t3);
				Q.Z = FMath::Lerp(P1.Z, P2.Z, t); // water never climbs: no overshoot in height
				P.Add(Q); Wd.Add(FMath::Lerp(R.Widths[i], R.Widths[i + 1], t));
			}
		}
		P.Add(Pts.Last()); Wd.Add(R.Widths.Last());
		const int32 Base = V.Num();
		double Along = 0.0;
		for (int32 i = 0; i < P.Num(); ++i)
		{
			const FVector Prev = P[FMath::Max(i - 1, 0)], Next = P[FMath::Min(i + 1, P.Num() - 1)];
			FVector2D Dir(Next.X - Prev.X, Next.Y - Prev.Y);
			if (!Dir.Normalize()) Dir = FVector2D(1, 0);
			const FVector Side(-Dir.Y, Dir.X, 0.0);
			// A shallow V across: the edges sit 0.6 m under the surface and a metre past the
			// mapped width, so the banks cut their own ragged shoreline through it.
			const double Half = Wd[i] * 0.5 + 100.0;
			if (i > 0) Along += FVector::Dist2D(P[i], P[i - 1]);
			const double Run = FMath::Max(1.0, FVector::Dist2D(Prev, Next));
			const float Steep = FMath::Clamp(float((Prev.Z - Next.Z) / Run - 0.04) * 4.f, 0.f, 1.f);
			const FColor C(uint8(Steep * 255.f), 0, 0, 255);
			const FVector Edge(0, 0, -60.0);
			V.Add(P[i] + Side * Half + Edge); V.Add(P[i]); V.Add(P[i] - Side * Half + Edge);
			UV.Add(FVector2D(-Half / 100.0, Along / 100.0)); UV.Add(FVector2D(0.0, Along / 100.0)); UV.Add(FVector2D(Half / 100.0, Along / 100.0));
			for (int k = 0; k < 3; ++k) { Nm.Add(FVector::UpVector); Col.Add(C); Tan.Add(FProcMeshTangent(Dir.X, Dir.Y, 0)); }
			if (i > 0)
			{
				for (int32 s2 = 0; s2 < 2; ++s2)
				{
					const int32 A = Base + (i - 1) * 3 + s2, B = A + 1, Cc = A + 3, D = A + 4;
					T.Append({ A, Cc, B, B, Cc, D, A, B, Cc, B, D, Cc }); // both faces
				}
			}
		}
		++Count;
	}
	if (T.Num() == 0) return 0;
	Rivers = NewObject<UProceduralMeshComponent>(this, TEXT("Rivers"));
	Rivers->SetAbsolute(true, true, true);
	Rivers->SetupAttachment(RootComponent);
	Rivers->RegisterComponent();
	Rivers->SetWorldLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);
	Rivers->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Rivers->SetCastShadow(false);
	Rivers->CreateMeshSection(0, V, T, Nm, UV, Col, Tan, false);
	if (RiverMaterial) Rivers->SetMaterial(0, RiverMaterial);
	return Count;
}

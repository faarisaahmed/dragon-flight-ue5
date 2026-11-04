#include "DragonScatter.h"
#include "DragonFlight.h"
#include "DragonWorldSubsystem.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Scalability.h"

namespace
{
	constexpr double M = 100.0;

	float Smooth(float X, float A, float B)
	{
		const float T = FMath::Clamp((X - A) / (B - A), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}

	/** Small deterministic hash -> [0,1). Same tile, same trees, every visit. */
	struct FTileRand
	{
		uint32 S;
		explicit FTileRand(uint32 Seed) : S(Seed ? Seed : 0x9E3779B9u) {}
		float Next()
		{
			S ^= S << 13; S ^= S >> 17; S ^= S << 5;
			return (S & 0xFFFFFF) / 16777216.f;
		}
	};
}

ADragonScatter::ADragonScatter()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.f;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ADragonScatter::SetupDefaultLayers(const TMap<FName, TArray<TSoftObjectPtr<UStaticMesh>>>& ByGroup)
{
	ScatterLayers.Reset();
	auto Add = [&](FName Name, EDragonScatterKind Kind, FName Group, float PerHa, float Tile, float Radius, FVector2D Scale, float Sink, float Align, bool bShadow, bool bCollide, int32 Seed)
	{
		const TArray<TSoftObjectPtr<UStaticMesh>>* Meshes = ByGroup.Find(Group);
		if (!Meshes || Meshes->Num() == 0) return;
		FDragonScatterLayer L;
		L.Name = Name; L.Kind = Kind; L.Meshes = *Meshes; L.PerHectare = PerHa; L.TileM = Tile; L.RadiusM = Radius;
		L.ScaleRange = Scale; L.SinkM = Sink; L.AlignToSlope = Align; L.bCastShadow = bShadow; L.bCollide = bCollide; L.Seed = Seed;
		ScatterLayers.Add(L);
	};
	//   name        kind                          group          /ha    tile   radius  scale                 sink  align shadow collide seed
	Add("Trees",    EDragonScatterKind::Tree,     "trees",       140.f, 256.f, 9000.f, FVector2D(0.8f, 1.5f), 0.4f, 0.f, true,  false, 11);
	Add("Saplings", EDragonScatterKind::Sapling,  "trees",       40.f,  192.f, 2500.f, FVector2D(0.35f, 0.7f), 0.2f, 0.f, true,  false, 23);
	Add("DeadWood", EDragonScatterKind::DeadWood, "deadwood",    8.f,   192.f, 1500.f, FVector2D(0.8f, 1.4f), 0.2f, 0.8f, true, false, 37);
	// The Poly Haven cliff and rock scans are already life-size (cliff pieces 20-50 m), so
	// they are scattered near 1:1 and sparsely; the terrain is the landform, they are detail.
	Add("Rocks",    EDragonScatterKind::Rock,     "rocks",       12.f,  256.f, 5000.f, FVector2D(0.6f, 1.6f), 0.4f, 0.7f, true,  true,  41);
	Add("Cliffs",   EDragonScatterKind::Cliff,    "cliffs",      0.5f,  512.f, 12000.f, FVector2D(0.7f, 1.3f), 3.0f, 0.9f, true, true,  53);
	Add("Ground",   EDragonScatterKind::Ground,   "ground",      2600.f, 32.f, 160.f,  FVector2D(0.7f, 1.3f), 0.05f, 0.6f, false, false, 67);
}

void ADragonScatter::BeginPlay()
{
	Super::BeginPlay();
	LoadMasks();
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (const UDragonWorldSubsystem* WS = GI->GetSubsystem<UDragonWorldSubsystem>())
		{
			WorldSizeM = WS->GetTerrainSize() / M;
		}
	}
	LayerMeshes.SetNum(ScatterLayers.Num());
	for (int32 i = 0; i < ScatterLayers.Num(); ++i)
	{
		for (const TSoftObjectPtr<UStaticMesh>& S : ScatterLayers[i].Meshes)
		{
			if (UStaticMesh* Mesh = S.LoadSynchronous())
			{
				LayerMeshes[i].Add(Mesh);
				LoadedMeshes.AddUnique(Mesh);
			}
		}
	}
}

bool ADragonScatter::LoadMasks()
{
	const FString Dir = UDragonWorldSubsystem::DataDir();
	TArray<FString> Found;
	IFileManager::Get().FindFiles(Found, *(Dir / TEXT("Masks_*.png")), true, false);
	if (Found.Num() == 0) { UE_LOG(LogDragon, Warning, TEXT("Scatter: no Masks_*.png in %s"), *Dir); return false; }
	const FString Res = Found[0].Mid(6, Found[0].Len() - 10); // Masks_<res>.png

	IImageWrapperModule& IW = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	auto Load = [&](const FString& File, TArray<uint8>& Out, ERGBFormat Fmt, int32 Channels) -> int32
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *(Dir / File))) return 0;
		TSharedPtr<IImageWrapper> W = IW.CreateImageWrapper(EImageFormat::PNG);
		if (!W || !W->SetCompressed(Bytes.GetData(), Bytes.Num())) return 0;
		TArray<uint8> Raw;
		if (!W->GetRaw(ERGBFormat::RGBA, 8, Raw)) return 0;
		const int32 N = W->GetWidth();
		if (Channels == 4) Out = MoveTemp(Raw);
		else
		{
			Out.SetNumUninitialized(N * N * 3);
			for (int32 i = 0; i < N * N; ++i) { Out[i * 3] = Raw[i * 4]; Out[i * 3 + 1] = Raw[i * 4 + 1]; Out[i * 3 + 2] = Raw[i * 4 + 2]; }
		}
		return N;
	};
	MaskRes = Load(Found[0], Mask, ERGBFormat::RGBA, 4);
	const int32 R2 = Load(FString::Printf(TEXT("Masks2_%s.png"), *Res), Mask2, ERGBFormat::RGBA, 3);
	if (R2 != MaskRes) Mask2.Reset();
	UE_LOG(LogDragon, Log, TEXT("Scatter: masks %d px"), MaskRes);
	return MaskRes > 0;
}

FVector4f ADragonScatter::SampleMask(double XM, double YM) const
{
	if (MaskRes == 0) return FVector4f(0.5f, 0.5f, 0.f, 0.f);
	const double U = (XM / WorldSizeM + 0.5) * MaskRes - 0.5;
	const double V = (YM / WorldSizeM + 0.5) * MaskRes - 0.5;
	const int32 X0 = FMath::Clamp((int32)FMath::FloorToDouble(U), 0, MaskRes - 2);
	const int32 Y0 = FMath::Clamp((int32)FMath::FloorToDouble(V), 0, MaskRes - 2);
	const float TX = FMath::Clamp((float)(U - X0), 0.f, 1.f), TY = FMath::Clamp((float)(V - Y0), 0.f, 1.f);
	FVector4f Out;
	for (int32 C = 0; C < 4; ++C)
	{
		auto P = [&](int32 X, int32 Y) { return Mask[(Y * MaskRes + X) * 4 + C] / 255.f; };
		Out[C] = FMath::Lerp(FMath::Lerp(P(X0, Y0), P(X0 + 1, Y0), TX), FMath::Lerp(P(X0, Y0 + 1), P(X0 + 1, Y0 + 1), TX), TY);
	}
	return Out;
}

FVector ADragonScatter::SampleMask2(double XM, double YM) const
{
	if (Mask2.Num() == 0) return FVector::ZeroVector;
	const int32 X = FMath::Clamp((int32)((XM / WorldSizeM + 0.5) * MaskRes), 0, MaskRes - 1);
	const int32 Y = FMath::Clamp((int32)((YM / WorldSizeM + 0.5) * MaskRes), 0, MaskRes - 1);
	const int32 I = (Y * MaskRes + X) * 3;
	return FVector(Mask2[I] / 255.f, Mask2[I + 1] / 255.f, Mask2[I + 2] / 255.f);
}

float ADragonScatter::GroundM(double XM, double YM) const
{
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (const UDragonWorldSubsystem* WS = GI->GetSubsystem<UDragonWorldSubsystem>())
		{
			return WS->HeightAt(XM * M, YM * M) / M;
		}
	}
	return 0.f;
}

FVector ADragonScatter::GroundNormal(double XM, double YM) const
{
	const double E = 3.0;
	const float L = GroundM(XM - E, YM), R = GroundM(XM + E, YM), D = GroundM(XM, YM - E), U = GroundM(XM, YM + E);
	return FVector(L - R, D - U, 2.0 * E).GetSafeNormal();
}

bool ADragonScatter::Accept(const FDragonScatterLayer& L, double XM, double YM, float H, float Rand, float& OutScale) const
{
	const FVector4f Mk = SampleMask(XM, YM);
	const float Forest = Mk.X, Veg = Mk.Y, Ice = Mk.Z, Water = Mk.W;
	const FVector M2 = SampleMask2(XM, YM);
	const float Slope = M2.X, Pad = M2.Z;
	if (Water > 0.5f || Ice > 0.4f) return false;
	OutScale = 1.f;
	float P = 0.f;
	switch (L.Kind)
	{
	case EDragonScatterKind::Tree:
		if (H < 3.f) return false;
		P = FMath::Pow(Forest, 1.1f) * (1.f - Smooth(Slope, 0.46f, 0.88f)) * (1.f - Pad);
		// Trees shrink toward the tree line and the wood's edge.
		OutScale = FMath::Lerp(0.6f, 1.f, Smooth(Forest, 0.1f, 0.6f)) * (1.f - 0.35f * Smooth(H, 180.f, 320.f));
		break;
	case EDragonScatterKind::Sapling:
		if (H < 4.f) return false;
		P = (Forest * (1.f - Forest) * 3.f + Veg * (1.f - Forest) * 0.12f) * (1.f - Smooth(Slope, 0.5f, 0.85f)) * (1.f - Pad);
		break;
	case EDragonScatterKind::DeadWood:
		if (H < 3.f) return false;
		P = Forest * 0.5f * (1.f - Smooth(Slope, 0.5f, 0.85f));
		break;
	case EDragonScatterKind::Rock:
		if (H < -1.f) return false;
		P = Smooth(Slope, 0.28f, 0.7f) * 0.7f                    // scree and slopes
			+ (H < 9.f ? 0.45f : 0.f)                              // the shore
			+ (1.f - Veg) * (H > 2.f ? 0.12f : 0.f);              // bare ground
		P *= 1.f - Forest * 0.6f;
		OutScale = 1.f + 0.4f * Smooth(Slope, 0.5f, 0.9f);
		break;
	case EDragonScatterKind::Cliff:
		if (H < -2.f) return false;
		P = Smooth(Slope, 0.62f, 0.92f);
		break;
	case EDragonScatterKind::Ground:
		if (H < 1.5f) return false;
		P = Veg * (1.f - Forest * 0.4f) * (1.f - Smooth(Slope, 0.5f, 0.8f)) * (1.f - Pad * 0.7f);
		break;
	}
	return Rand < P;
}

float ADragonScatter::QualityScale() const
{
	static const float ByLevel[5] = { 0.25f, 0.45f, 0.65f, 0.85f, 1.f };
	const Scalability::FQualityLevels Q = Scalability::GetQualityLevels();
	return ByLevel[FMath::Clamp(Q.FoliageQuality, 0, 4)] * DistanceScale;
}

FVector ADragonScatter::ViewPointM() const
{
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (PC->PlayerCameraManager) return PC->PlayerCameraManager->GetCameraLocation() / M;
		if (const APawn* P = PC->GetPawn()) return P->GetActorLocation() / M;
	}
	return FVector::ZeroVector;
}

void ADragonScatter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (ScatterLayers.Num() == 0 || LayerMeshes.Num() != ScatterLayers.Num()) return;

	const FVector View = ViewPointM();
	PlanTimer -= DeltaSeconds;
	if (PlanTimer <= 0.f || FVector::Dist2D(View, LastPlanM) > 64.0)
	{
		PlanTimer = 0.5f;
		LastPlanM = View;
		const float QS = QualityScale();
		// Altitude pushes the radius out a little: from 1 km up you see much further.
		const float AltBoost = 1.f + FMath::Clamp((float)View.Z / 2000.f, 0.f, 0.5f);

		// Drop far tiles.
		TArray<FTileKey> ToDrop;
		for (const TPair<FTileKey, FTile>& T : Tiles)
		{
			const FDragonScatterLayer& L = ScatterLayers[T.Key.Layer];
			const FVector2D C((T.Key.X + 0.5) * L.TileM, (T.Key.Y + 0.5) * L.TileM);
			if (FVector2D::Distance(C, FVector2D(View)) > L.RadiusM * QS * AltBoost * 1.15f + L.TileM) ToDrop.Add(T.Key);
		}
		for (const FTileKey& K : ToDrop) DropTile(K);

		// Queue missing tiles, nearest first.
		Queue.Reset();
		for (int32 Li = 0; Li < ScatterLayers.Num(); ++Li)
		{
			const FDragonScatterLayer& L = ScatterLayers[Li];
			if (LayerMeshes[Li].Num() == 0) continue;
			const float R = L.RadiusM * QS * AltBoost;
			const int32 X0 = FMath::FloorToInt((View.X - R) / L.TileM), X1 = FMath::FloorToInt((View.X + R) / L.TileM);
			const int32 Y0 = FMath::FloorToInt((View.Y - R) / L.TileM), Y1 = FMath::FloorToInt((View.Y + R) / L.TileM);
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					const FVector2D C((X + 0.5) * L.TileM, (Y + 0.5) * L.TileM);
					if (FVector2D::Distance(C, FVector2D(View)) > R + L.TileM * 0.7f) continue;
					if (FMath::Abs(C.X) > WorldSizeM * 0.5 || FMath::Abs(C.Y) > WorldSizeM * 0.5) continue;
					const FTileKey K{ Li, X, Y };
					if (!Tiles.Contains(K)) Queue.Add(K);
				}
			}
		}
		Queue.Sort([&](const FTileKey& A, const FTileKey& B)
		{
			const FDragonScatterLayer& LA = ScatterLayers[A.Layer];
			const FDragonScatterLayer& LB = ScatterLayers[B.Layer];
			const double DA = FVector2D::Distance(FVector2D((A.X + 0.5) * LA.TileM, (A.Y + 0.5) * LA.TileM), FVector2D(View)) / LA.RadiusM;
			const double DB = FVector2D::Distance(FVector2D((B.X + 0.5) * LB.TileM, (B.Y + 0.5) * LB.TileM), FVector2D(View)) / LB.RadiusM;
			return DA < DB;
		});
	}

	// Time-sliced generation.
	const double Start = FPlatformTime::Seconds();
	int32 Done = 0;
	while (Done < Queue.Num() && (FPlatformTime::Seconds() - Start) * 1000.0 < BudgetMs)
	{
		const FTileKey K = Queue[Done++];
		if (!Tiles.Contains(K)) BuildTile(K.Layer, K.X, K.Y);
	}
	if (Done > 0) Queue.RemoveAt(0, Done, EAllowShrinking::No);
}

void ADragonScatter::BuildTile(int32 Li, int32 TX, int32 TY)
{
	const FDragonScatterLayer& L = ScatterLayers[Li];
	const TArray<UStaticMesh*>& Meshes = LayerMeshes[Li];
	FTile& Tile = Tiles.Add(FTileKey{ Li, TX, TY });
	if (Meshes.Num() == 0) return;

	TArray<TArray<FTransform>> PerMesh;
	PerMesh.SetNum(Meshes.Num());

	const float Hectares = (L.TileM * L.TileM) / 10000.f;
	const int32 Candidates = FMath::Max(1, FMath::RoundToInt(L.PerHectare * Hectares));
	FTileRand Rng(HashCombine(HashCombine(::GetTypeHash(TX * 73856093), ::GetTypeHash(TY * 19349663)), ::GetTypeHash(L.Seed)));
	const double OX = TX * (double)L.TileM, OY = TY * (double)L.TileM;

	// Jittered grid: even coverage without visible rows.
	const int32 Side = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt((float)Candidates)));
	const double Cell = L.TileM / Side;
	for (int32 j = 0; j < Side; ++j)
	{
		for (int32 i = 0; i < Side; ++i)
		{
			const double X = OX + (i + Rng.Next()) * Cell;
			const double Y = OY + (j + Rng.Next()) * Cell;
			const float Pick = Rng.Next(), Keep = Rng.Next(), S01 = Rng.Next(), Yaw = Rng.Next() * 360.f;
			const float H = GroundM(X, Y);
			float ScaleMul = 1.f;
			if (!Accept(L, X, Y, H, Keep, ScaleMul)) continue;

			const int32 MeshIdx = FMath::Min(Meshes.Num() - 1, (int32)(Pick * Meshes.Num()));
			const float Scale = FMath::Lerp(L.ScaleRange.X, L.ScaleRange.Y, S01 * S01) * ScaleMul;
			FQuat Rot = FQuat(FVector::UpVector, FMath::DegreesToRadians(Yaw));
			if (L.AlignToSlope > 0.f)
			{
				const FVector N = GroundNormal(X, Y);
				const FVector Up = FMath::Lerp(FVector::UpVector, N, L.AlignToSlope).GetSafeNormal();
				Rot = FQuat::FindBetweenNormals(FVector::UpVector, Up) * Rot;
			}
			const FVector Pos(X * M, Y * M, (H - L.SinkM * Scale) * M);
			PerMesh[MeshIdx].Add(FTransform(Rot, Pos, FVector(Scale)));
		}
	}

	for (int32 m = 0; m < Meshes.Num(); ++m)
	{
		if (PerMesh[m].Num() == 0) continue;
		UHierarchicalInstancedStaticMeshComponent* C = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, NAME_None, RF_Transient);
		C->SetStaticMesh(Meshes[m]);
		C->SetMobility(EComponentMobility::Static);
		C->SetCollisionEnabled(L.bCollide ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
		C->SetCastShadow(L.bCastShadow);
		C->bAffectDistanceFieldLighting = L.Kind != EDragonScatterKind::Ground;
		C->SetCullDistances(0, FMath::RoundToInt(L.RadiusM * 1.1f * M));
		C->SetupAttachment(RootComponent);
		C->RegisterComponent();
		C->AddInstances(PerMesh[m], false, true);
		Tile.Components.Add(C);
	}
}

void ADragonScatter::DropTile(const FTileKey& Key)
{
	if (FTile* T = Tiles.Find(Key))
	{
		for (UHierarchicalInstancedStaticMeshComponent* C : T->Components)
		{
			if (C) C->DestroyComponent();
		}
	}
	Tiles.Remove(Key);
}

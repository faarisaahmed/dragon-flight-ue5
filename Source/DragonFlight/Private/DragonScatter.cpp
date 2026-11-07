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
#include "Materials/MaterialInterface.h"

namespace
{
	constexpr double M = 100.0;

	float Smooth(float X, float A, float B)
	{
		const float T = FMath::Clamp((X - A) / (B - A), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}

	/** Smooth value noise in [0,1] on a grid of CellM metres: clumps, groves, boulder fields. */
	float Clump(double XM, double YM, double CellM, uint32 Seed)
	{
		const double U = XM / CellM, V = YM / CellM;
		const int64 X0 = (int64)FMath::FloorToDouble(U), Y0 = (int64)FMath::FloorToDouble(V);
		auto Hv = [Seed](int64 X, int64 Y)
		{
			uint32 h = (uint32)(X * 73856093) ^ (uint32)(Y * 19349663) ^ (Seed * 83492791u);
			h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
			return (h & 0xFFFF) / 65535.f;
		};
		float TX = (float)(U - X0), TY = (float)(V - Y0);
		TX = TX * TX * (3.f - 2.f * TX); TY = TY * TY * (3.f - 2.f * TY);
		return FMath::Lerp(FMath::Lerp(Hv(X0, Y0), Hv(X0 + 1, Y0), TX), FMath::Lerp(Hv(X0, Y0 + 1), Hv(X0 + 1, Y0 + 1), TX), TY);
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
		const TArray<TSoftObjectPtr<UStaticMesh>>* Found = ByGroup.Find(Group);
		if (!Found || Found->Num() == 0) return;
		// glTF scans carry their lower LODs as separate meshes: keep only full detail.
		TArray<TSoftObjectPtr<UStaticMesh>> Filtered;
		for (const TSoftObjectPtr<UStaticMesh>& S : *Found)
		{
			const FString N = S.GetAssetName();
			if (N.Contains(TEXT("_LOD")) && !N.Contains(TEXT("_LOD0"))) continue;
			Filtered.Add(S);
		}
		const TArray<TSoftObjectPtr<UStaticMesh>>* Meshes = &Filtered;
		if (Meshes->Num() == 0) return;
		FDragonScatterLayer L;
		L.Name = Name; L.Kind = Kind; L.Meshes = *Meshes; L.PerHectare = PerHa; L.TileM = Tile; L.RadiusM = Radius;
		L.ScaleRange = Scale; L.SinkM = Sink; L.AlignToSlope = Align; L.bCastShadow = bShadow; L.bCollide = bCollide; L.Seed = Seed;
		ScatterLayers.Add(L);
	};
	//   name        kind                          group          /ha    tile   radius  scale                 sink  align shadow collide seed
	Add("Trees",    EDragonScatterKind::Tree,     "trees",       320.f, 256.f, 6000.f, FVector2D(0.8f, 1.5f), 0.4f, 0.f, true,  false, 11);
	Add("Saplings", EDragonScatterKind::Sapling,  "trees",       40.f,  192.f, 2500.f, FVector2D(0.35f, 0.7f), 0.2f, 0.f, true,  false, 23);
	Add("DeadWood", EDragonScatterKind::DeadWood, "deadwood",    8.f,   192.f, 1500.f, FVector2D(0.8f, 1.4f), 0.2f, 0.8f, true, false, 37);
	// The Poly Haven cliff and rock scans are already life-size (cliff pieces 20-50 m), so
	// they are scattered near 1:1 and sparsely; the terrain is the landform, they are detail.
	Add("Rocks",    EDragonScatterKind::Rock,     "rocks",       90.f,  256.f, 5000.f, FVector2D(0.9f, 2.2f), 0.4f, 0.7f, true,  true,  41);
	// Stones in the grass at your feet (the same scans, small).
	Add("Stones",   EDragonScatterKind::Stone,    "rocks",       160.f, 64.f,  450.f,  FVector2D(0.12f, 0.4f), 0.1f, 0.8f, true,  false, 43);
	// Cliff pieces stand upright and are set into steep faces as a skin of real rock
	// (the 5.5 m landscape grid can only round a sea cliff off); see BuildTile.
	Add("Cliffs",   EDragonScatterKind::Cliff,    "cliffs",      0.6f,  512.f, 12000.f, FVector2D(0.7f, 1.4f), 4.0f, 0.15f, true, true,  53);
	// Turf itself is the landscape grass system's job now; this adds the odd bigger shrub.
	Add("Ground",   EDragonScatterKind::Ground,   "ground",      500.f, 32.f, 350.f,  FVector2D(0.7f, 1.3f), 0.05f, 0.6f, false, false, 67);
	// Juniper, heather and bracken clumps big enough to read from flying height.
	Add("Shrubs",   EDragonScatterKind::Shrub,    "ground",      60.f,  128.f, 1400.f, FVector2D(1.3f, 2.6f), 0.1f, 0.5f, true,  false, 71);
	for (FDragonScatterLayer& L : ScatterLayers)
	{
		if (L.Kind == EDragonScatterKind::Shrub)
			L.Meshes.RemoveAll([](const TSoftObjectPtr<UStaticMesh>& S) { const FString N = S.GetAssetName(); return !(N.Contains(TEXT("shrub")) || N.Contains(TEXT("fern"))); });
		if (L.Kind == EDragonScatterKind::Stone)
			L.Meshes.RemoveAll([](const TSoftObjectPtr<UStaticMesh>& S) { return S.GetAssetName().Contains(TEXT("coast")); });
	}
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
			WorldSub = WS;
		}
	}
	// Drift ice off the arctic islands: the cliff and boulder scans, recast in ice.
	IceMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/World/Materials/M_Ice.M_Ice"));
	if (IceMaterial && !ScatterLayers.ContainsByPredicate([](const FDragonScatterLayer& L) { return L.Kind == EDragonScatterKind::Iceberg; }))
	{
		FDragonScatterLayer Ice;
		Ice.Name = TEXT("Icebergs"); Ice.Kind = EDragonScatterKind::Iceberg;
		for (const FDragonScatterLayer& L : ScatterLayers)
			if (L.Kind == EDragonScatterKind::Cliff || L.Kind == EDragonScatterKind::Rock)
				for (const TSoftObjectPtr<UStaticMesh>& Mh : L.Meshes)
					if (!Mh.GetAssetName().Contains(TEXT("coast_land"))) Ice.Meshes.Add(Mh);
		Ice.PerHectare = 0.05f; Ice.TileM = 1024.f; Ice.RadiusM = 14000.f;
		Ice.ScaleRange = FVector2D(0.5f, 2.2f); Ice.SinkM = 0.f; Ice.AlignToSlope = 0.f;
		Ice.bCastShadow = true; Ice.bCollide = true; Ice.Seed = 79;
		if (Ice.Meshes.Num()) ScatterLayers.Add(Ice);
	}
	LayerMeshes.SetNum(ScatterLayers.Num());
	ShoreMesh.SetNum(ScatterLayers.Num());
	for (int32 i = 0; i < ScatterLayers.Num(); ++i)
	{
		for (const TSoftObjectPtr<UStaticMesh>& S : ScatterLayers[i].Meshes)
		{
			if (UStaticMesh* Mesh = S.LoadSynchronous())
			{
				LayerMeshes[i].Add(Mesh);
				LoadedMeshes.AddUnique(Mesh);
				ShoreMesh[i].Add(Mesh->GetName().Contains(TEXT("coast")));
			}
		}
		// A layer that is all shore or all inland pieces uses them everywhere.
		const int32 NShore = ShoreMesh[i].FilterByPredicate([](bool b) { return b; }).Num();
		if (NShore == 0 || NShore == ShoreMesh[i].Num()) ShoreMesh[i].Reset();
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
	const int32 R3 = Load(FString::Printf(TEXT("Masks3_%s.png"), *Res), Mask3, ERGBFormat::RGBA, 3);   // biomes (optional)
	if (R3 != MaskRes) Mask3.Reset();
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

FVector ADragonScatter::SampleMask3(double XM, double YM) const
{
	if (Mask3.Num() == 0) return FVector::ZeroVector;
	const int32 X = FMath::Clamp((int32)((XM / WorldSizeM + 0.5) * MaskRes), 0, MaskRes - 1);
	const int32 Y = FMath::Clamp((int32)((YM / WorldSizeM + 0.5) * MaskRes), 0, MaskRes - 1);
	const int32 I = (Y * MaskRes + X) * 3;
	return FVector(Mask3[I] / 255.f, Mask3[I + 1] / 255.f, Mask3[I + 2] / 255.f);
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
	const FVector Bio = SampleMask3(XM, YM);                  // woodland, volcanic, moorland
	const float Volc = (float)Bio.Y, Moor = (float)Bio.Z;
	// Woodland isles grow conifer forest wherever the ground allows; lava and bog don't.
	const float Forest = FMath::Max(Mk.X, (float)Bio.X * 0.8f) * (1.f - Volc) * (1.f - 0.7f * Moor);
	const float Veg = Mk.Y, Ice = Mk.Z, Water = Mk.W;
	const FVector M2 = SampleMask2(XM, YM);
	const float Slope = M2.X, Pad = M2.Z;
	if (L.Kind == EDragonScatterKind::Iceberg)
	{
		// Afloat in open water a few hundred metres to a few km off the arctic coasts.
		if (!WorldSub || H > -14.f || H < -160.f) return false;
		const float Cold = WorldSub->ColdAt(XM * M, YM * M);
		OutScale = 1.f;
		return Rand < Smooth(Cold, 0.45f, 0.9f) * Smooth(-H, 14.f, 40.f);
	}
	if (Water > 0.5f || Ice > 0.4f) return false;
	// Climate: the snow line falls toward the north (UDragonWorldSubsystem::SnowLineAt); the tree
	// line sits well below it, and nothing grows on lying snow.
	const float SnowM = WorldSub ? (float)(WorldSub->SnowLineAt(XM * M, YM * M) / M) : 480.f;
	const float Cold = WorldSub ? WorldSub->ColdAt(XM * M, YM * M) : 0.f;
	const float TreeLine = FMath::Min(330.f, SnowM * 0.62f);
	const float Snowy = Smooth(H, SnowM - 30.f, SnowM + 40.f);
	OutScale = 1.f;
	float P = 0.f;
	float Field = 0.f;   // boulder-field strength (rocks)
	switch (L.Kind)
	{
	case EDragonScatterKind::Tree:
		if (H < 3.f) return false;
		P = FMath::Pow(Forest, 1.1f);
		{
			// Beyond the woodland isles: groves of pine in the sheltered hollows and gullies, and
			// lone trees on the lower slopes, as in the films' islands.
			const double E = 18.0;
			const float Lap = (GroundM(XM + E, YM) + GroundM(XM - E, YM) + GroundM(XM, YM + E) + GroundM(XM, YM - E) - 4.f * H) / (float)(E * E);
			const float Grove = Smooth(Clump(XM, YM, 140.0, 5), 0.5f, 0.78f);
			P = FMath::Max(P, Veg * (1.f - Volc) * (1.f - 0.6f * Moor) * (0.04f + 0.45f * Grove * (0.35f + Smooth(Lap, 0.0f, 0.01f))));
		}
		P *= (1.f - Smooth(Slope, 0.46f, 0.88f)) * (1.f - Pad);
		// Salt wind keeps the woods off the shore; they thin out and shrink toward the tree line.
		P *= Smooth(H, 4.f, 16.f) * (1.f - Smooth(H, TreeLine * 0.75f, TreeLine)) * (1.f - Cold) * (1.f - Cold);
		OutScale = FMath::Lerp(0.6f, 1.f, Smooth(Forest, 0.1f, 0.6f)) * (1.f - 0.4f * Smooth(H, TreeLine * 0.5f, TreeLine));
		break;
	case EDragonScatterKind::Sapling:
		if (H < 4.f) return false;
		P = (Forest * (1.f - Forest) * 3.f + Veg * (1.f - Forest) * 0.12f) * (1.f - Smooth(Slope, 0.5f, 0.85f)) * (1.f - Pad);
		P *= (1.f - Smooth(H, TreeLine * 0.85f, TreeLine * 1.1f)) * (1.f - Cold);
		break;
	case EDragonScatterKind::DeadWood:
		if (H < 3.f) return false;
		P = Forest * 0.5f * (1.f - Smooth(Slope, 0.5f, 0.85f)) * (1.f - Smooth(H, TreeLine * 0.8f, TreeLine)) * (1.f - Cold);
		break;
	case EDragonScatterKind::Rock:
		if (H < -1.f) return false;
		// Boulders where rock really lies: scree under steep ground, and the foreshore of rocky
		// coasts. Not scattered over the turf.
		P = Smooth(Slope, 0.35f, 0.7f) * 0.6f * (1.f - Smooth(Slope, 0.85f, 1.f) * 0.5f)
			+ (H < 5.f ? 0.5f * Smooth(Slope, 0.08f, 0.25f) : 0.f)
			+ (Veg < 0.25f && H > 2.f ? 0.1f * Smooth(Slope, 0.2f, 0.4f) : 0.f)
			+ Snowy * 0.05f;
		// Bedrock breaking through the turf on knolls and ridge crests (convex ground).
		if (H > 8.f && Slope < 0.75f)
		{
			const double E = 14.0;
			const float Lap = (GroundM(XM + E, YM) + GroundM(XM - E, YM) + GroundM(XM, YM + E) + GroundM(XM, YM - E) - 4.f * H) / (float)(E * E);
			P += 0.35f * Smooth(-Lap, 0.004f, 0.02f);
		}
		P += Volc * 0.3f * Smooth(H, 2.f, 10.f);                     // lava boulders on the young isles
		// Glacial erratics and frost-heaved boulders lie all over the turf, thick in some fields
		// and few in others.
		if (H > 3.f && Slope < 0.7f)
		{
			Field = Smooth(Clump(XM, YM, 90.0, 17), 0.45f, 0.85f);
			P += (0.06f + 0.4f * Field) * (1.f - Snowy * 0.5f);
		}
		P *= 1.f - Forest * 0.6f;
		// boulder fields hold the big ones: erratics the size of a cottage among the small
		OutScale = (1.f + 0.4f * Smooth(Slope, 0.5f, 0.9f)) * (1.f + 1.6f * Field * FMath::Square(FMath::Frac(Rand * 7.31f)) + 0.6f * Field);
		break;
	case EDragonScatterKind::Cliff:
		if (H < -2.f) return false;
		// Rock faces only on the sheer ground: sea cliffs and crags.
		P = Smooth(Slope, 0.72f, 0.95f);
		OutScale = 1.f + 0.5f * Smooth(Slope, 0.75f, 0.98f);   // the sheerest faces get the biggest pieces
		break;
	case EDragonScatterKind::Stone:
		if (H < 1.f) return false;
		P = (0.12f + 0.6f * Smooth(Clump(XM, YM, 40.0, 19), 0.4f, 0.85f) + 0.4f * Smooth(Slope, 0.25f, 0.6f)) * (1.f - Smooth(Slope, 0.8f, 0.95f)) * (1.f - Pad * 0.5f);
		break;
	case EDragonScatterKind::Shrub:
		if (H < 2.f) return false;
		// Clumped: dense in patches, open turf between. Heather on the moors, few in the cold.
		P = Veg * (0.08f + 0.6f * Smooth(Clump(XM, YM, 60.0, 29), 0.45f, 0.82f)) * (1.f + 0.6f * Moor) * (1.f - Smooth(Slope, 0.55f, 0.8f))
			* (1.f - Pad) * (1.f - Snowy) * (1.f - 0.7f * Volc) * (1.f - 0.7f * Cold) * (1.f - 0.5f * Forest);
		break;
	case EDragonScatterKind::Ground:
		if (H < 1.5f) return false;
		P = Veg * (1.f - Forest * 0.4f) * (1.f - Smooth(Slope, 0.5f, 0.8f)) * (1.f - Pad * 0.7f) * (1.f - Snowy) * (1.f - 0.7f * Volc);
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

	StatTimer -= DeltaSeconds;
	if (StatTimer <= 0.f)
	{
		StatTimer = 5.f;
		TArray<int32> Inst; Inst.SetNumZeroed(ScatterLayers.Num());
		TArray<int32> NT; NT.SetNumZeroed(ScatterLayers.Num());
		for (const TPair<FTileKey, FTile>& T : Tiles)
		{
			++NT[T.Key.Layer];
			for (const UHierarchicalInstancedStaticMeshComponent* C : T.Value.Components) if (C) Inst[T.Key.Layer] += C->GetInstanceCount();
		}
		FString Msg;
		for (int32 i = 0; i < ScatterLayers.Num(); ++i) Msg += FString::Printf(TEXT(" %s %d tiles/%d inst (%d meshes);"), *ScatterLayers[i].Name.ToString(), NT[i], Inst[i], LayerMeshes[i].Num());
		UE_LOG(LogDragon, Display, TEXT("Scatter: queue %d;%s"), Queue.Num(), *Msg);
	}
}

bool ADragonScatter::IsBusy() const
{
	return Queue.Num() > 0;
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

			int32 MeshIdx = FMath::Min(Meshes.Num() - 1, (int32)(Pick * Meshes.Num()));
			// Wave-worn coast pieces belong on the shore; mossy boulders and crag faces inland.
			if (Li < ShoreMesh.Num() && ShoreMesh[Li].Num() == Meshes.Num())
			{
				const bool bShore = H < (L.Kind == EDragonScatterKind::Cliff ? 45.f : 6.f);
				TArray<int32, TInlineAllocator<16>> Ok;
				for (int32 m = 0; m < Meshes.Num(); ++m) if (ShoreMesh[Li][m] == bShore) Ok.Add(m);
				if (Ok.Num()) MeshIdx = Ok[FMath::Min(Ok.Num() - 1, (int32)(Pick * Ok.Num()))];
			}
			const float Scale = FMath::Lerp(L.ScaleRange.X, L.ScaleRange.Y, S01 * S01) * ScaleMul;
			FQuat Rot = FQuat(FVector::UpVector, FMath::DegreesToRadians(Yaw));
			if (L.AlignToSlope > 0.f)
			{
				const FVector N = GroundNormal(X, Y);
				const FVector Up = FMath::Lerp(FVector::UpVector, N, L.AlignToSlope).GetSafeNormal();
				Rot = FQuat::FindBetweenNormals(FVector::UpVector, Up) * Rot;
			}
			FVector Pos(X * M, Y * M, (H - L.SinkM * Scale) * M);
			if (L.Kind == EDragonScatterKind::Rock && H > 8.f && !(Li < ShoreMesh.Num() && ShoreMesh[Li].IsValidIndex(MeshIdx) && ShoreMesh[Li][MeshIdx]))
			{
				// Inland: set a third to a half of the stone into the ground, so it is bedrock
				// showing through, not something dropped on the grass.
				const FBoxSphereBounds B = Meshes[MeshIdx]->GetBounds();
				Pos.Z -= B.BoxExtent.Z * 2.0 * Scale * (0.15 + 0.2 * Rng.Next());
			}
			if (L.Kind == EDragonScatterKind::Iceberg)
			{
				// Most of a berg is under water: float it with ~30% of its height showing, tipped a little.
				const FBoxSphereBounds B = Meshes[MeshIdx]->GetBounds();
				const double Ht = B.BoxExtent.Z * 2.0 * Scale;
				Pos.Z = -(B.Origin.Z - B.BoxExtent.Z) * Scale - Ht * 0.7;
				Rot = FQuat(FVector(Rng.Next() - 0.5f, Rng.Next() - 0.5f, 0.f).GetSafeNormal(), FMath::DegreesToRadians(Rng.Next() * 25.f)) * Rot;
			}
			if (L.Kind == EDragonScatterKind::Cliff)
			{
				// Set the piece into the face: back along the horizontal normal by a third of its
				// width, so the hill swallows its back and only the weathered front stands proud.
				const FVector N = GroundNormal(X, Y);
				const FVector2D Out(N.X, N.Y);
				if (!Out.IsNearlyZero(0.05))
				{
					const FBoxSphereBounds B = Meshes[MeshIdx]->GetBounds();
					const double Half = FMath::Max(B.BoxExtent.X, B.BoxExtent.Y) * Scale;
					const FVector2D In = -Out.GetSafeNormal() * Half * 0.35;
					Pos.X += In.X; Pos.Y += In.Y;
				}
			}
			PerMesh[MeshIdx].Add(FTransform(Rot, Pos, FVector(Scale)));
		}
	}

	for (int32 m = 0; m < Meshes.Num(); ++m)
	{
		if (PerMesh[m].Num() == 0) continue;
		UHierarchicalInstancedStaticMeshComponent* C = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, NAME_None, RF_Transient);
		C->SetStaticMesh(Meshes[m]);
		if (L.Kind == EDragonScatterKind::Iceberg && IceMaterial)
			for (int32 k = 0; k < Meshes[m]->GetStaticMaterials().Num(); ++k) C->SetMaterial(k, IceMaterial);
		C->SetMobility(EComponentMobility::Movable); // created at runtime under a movable root
		C->SetCollisionEnabled(L.bCollide ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
		C->SetCastShadow(L.bCastShadow);
		// Only rocks and cliffs go into Lumen's distance fields: 50k trees in the SDF atlas cost
		// more GPU than their bounce light is worth.
		C->bAffectDistanceFieldLighting = L.Kind == EDragonScatterKind::Rock || L.Kind == EDragonScatterKind::Cliff || L.Kind == EDragonScatterKind::Iceberg;
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

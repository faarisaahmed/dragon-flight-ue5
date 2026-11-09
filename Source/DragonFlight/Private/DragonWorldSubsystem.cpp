#include "DragonWorldSubsystem.h"
#include "DragonFlight.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

void UDragonWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Load();
}

FString UDragonWorldSubsystem::DataDir()
{
	// Content/WorldData holds just what the game reads at runtime (the builder copies it there,
	// and packaging stages it); Data/World is the full bake, used by the tools and editor.
	static const FString Runtime = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("WorldData"));
	static const bool bRuntime = FPaths::FileExists(Runtime / TEXT("World.json"));
	return bRuntime ? Runtime : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data/World"));
}

bool UDragonWorldSubsystem::Load()
{
	if (bLoaded) return true;

	FString Text;
	const FString JsonPath = DataDir() / TEXT("World.json");
	if (!FFileHelper::LoadFileToString(Text, *JsonPath))
	{
		UE_LOG(LogDragon, Warning, TEXT("World.json not found at %s — run Tools/WorldBake or Tools/Setup."), *JsonPath);
		return false;
	}
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		UE_LOG(LogDragon, Error, TEXT("World.json failed to parse"));
		return false;
	}

	using namespace DragonUnits;
	TerrainSize = Json->GetNumberField(TEXT("terrainSize")) * M;
	SeaLevel = Json->GetNumberField(TEXT("seaLevel")) * M;

	if (const TSharedPtr<FJsonObject>* HM; Json->TryGetObjectField(TEXT("heightmap"), HM))
	{
		ZScalePercent = (*HM)->GetNumberField(TEXT("zScalePercent"));
	}
	if (const TSharedPtr<FJsonObject>* S; Json->TryGetObjectField(TEXT("spawn"), S))
	{
		Spawn = FromWeb((*S)->GetNumberField(TEXT("x")), (*S)->GetNumberField(TEXT("y")), (*S)->GetNumberField(TEXT("z")));
	}
	if (const TSharedPtr<FJsonObject>* H; Json->TryGetObjectField(TEXT("home"), H))
	{
		Home = FromWebXZ((*H)->GetNumberField(TEXT("x")), (*H)->GetNumberField(TEXT("z")));
	}

	for (const TSharedPtr<FJsonValue>& V : Json->GetArrayField(TEXT("islands")))
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FDragonIsland I;
		I.Name = O->GetStringField(TEXT("name"));
		I.Centre = FromWeb(O->GetNumberField(TEXT("x")), O->GetNumberField(TEXT("top")), O->GetNumberField(TEXT("z")));
		I.Radius = O->GetNumberField(TEXT("r")) * M;
		I.Peak = O->GetNumberField(TEXT("peak")) * M;
		Islands.Add(I);
	}

	for (const TSharedPtr<FJsonValue>& V : Json->GetArrayField(TEXT("rivers")))
	{
		FDragonRiver R;
		for (const TSharedPtr<FJsonValue>& P : V->AsObject()->GetArrayField(TEXT("pts")))
		{
			const TArray<TSharedPtr<FJsonValue>>& A = P->AsArray();
			if (A.Num() < 4) continue;
			R.Points.Add(FromWeb(A[0]->AsNumber(), A[2]->AsNumber(), A[1]->AsNumber()));
			R.Widths.Add(A[3]->AsNumber() * M);
		}
		if (R.Points.Num() >= 2) Rivers.Add(MoveTemp(R));
	}

	// Lakes of the final terrain (Tools/WorldBake/lakes.py) when the bake has them; the web
	// game's own lakes no longer fit the eroded ground.
	const TArray<TSharedPtr<FJsonValue>>* Lakes2 = nullptr;
	if (Json->TryGetArrayField(TEXT("lakes2"), Lakes2))
	{
		for (const TSharedPtr<FJsonValue>& V : *Lakes2)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			FDragonLake L;
			L.Level = O->GetNumberField(TEXT("level")) * M;
			L.Cell = O->GetNumberField(TEXT("cell")) * M;
			L.W = (int32)O->GetNumberField(TEXT("w")); L.H = (int32)O->GetNumberField(TEXT("h"));
			const double Half = TerrainSize * 0.5;
			const double X0 = O->GetNumberField(TEXT("i0")) * L.Cell - Half, Y0 = O->GetNumberField(TEXT("j0")) * L.Cell - Half;
			// Cell centres sit on the heightmap's vertices: a cell spans +-Cell/2 around them.
			L.Bounds = FBox2D(FVector2D(X0 - L.Cell * 0.5, Y0 - L.Cell * 0.5), FVector2D(X0 + (L.W - 0.5) * L.Cell, Y0 + (L.H - 0.5) * L.Cell));
			L.Wet.SetNumZeroed(L.W * L.H);
			int32 At = 0; uint8 Val = 0;
			for (const TSharedPtr<FJsonValue>& R : O->GetArrayField(TEXT("rle")))
			{
				const int32 Run = (int32)R->AsNumber();
				if (Val) for (int32 k = 0; k < Run && At + k < L.Wet.Num(); ++k) L.Wet[At + k] = 1;
				At += Run; Val ^= 1;
			}
			Lakes.Add(MoveTemp(L));
		}
	}
	else for (const TSharedPtr<FJsonValue>& V : Json->GetArrayField(TEXT("lakes")))
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FDragonLake L;
		L.Level = O->GetNumberField(TEXT("level")) * M;
		const double X0 = O->GetNumberField(TEXT("x0")), Z0 = O->GetNumberField(TEXT("z0"));
		const double Span = O->GetNumberField(TEXT("s")) * O->GetNumberField(TEXT("n"));
		L.Bounds = FBox2D(FVector2D(X0 * M, Z0 * M), FVector2D((X0 + Span) * M, (Z0 + Span) * M));
		Lakes.Add(L);
	}

	LoadHeightmap();
	bLoaded = true;
	UE_LOG(LogDragon, Log, TEXT("World loaded: %.1f km, %d islands, %d rivers, %d lakes, heightmap %d"),
		TerrainSize / 100000.0, Islands.Num(), Rivers.Num(), Lakes.Num(), HeightRes);
	return true;
}

bool UDragonWorldSubsystem::LoadHeightmap()
{
	// Use the same heightmap the Landscape was built from (the builder records it in
	// LandscapeRes.txt) so scattered trees and rocks sit exactly on the drawn ground.
	// Otherwise prefer 8129 if present, else the smallest.
	TArray<FString> Found;
	IFileManager::Get().FindFiles(Found, *(DataDir() / TEXT("Heightmap_*.r16")), true, false);
	int32 Want = 0;
	FString ResText;
	if (FFileHelper::LoadFileToString(ResText, *(DataDir() / TEXT("LandscapeRes.txt")))) Want = FCString::Atoi(*ResText.TrimStartAndEnd());
	TArray<int32> Sizes;
	for (const FString& F : Found)
	{
		const int32 N = FCString::Atoi(*F.Mid(10)); // "Heightmap_" is 10 chars
		if (N > 0) Sizes.Add(N);
	}
	if (Sizes.Num() == 0) return false;
	Sizes.Sort();
	int32 Best = Sizes[0];
	if (Want > 0 && Sizes.Contains(Want)) Best = Want;
	else if (Sizes.Contains(8129)) Best = 8129;
	const FString BestFile = FString::Printf(TEXT("Heightmap_%d.r16"), Best);

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *(DataDir() / BestFile))) return false;
	if (Bytes.Num() != Best * Best * 2) return false;
	Heights.SetNumUninitialized(Best * Best);
	FMemory::Memcpy(Heights.GetData(), Bytes.GetData(), Bytes.Num()); // little-endian on every target we ship
	HeightRes = Best;
	return true;
}

double UDragonWorldSubsystem::HeightAt(double X, double Y) const
{
	if (HeightRes < 2) return 0.0;
	const double Half = TerrainSize * 0.5;
	const double Step = TerrainSize / (HeightRes - 1);
	const double FX = FMath::Clamp((X + Half) / Step, 0.0, HeightRes - 1.001);
	const double FY = FMath::Clamp((Y + Half) / Step, 0.0, HeightRes - 1.001);
	const int32 IX = (int32)FX, IY = (int32)FY;
	const double TX = FX - IX, TY = FY - IY;
	auto H = [&](int32 I, int32 J) { return (double)Heights[J * HeightRes + I]; };
	const double V = FMath::Lerp(FMath::Lerp(H(IX, IY), H(IX + 1, IY), TX), FMath::Lerp(H(IX, IY + 1), H(IX + 1, IY + 1), TX), TY);
	return (V - 32768.0) / 128.0 * ZScalePercent;
}

float UDragonWorldSubsystem::ColdAt(double X, double Y) const
{
	// North is UE -Y. Temperate up to ~9 km north of the middle, arctic at the northern edge, with a
	// wobble so the line between them follows no parallel.
	const double Ym = Y / 100.0, Xm = X / 100.0;
	const double Wobble = 1800.0 * FMath::Sin(Xm / 5200.0 + 0.7) + 900.0 * FMath::Sin(Xm / 2100.0 + 2.1);
	const float T = (float)((-Ym - 9000.0 + Wobble) / 9000.0);
	const float C = FMath::Clamp(T, 0.f, 1.f);
	return C * C * (3.f - 2.f * C);
}

double UDragonWorldSubsystem::SnowLineAt(double X, double Y) const
{
	return FMath::Lerp(480.0, 40.0, (double)ColdAt(X, Y)) * 100.0;
}

FString UDragonWorldSubsystem::NearestIsland(const FVector& Where, double& OutDistance) const
{
	OutDistance = TNumericLimits<double>::Max();
	FString Name;
	for (const FDragonIsland& I : Islands)
	{
		// Distance to the island's rough edge, so standing on a big island beats being near a small one's centre.
		const double D = FVector2D::Distance(FVector2D(Where), FVector2D(I.Centre)) - I.Radius;
		if (D < OutDistance) { OutDistance = D; Name = I.Name; }
	}
	return Name;
}

bool UDragonWorldSubsystem::IsInLake(double X, double Y, double GroundZ, double Margin) const
{
	for (const FDragonLake& L : Lakes)
	{
		if (L.Wet.Num() != L.W * L.H || L.W <= 0 || GroundZ > L.Level + Margin) continue;
		if (X < L.Bounds.Min.X - L.Cell || X > L.Bounds.Max.X + L.Cell || Y < L.Bounds.Min.Y - L.Cell || Y > L.Bounds.Max.Y + L.Cell) continue;
		// The surface overhangs each wet cell by 3/4 of a cell (ADragonSea::BuildLake): check the neighbours too.
		const int32 cx = FMath::FloorToInt32((X - L.Bounds.Min.X) / L.Cell), cy = FMath::FloorToInt32((Y - L.Bounds.Min.Y) / L.Cell);
		for (int32 dy = -1; dy <= 1; ++dy)
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				const int32 x = cx + dx, y = cy + dy;
				if (x >= 0 && y >= 0 && x < L.W && y < L.H && L.Wet[y * L.W + x]) return true;
			}
	}
	return false;
}

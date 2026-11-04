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

	for (const TSharedPtr<FJsonValue>& V : Json->GetArrayField(TEXT("lakes")))
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

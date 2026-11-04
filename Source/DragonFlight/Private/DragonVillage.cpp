#include "DragonVillage.h"
#include "DragonFlight.h"
#include "DragonWorldSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"

ADragonVillage::ADragonVillage()
{
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
}

TMap<FString, TArray<UStaticMesh*>> ADragonVillage::LoadKit() const
{
	TMap<FString, TArray<UStaticMesh*>> Kit;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Found;
	Registry.GetAssetsByPath(FName(*KitPath), Found, true);
	for (const FAssetData& A : Found)
	{
		if (A.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName()) continue;
		const FString Name = A.AssetName.ToString();
		if (Name.Contains(TEXT("_LOD")) && !Name.EndsWith(TEXT("_LOD0"))) continue;
		FString Kind = Name, Rest;
		Name.Split(TEXT("_"), &Kind, &Rest);
		Kind = Kind.ToLower();
		if (UStaticMesh* M = Cast<UStaticMesh>(A.GetAsset())) Kit.FindOrAdd(Kind).Add(M);
	}
	return Kit;
}

void ADragonVillage::BeginPlay()
{
	Super::BeginPlay();
	const UDragonWorldSubsystem* W = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	const TSharedPtr<FJsonObject> J = W ? W->GetJson() : nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Plots = nullptr;
	if (!J || !J->TryGetArrayField(TEXT("village"), Plots)) { UE_LOG(LogDragon, Log, TEXT("Village: no plots")); return; }

	const TMap<FString, TArray<UStaticMesh*>> Kit = LoadKit();
	if (Kit.Num() == 0) { UE_LOG(LogDragon, Log, TEXT("Village: no kit under %s"), *KitPath); return; }
	// Kinds the kit might not have yet fall back to the nearest thing it does.
	auto Resolve = [&](const FString& Kind) -> const TArray<UStaticMesh*>*
	{
		static const TMap<FString, TArray<FString>> Fallback = {
			{ TEXT("stilt"), { TEXT("cottage") } }, { TEXT("tall"), { TEXT("cottage") } }, { TEXT("turf"), { TEXT("cottage") } },
			{ TEXT("hallhouse"), { TEXT("longhouse") } }, { TEXT("longhouse"), { TEXT("hallhouse"), TEXT("cottage") } },
			{ TEXT("shed"), { TEXT("cottage") } }, { TEXT("cottage"), { TEXT("turf"), TEXT("longhouse") } } };
		if (const TArray<UStaticMesh*>* M = Kit.Find(Kind)) return M;
		if (const TArray<FString>* F = Fallback.Find(Kind))
			for (const FString& K : *F) if (const TArray<UStaticMesh*>* M = Kit.Find(K)) return M;
		return nullptr;
	};

	TMap<UStaticMesh*, TArray<FTransform>> ByMesh;
	int32 Index = 0;
	for (const TSharedPtr<FJsonValue>& V : *Plots)
	{
		const TSharedPtr<FJsonObject> P = V->AsObject();
		if (!P) continue;
		const FString Kind = P->GetStringField(TEXT("kind")).ToLower();
		const TArray<UStaticMesh*>* Meshes = Resolve(Kind);
		++Index;
		if (!Meshes || Meshes->Num() == 0) continue;
		// web (x, y-up, z) metres -> UE (X = x, Y = z, Z = y) centimetres; the axis swap mirrors,
		// so a web yaw (about +Y, counter-clockwise from above) is a negative UE yaw.
		const double X = P->GetNumberField(TEXT("x")) * 100.0, Y = P->GetNumberField(TEXT("z")) * 100.0;
		const double H = P->GetNumberField(TEXT("h"));
		const double YawDeg = -FMath::RadiansToDegrees(P->GetNumberField(TEXT("yaw"))) + YawOffsetDeg;
		double S = 1.0; P->TryGetNumberField(TEXT("s"), S);
		UStaticMesh* Mesh = (*Meshes)[(Index * 7919) % Meshes->Num()];
		ByMesh.FindOrAdd(Mesh).Add(FTransform(FRotator(0.0, YawDeg, 0.0), FVector(X, Y, (H - SinkM) * 100.0), FVector(S)));
	}

	for (TPair<UStaticMesh*, TArray<FTransform>>& It : ByMesh)
	{
		UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(this, NAME_None);
		C->SetStaticMesh(It.Key);
		C->SetMobility(EComponentMobility::Movable);
		C->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		C->SetupAttachment(RootComponent);
		C->RegisterComponent();
		C->AddInstances(It.Value, false, true);
		Parts.Add(C);
		Placed += It.Value.Num();
	}
	UE_LOG(LogDragon, Display, TEXT("Village: %d buildings from %d plots, %d kinds in the kit"), Placed, Plots->Num(), Kit.Num());
}

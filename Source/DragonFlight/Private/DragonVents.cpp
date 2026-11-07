#include "DragonVents.h"
#include "DragonFlight.h"
#include "DragonFX.h"
#include "DragonWorldSubsystem.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"

ADragonVents::ADragonVents()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ADragonVents::BeginPlay()
{
	Super::BeginPlay();
	const UDragonWorldSubsystem* W = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	const TSharedPtr<FJsonObject> J = W ? W->GetJson() : nullptr;
	const TSharedPtr<FJsonObject>* Bio = nullptr;
	if (!J || !J->TryGetObjectField(TEXT("biomes"), Bio)) return;
	for (const FDragonIsland& I : W->GetIslands())
	{
		FString B;
		if (!(*Bio)->TryGetStringField(I.Name, B) || B != TEXT("volcanic")) continue;
		// A dozen-odd vents on gentle ground above the shore, clustered in two or three fields.
		const int32 Fields = 3;
		for (int32 f = 0; f < Fields; ++f)
		{
			const float A = Rand() * 2.f * PI, R = (0.15f + 0.5f * Rand()) * (float)I.Radius;
			const FVector C(I.Centre.X + FMath::Cos(A) * R, I.Centre.Y + FMath::Sin(A) * R, 0.0);
			for (int32 k = 0; k < 6; ++k)
			{
				FVector P = C + FVector(Rand() - 0.5f, Rand() - 0.5f, 0.f) * 40000.f;
				const double G = W->HeightAt(P.X, P.Y);
				const double Gx = W->HeightAt(P.X + 600.0, P.Y), Gy = W->HeightAt(P.X, P.Y + 600.0);
				if (G < 800.0 || FMath::Abs(Gx - G) > 400.0 || FMath::Abs(Gy - G) > 400.0) continue;
				P.Z = G;
				Vents.Add({ P, 0.4f + 0.6f * Rand(), Rand() });
			}
		}
	}
	UE_LOG(LogDragon, Display, TEXT("Vents: %d fumaroles"), Vents.Num());
}

void ADragonVents::Tick(float Dt)
{
	Super::Tick(Dt);
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager || Vents.Num() == 0) return;
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	ADragonFX* FX = nullptr;
	for (FVent& V : Vents)
	{
		if (FVector::DistSquared(V.At, Cam) > FMath::Square(450000.f)) continue;
		V.Timer -= Dt;
		if (V.Timer > 0.f) continue;
		V.Timer = 0.35f + 0.3f * Rand();
		if (!FX) FX = ADragonFX::Get(this);
		if (!FX) return;
		FDragonPuff P;
		P.Kind = EDragonPuff::Smoke;
		P.Pos = V.At + FVector(Rand() - 0.5f, Rand() - 0.5f, 0.f) * 300.f;
		P.Vel = FVector(0, 0, 250.f + 250.f * V.Strength);
		P.Drag = 0.25f; P.Buoyancy = 60.f;
		P.Life = 7.f + 4.f * Rand();
		P.Size0 = 300.f; P.Size1 = 1600.f + 1400.f * V.Strength;
		P.Colour0 = FLinearColor(0.92f, 0.93f, 0.95f); P.Colour1 = FLinearColor(0.85f, 0.87f, 0.9f);
		P.Intensity = 0.32f * V.Strength; P.FadeIn = 0.15f; P.Spin = Rand() - 0.5f;
		FX->Spawn(P);
	}
}

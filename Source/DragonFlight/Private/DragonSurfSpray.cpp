#include "DragonSurfSpray.h"
#include "DragonFlight.h"
#include "DragonFX.h"
#include "DragonSky.h"
#include "DragonWorldSubsystem.h"
#include "EngineUtils.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"

namespace { constexpr float RangeCm = 90000.f; }   // spots within 900 m of the camera

ADragonSurfSpray::ADragonSurfSpray()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ADragonSurfSpray::Search(const FVector& Cam)
{
	const UDragonWorldSubsystem* W = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	if (!W) return;
	// Keep the spots still near; look for new ones along random rays out from under the camera.
	Spots.RemoveAll([&](const FSpot& S) { return FVector::Dist2D(S.At, Cam) > RangeCm * 1.2f; });
	for (int32 Ray = 0; Ray < 48 && Spots.Num() < 36; ++Ray)
	{
		const float A = Rand() * 2.f * PI;
		const FVector D(FMath::Cos(A), FMath::Sin(A), 0.f);
		double Prev = W->HeightAt(Cam.X, Cam.Y);
		for (float R = 1000.f; R < RangeCm; R += 800.f)
		{
			const FVector P = FVector(Cam.X, Cam.Y, 0.0) + D * R;
			const double H = W->HeightAt(P.X, P.Y);
			if ((Prev < 0.0) != (H < 0.0))
			{
				// The shoreline: which way is the sea, and is it a cliff (high ground just behind,
				// deep water just off) rather than a beach?
				const FVector ToSea = H < 0.0 ? D : -D;
				FVector Shore = P - D * 400.f;
				for (int32 k = 0; k < 6; ++k) { const FVector Q = Shore + D * 133.f * k; if ((W->HeightAt(Q.X, Q.Y) < 0.0) == (H < 0.0)) { Shore = Q; break; } }
				const double Land = W->HeightAt(Shore.X - ToSea.X * 1500.f, Shore.Y - ToSea.Y * 1500.f);
				const double Sea = W->HeightAt(Shore.X + ToSea.X * 1500.f, Shore.Y + ToSea.Y * 1500.f);
				if (Land > 600.0 && Sea < -400.0)
				{
					bool bNear = false;
					for (const FSpot& S : Spots) if (FVector::DistSquared2D(S.At, Shore) < FMath::Square(4000.f)) { bNear = true; break; }
					if (!bNear)
						Spots.Add({ FVector(Shore.X, Shore.Y, 0.f), ToSea, 1.f + 6.f * Rand(), FMath::Clamp((float)(Land / 2500.0), 0.4f, 1.f) });
				}
				break;
			}
			Prev = H;
		}
	}
}

void ADragonSurfSpray::Burst(const FSpot& S, float Sea)
{
	ADragonFX* FX = ADragonFX::Get(this);
	if (!FX) return;
	const float K = S.Size * (0.5f + Sea);
	// The sheet of white water thrown up the face and falling back.
	const int32 N = FMath::RoundToInt(8 + 10 * Sea);
	for (int32 i = 0; i < N; ++i)
	{
		FDragonPuff P;
		P.Kind = EDragonPuff::Spray;
		const FVector Along(-S.Out.Y, S.Out.X, 0.f);
		P.Pos = S.At + Along * (Rand() - 0.5f) * 1800.f * K + S.Out * 150.f + FVector(0, 0, 50.f);
		P.Vel = FVector(0, 0, (700.f + 1100.f * Rand()) * K) + S.Out * (100.f + 300.f * Rand()) + Along * (Rand() - 0.5f) * 300.f;
		P.Drag = 0.5f; P.Gravity = 980.f;
		P.Life = 1.4f + 1.2f * Rand();
		P.Size0 = 250.f * K; P.Size1 = (700.f + 600.f * Rand()) * K; P.Stretch = 0.002f;
		P.Colour0 = FLinearColor(0.95f, 0.97f, 1.f); P.Colour1 = FLinearColor(0.88f, 0.9f, 0.93f);
		P.Intensity = 0.75f; P.FadeIn = 0.05f;
		FX->Spawn(P);
	}
	// Salt mist that hangs and blows off the rocks.
	for (int32 i = 0; i < 3; ++i)
	{
		FDragonPuff P;
		P.Kind = EDragonPuff::Smoke;
		P.Pos = S.At + S.Out * (200.f + 400.f * Rand()) + FVector(0, 0, 300.f + 500.f * Rand());
		P.Vel = FVector(0, 0, 80.f) + S.Out * 150.f;
		P.Drag = 0.4f; P.Buoyancy = 20.f;
		P.Life = 4.f + 3.f * Rand();
		P.Size0 = 600.f * K; P.Size1 = 2200.f * K;
		P.Colour0 = FLinearColor(0.9f, 0.92f, 0.95f); P.Colour1 = FLinearColor(0.85f, 0.88f, 0.92f);
		P.Intensity = 0.22f; P.FadeIn = 0.1f; P.Spin = Rand() - 0.5f;
		FX->Spawn(P);
	}
}

void ADragonSurfSpray::Tick(float Dt)
{
	Super::Tick(Dt);
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager) return;
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	SearchTimer -= Dt;
	if (SearchTimer <= 0.f || FVector::Dist2D(Cam, LastSearch) > 20000.f)
	{
		SearchTimer = 2.f;
		LastSearch = Cam;
		Search(Cam);
	}
	// High above the sea you don't see the spray; it starts when you come within ~600 m of it.
	float Sea = 0.6f;
	TActorIterator<ADragonSky> It(GetWorld());
	if (It) Sea = FMath::Clamp((It->GetWeatherState().Wind - 0.6f) / 1.6f, 0.15f, 1.f);
	for (FSpot& S : Spots)
	{
		S.Timer -= Dt;
		if (S.Timer > 0.f) continue;
		// Sets of swell: every 5-12 s, quicker in a storm.
		S.Timer = FMath::Lerp(12.f, 5.f, Sea) * (0.6f + 0.8f * Rand());
		if (FVector::Dist(FVector(S.At.X, S.At.Y, 0.f), Cam) > 65000.f) continue;
		Burst(S, Sea);
	}
}

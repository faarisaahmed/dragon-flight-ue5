#include "DragonFX.h"
#include "DragonFlight.h"
#include "DragonSky.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/DecalComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	const TCHAR* GlowPath = TEXT("/Game/World/Materials/M_FxGlow.M_FxGlow");
	const TCHAR* SmokePath = TEXT("/Game/World/Materials/M_FxSmoke.M_FxSmoke");
	const TCHAR* CorePath = TEXT("/Game/World/Materials/M_PlasmaCore.M_PlasmaCore");
	const TCHAR* ScorchPath = TEXT("/Game/World/Materials/M_Scorch.M_Scorch");

	// Plasma: a white-hot heart in a violet-blue sheath (the colours of an electric arc).
	const FLinearColor PlasmaHot(0.85f, 0.9f, 1.f);
	const FLinearColor PlasmaBlue(0.32f, 0.42f, 1.f);
	const FLinearColor PlasmaViolet(0.55f, 0.28f, 1.f);
	const FLinearColor FireOrange(1.f, 0.42f, 0.10f);
	const FLinearColor FireDeep(0.75f, 0.16f, 0.03f);
}

ADragonFX::ADragonFX()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;   // after the camera has moved
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

ADragonFX* ADragonFX::Get(const UObject* WorldContext)
{
	UWorld* W = WorldContext ? WorldContext->GetWorld() : nullptr;
	if (!W) return nullptr;
	TActorIterator<ADragonFX> It(W);
	if (It) return *It;
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return W->SpawnActor<ADragonFX>(ADragonFX::StaticClass(), FTransform::Identity, SP);
}

void ADragonFX::BeginPlay()
{
	Super::BeginPlay();
	Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	GlowMat = LoadObject<UMaterialInterface>(nullptr, GlowPath);
	SmokeMat = LoadObject<UMaterialInterface>(nullptr, SmokePath);
	CoreMat = LoadObject<UMaterialInterface>(nullptr, CorePath);
	ScorchMat = LoadObject<UMaterialInterface>(nullptr, ScorchPath);
	if (!GlowMat) UE_LOG(LogDragon, Warning, TEXT("FX: %s missing - run the builder (materials)"), GlowPath);
	Seed = (uint32)(FPlatformTime::Cycles() | 1);
}

int32 ADragonFX::Acquire(EDragonPuff Kind)
{
	// Reuse a free slot of the same kind (its material instance fits), else make one.
	for (int32 i = Free.Num() - 1; i >= 0; --i)
	{
		const int32 S = Free[i];
		if (SlotKind[S] == (uint8)Kind) { Free.RemoveAtSwap(i); return S; }
	}
	if (!Plane) return -1;
	UMaterialInterface* Base = Kind == EDragonPuff::Glow ? GlowMat.Get() : SmokeMat.Get();
	if (!Base) return -1;
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
	C->SetStaticMesh(Plane);
	C->SetMobility(EComponentMobility::Movable);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCastShadow(false);
	C->bAffectDistanceFieldLighting = false;
	C->SetAbsolute(true, true, true);
	C->SetupAttachment(RootComponent);
	C->RegisterComponent();
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
	C->SetMaterial(0, MID);
	Slots.Add(C); SlotMIDs.Add(MID); SlotKind.Add((uint8)Kind);
	return Slots.Num() - 1;
}

void ADragonFX::Release(int32 Slot)
{
	if (!Slots.IsValidIndex(Slot)) return;
	Slots[Slot]->SetVisibility(false);
	Free.Add(Slot);
}

void ADragonFX::Spawn(const FDragonPuff& P)
{
	if (Live.Num() >= MaxPuffs)
	{
		// Make room: the oldest (by share of life) goes.
		int32 Oldest = 0; float Most = -1.f;
		for (int32 i = 0; i < Live.Num(); ++i) { const float A = Live[i].P.Age / Live[i].P.Life; if (A > Most) { Most = A; Oldest = i; } }
		Release(Live[Oldest].Slot);
		Live.RemoveAtSwap(Oldest);
	}
	const int32 S = Acquire(P.Kind);
	if (S < 0) return;
	FLive L; L.P = P; L.Slot = S;
	if (SlotMIDs[S]) SlotMIDs[S]->SetScalarParameterValue(TEXT("Seed"), Rand() * 100.f);
	Live.Add(L);
}

void ADragonFX::Tick(float Dt)
{
	Super::Tick(Dt);
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager) return;
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	const FVector CamUp = PC->PlayerCameraManager->GetCameraRotation().Quaternion().GetUpVector();
	FVector Wind = FVector::ZeroVector;
	float SkyLight = 1.f;
	if (const ADragonSky* Sky = ADragonSky::Get(this))
	{
		Wind = Sky->GetWind();
		// Smoke is lit by the sky: grey by day, near black at night.
		SkyLight = FMath::Clamp((Sky->GetSunElevation() + 6.f) / 18.f, 0.08f, 1.f) * (1.f - 0.5f * Sky->GetWeatherState().SunDim);
	}

	for (int32 i = Live.Num() - 1; i >= 0; --i)
	{
		FDragonPuff& P = Live[i].P;
		P.Age += Dt;
		if (P.Age >= P.Life) { Release(Live[i].Slot); Live.RemoveAtSwap(i); continue; }
		const float T = P.Age / P.Life;
		P.Vel *= FMath::Exp(-P.Drag * Dt);
		P.Vel.Z += (P.Buoyancy - P.Gravity) * Dt;
		// Smoke rides the wind once it has slowed.
		if (P.Kind == EDragonPuff::Smoke) P.Vel += (Wind - P.Vel) * FMath::Min(1.f, 0.4f * Dt) * FMath::Clamp(T * 3.f, 0.f, 1.f);
		P.Pos += P.Vel * Dt;

		const float Size = FMath::Lerp(P.Size0, P.Size1, FMath::Pow(T, P.GrowPow));
		const float In = P.FadeIn > 0.f ? FMath::Clamp(T / P.FadeIn, 0.f, 1.f) : 1.f;
		const float Out = 1.f - FMath::SmoothStep(0.45f, 1.f, T);
		const float Fade = In * Out * P.Intensity;
		FLinearColor C = FMath::Lerp(P.Colour0, P.Colour1, T);
		if (P.Kind != EDragonPuff::Glow) C = C * SkyLight;

		UStaticMeshComponent* Comp = Slots[Live[i].Slot];
		// Face the camera; streaks lie along their velocity.
		const FVector ToCam = (Cam - P.Pos).GetSafeNormal();
		FVector Along = P.LengthCm > 0.f ? P.Axis : P.Stretch > 0.f && !P.Vel.IsNearlyZero() ? P.Vel.GetSafeNormal() : CamUp.RotateAngleAxis(P.Spin * P.Age * 57.3f, ToCam);
		const FMatrix R = FRotationMatrix::MakeFromZX(ToCam, Along);
		const float Len = P.LengthCm > 0.f ? P.LengthCm : P.Stretch > 0.f ? FMath::Max(Size, Size + P.Vel.Size() * P.Stretch) : Size;
		// The engine plane is 100 cm square.
		Comp->SetWorldTransform(FTransform(R.ToQuat(), P.Pos, FVector(Len / 100.f, Size / 100.f, 1.f)));
		Comp->SetVisibility(true);
		if (UMaterialInstanceDynamic* MID = SlotMIDs[Live[i].Slot])
		{
			MID->SetVectorParameterValue(TEXT("Colour"), C);
			MID->SetScalarParameterValue(TEXT("Fade"), Fade);
			MID->SetScalarParameterValue(TEXT("Age"), T);
		}
	}

	for (int32 i = Flashes.Num() - 1; i >= 0; --i)
	{
		FFlash& F = Flashes[i];
		F.Age += Dt;
		const float T = F.Age / F.Life;
		if (T >= 1.f || !F.Light) { if (F.Light) { F.Light->SetVisibility(false); LightPool.Add(F.Light); } Flashes.RemoveAtSwap(i); continue; }
		// A sharp peak, then a long orange tail, with the flicker of fire.
		const float Env = FMath::Exp(-T * 6.f) * 0.85f + (1.f - T) * 0.15f;
		const float Flick = 0.85f + 0.15f * FMath::Sin(F.Age * 47.f) * FMath::Sin(F.Age * 13.f + 1.f);
		F.Light->SetIntensity(F.Peak * Env * Flick);
		F.Light->SetLightColor(FMath::Lerp(F.C0, F.C1, FMath::Clamp(T * 2.5f, 0.f, 1.f)));
	}

	for (int32 i = Marks.Num() - 1; i >= 0; --i)
	{
		FMark& K = Marks[i];
		K.Age += Dt;
		if (!K.Decal || K.Age > 75.f) { if (K.Decal) K.Decal->DestroyComponent(); Marks.RemoveAtSwap(i); continue; }
		if (K.MID)
		{
			K.MID->SetScalarParameterValue(TEXT("Fade"), 1.f - FMath::SmoothStep(45.f, 75.f, K.Age));
			K.MID->SetScalarParameterValue(TEXT("Glow"), FMath::Exp(-K.Age * 0.35f));   // embers cooling
		}
	}
}

void ADragonFX::Flash(const FVector& At, float Candelas, float RadiusCm, float Life, const FLinearColor& C0, const FLinearColor& C1)
{
	UPointLightComponent* L = nullptr;
	if (LightPool.Num()) L = LightPool.Pop();
	else
	{
		L = NewObject<UPointLightComponent>(this, NAME_None, RF_Transient);
		L->SetMobility(EComponentMobility::Movable);
		L->SetIntensityUnits(ELightUnits::Candelas);
		L->SetCastShadows(false);
		L->SetAbsolute(true, true, true);
		L->SetupAttachment(RootComponent);
		L->RegisterComponent();
	}
	if (Flashes.Num() > 6) { Flashes[0].Life = 0.f; }   // keep the light count small
	L->SetWorldLocation(At);
	L->SetAttenuationRadius(RadiusCm);
	L->SetSourceRadius(RadiusCm * 0.02f);
	L->SetLightColor(C0);
	L->SetIntensity(Candelas);
	L->SetVisibility(true);
	FFlash F; F.Light = L; F.Life = Life; F.Peak = Candelas; F.C0 = C0; F.C1 = C1;
	Flashes.Add(F);
}

void ADragonFX::Scorch(const FVector& At, const FVector& Normal, float RadiusCm)
{
	if (!ScorchMat) return;
	UDecalComponent* D = NewObject<UDecalComponent>(this, NAME_None, RF_Transient);
	D->SetAbsolute(true, true, true);
	D->SetupAttachment(RootComponent);
	D->RegisterComponent();
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(ScorchMat, this);
	MID->SetScalarParameterValue(TEXT("Seed"), Rand() * 100.f);
	D->SetDecalMaterial(MID);
	// A decal projects along its X axis: point it into the ground.
	D->SetWorldLocationAndRotation(At, (-Normal).Rotation() + FRotator(0.f, 0.f, Rand() * 360.f));
	D->DecalSize = FVector(RadiusCm * 0.6f, RadiusCm, RadiusCm);
	D->SetFadeScreenSize(0.002f);
	if (Marks.Num() > 24) { Marks[0].Age = 1000.f; }
	FMark K; K.Decal = D; K.MID = MID;
	Marks.Add(K);
}

void ADragonFX::PlasmaMuzzle(const FVector& At, const FVector& Dir)
{
	for (int32 k = 0; k < 3; ++k)
	{
		FDragonPuff P;
		P.Kind = EDragonPuff::Glow;
		P.Pos = At + Dir * (40.f + k * 70.f);
		P.Vel = Dir * (900.f + k * 400.f) + RandDir() * 150.f;
		P.Drag = 6.f; P.Life = 0.12f + 0.05f * k;
		P.Size0 = 260.f - k * 50.f; P.Size1 = 420.f;
		P.Colour0 = k == 0 ? PlasmaHot * 6.f : PlasmaBlue * 4.f; P.Colour1 = PlasmaViolet * 1.5f;
		P.FadeIn = 0.f;
		Spawn(P);
	}
	Flash(At + Dir * 100.f, 60000.f, 3000.f, 0.15f, PlasmaHot, PlasmaViolet);
}

void ADragonFX::PlasmaTrail(const FVector& From, const FVector& To, const FVector& Vel)
{
	// The wake: overlapping streaks laid along the path, so it reads as one glowing ribbon
	// that thins and fades behind the bolt, with a softer violet haze around it.
	const FVector D = To - From;
	const float Len = D.Size();
	if (Len < 1.f) return;
	const FVector Dir = D / Len;
	const int32 N = FMath::Clamp(FMath::CeilToInt(Len / 600.f), 1, 12);
	for (int32 i = 0; i < N; ++i)
	{
		const float A0 = (float)i / N, A1 = (i + 1.f) / N;
		FDragonPuff P;
		P.Kind = EDragonPuff::Glow;
		P.Pos = FMath::Lerp(From, To, (A0 + A1) * 0.5f);
		P.Axis = Dir; P.LengthCm = Len / N * 1.6f;
		P.Life = 0.35f + Rand() * 0.1f;
		P.Size0 = 110.f; P.Size1 = 30.f; P.GrowPow = 0.8f;
		P.Colour0 = FMath::Lerp(PlasmaBlue, PlasmaViolet, 0.35f + 0.3f * Rand()) * 3.f; P.Colour1 = PlasmaViolet * 0.5f;
		P.FadeIn = 0.f;
		Spawn(P);
		FDragonPuff H = P;
		H.Size0 = 320.f; H.Size1 = 160.f; H.Life = 0.5f; H.LengthCm = Len / N * 1.8f;
		H.Colour0 = PlasmaViolet * 0.7f; H.Colour1 = PlasmaViolet * 0.1f;
		Spawn(H);
	}
	// The odd spark thrown off the bolt.
	if (Rand() < 0.5f)
	{
		FDragonPuff S;
		S.Kind = EDragonPuff::Glow;
		S.Pos = To; S.Vel = Vel * 0.1f + RandDir() * 1200.f; S.Drag = 2.f; S.Gravity = 300.f;
		S.Life = 0.3f + Rand() * 0.3f; S.Size0 = 26.f; S.Size1 = 10.f; S.Stretch = 0.03f;
		S.Colour0 = PlasmaHot * 6.f; S.Colour1 = PlasmaBlue * 2.f; S.FadeIn = 0.f;
		Spawn(S);
	}
}

void ADragonFX::PlasmaBlast(const FVector& At, const FVector& Normal, bool bWater, float RadiusCm)
{
	const FVector Up = Normal.IsNearlyZero() ? FVector::UpVector : Normal.GetSafeNormal();
	const float R = RadiusCm;

	// 1. The flash: blue-white, then the orange of what is burning.
	Flash(At + Up * 300.f, 2.5e6f, R * 9.f, 1.6f, FLinearColor(0.7f, 0.75f, 1.f), FireOrange);

	// 2. The plasma ball: a white core bursting into a violet shell.
	for (int32 k = 0; k < 6; ++k)
	{
		FDragonPuff P;
		P.Kind = EDragonPuff::Glow;
		P.Pos = At + Up * (R * 0.15f) + RandDir() * R * 0.08f;
		P.Vel = RandDir() * R * 0.6f;
		P.Drag = 5.f;
		P.Life = 0.28f + 0.1f * Rand();
		P.Size0 = R * 0.4f; P.Size1 = R * (1.3f + 0.4f * Rand()); P.GrowPow = 0.35f;
		P.Colour0 = (k < 2 ? PlasmaHot * 5.f : FMath::Lerp(PlasmaBlue, PlasmaViolet, Rand()) * 4.f); P.Colour1 = PlasmaViolet * 1.2f;
		P.FadeIn = 0.f;
		Spawn(P);
	}
	// 3. Plasma tongues thrown outward.
	for (int32 k = 0; k < 14; ++k)
	{
		FDragonPuff P;
		P.Kind = EDragonPuff::Glow;
		const FVector D = (RandDir() + Up * 0.7f).GetSafeNormal();
		P.Pos = At + D * R * 0.1f;
		P.Vel = D * R * (2.5f + 2.f * Rand());
		P.Drag = 4.5f; P.Gravity = 200.f;
		P.Life = 0.35f + 0.3f * Rand();
		P.Size0 = R * 0.25f; P.Size1 = R * 0.6f; P.Stretch = 0.012f;
		P.Colour0 = FMath::Lerp(PlasmaBlue, PlasmaViolet, Rand()) * 3.5f; P.Colour1 = FireOrange * 1.8f;
		P.FadeIn = 0.f;
		Spawn(P);
	}

	if (bWater)
	{
		// Spray thrown up and falling back, and a cloud of steam that drifts away.
		for (int32 k = 0; k < 26; ++k)
		{
			FDragonPuff P;
			P.Kind = EDragonPuff::Spray;
			const FVector D = (FVector(FMath::Cos(Rand() * 6.28f), FMath::Sin(Rand() * 6.28f), 0.f) * (0.2f + 0.5f * Rand()) + FVector(0, 0, 1)).GetSafeNormal();
			P.Pos = At + D * R * 0.1f;
			P.Vel = D * R * (1.6f + 1.6f * Rand());
			P.Drag = 0.6f; P.Gravity = 980.f;
			P.Life = 1.2f + 1.0f * Rand();
			P.Size0 = R * 0.2f; P.Size1 = R * 0.7f; P.Stretch = 0.004f;
			P.Colour0 = FLinearColor(0.95f, 0.97f, 1.f); P.Colour1 = FLinearColor(0.9f, 0.92f, 0.95f);
			P.Intensity = 0.8f; P.FadeIn = 0.02f;
			Spawn(P);
		}
		for (int32 k = 0; k < 10; ++k)
		{
			FDragonPuff P;
			P.Kind = EDragonPuff::Smoke;
			P.Pos = At + FVector(RandDir().X, RandDir().Y, 0.2f) * R * 0.3f;
			P.Vel = FVector(0, 0, 300.f + 300.f * Rand()) + RandDir() * 200.f;
			P.Drag = 0.8f; P.Buoyancy = 120.f;
			P.Life = 3.f + 2.f * Rand();
			P.Size0 = R * 0.5f; P.Size1 = R * (1.6f + Rand());
			P.Colour0 = FLinearColor(0.85f, 0.87f, 0.9f); P.Colour1 = FLinearColor(0.8f, 0.82f, 0.86f);
			P.Intensity = 0.45f; P.FadeIn = 0.1f; P.Spin = Rand() - 0.5f;
			Spawn(P);
		}
		return;
	}

	// 4. Fire where it struck, licking up for a second or two.
	for (int32 k = 0; k < 12; ++k)
	{
		FDragonPuff P;
		P.Kind = EDragonPuff::Glow;
		P.Pos = At + Up * R * 0.1f + FVector(RandDir().X, RandDir().Y, 0.f) * R * 0.45f;
		P.Vel = Up * (200.f + 400.f * Rand()) + RandDir() * 150.f;
		P.Drag = 1.5f; P.Buoyancy = 500.f;
		P.Life = 0.7f + 0.9f * Rand();
		P.Size0 = R * 0.35f; P.Size1 = R * (0.6f + 0.3f * Rand()); P.GrowPow = 0.6f;
		P.Colour0 = FireOrange * (5.f + 3.f * Rand()); P.Colour1 = FireDeep * 1.2f;
		P.FadeIn = 0.15f; P.Spin = Rand() - 0.5f;
		Spawn(P);
	}
	// 5. Smoke: a dark column that rises, spreads and blows away.
	for (int32 k = 0; k < 14; ++k)
	{
		FDragonPuff P;
		P.Kind = EDragonPuff::Smoke;
		P.Pos = At + Up * R * (0.2f + 0.3f * Rand()) + FVector(RandDir().X, RandDir().Y, 0.f) * R * 0.4f;
		P.Vel = Up * (350.f + 500.f * Rand()) + RandDir() * 250.f;
		P.Drag = 0.9f; P.Buoyancy = 160.f;
		P.Life = 4.f + 4.f * Rand();
		P.Size0 = R * 0.5f; P.Size1 = R * (1.8f + 1.2f * Rand());
		const float G = 0.10f + 0.12f * Rand();
		P.Colour0 = FLinearColor(G, G * 0.95f, G * 0.9f); P.Colour1 = FLinearColor(0.32f, 0.32f, 0.33f);
		P.Intensity = 0.75f; P.FadeIn = 0.12f; P.Spin = (Rand() - 0.5f) * 0.6f;
		Spawn(P);
	}
	// 6. Embers and clods flung out, glowing as they fall.
	for (int32 k = 0; k < 28; ++k)
	{
		FDragonPuff P;
		P.Kind = EDragonPuff::Glow;
		const FVector D = (RandDir() + Up * 1.1f).GetSafeNormal();
		P.Pos = At + Up * 50.f;
		P.Vel = D * (1500.f + 2500.f * Rand());
		P.Drag = 0.5f; P.Gravity = 980.f;
		P.Life = 0.8f + 1.4f * Rand();
		P.Size0 = 18.f + 12.f * Rand(); P.Size1 = 8.f; P.Stretch = 0.02f;
		P.Colour0 = FireOrange * 10.f; P.Colour1 = FireDeep * 3.f;
		P.FadeIn = 0.f;
		Spawn(P);
	}
	// 7. Dust kicked out along the ground.
	for (int32 k = 0; k < 10; ++k)
	{
		FDragonPuff P;
		P.Kind = EDragonPuff::Smoke;
		const FVector D = FVector::VectorPlaneProject(RandDir(), Up).GetSafeNormal();
		P.Pos = At + Up * 80.f;
		P.Vel = D * R * (2.f + 1.5f * Rand()) + Up * 100.f;
		P.Drag = 2.2f; P.Buoyancy = 40.f;
		P.Life = 2.f + 1.5f * Rand();
		P.Size0 = R * 0.3f; P.Size1 = R * (1.2f + 0.5f * Rand());
		P.Colour0 = FLinearColor(0.42f, 0.38f, 0.33f); P.Colour1 = FLinearColor(0.5f, 0.48f, 0.45f);
		P.Intensity = 0.5f; P.FadeIn = 0.05f;
		Spawn(P);
	}
	Scorch(At, Up, R * 0.75f);
}

void ADragonFX::LightningBolt(const FVector& Cloud, const FVector& To, float WidthCm)
{
	// A jagged channel: segments wander sideways by up to ~40% of their length, with a
	// couple of forks that die out partway down. Each segment is a fixed glowing streak.
	auto Channel = [&](FVector A, const FVector& B, int32 Segs, float Width, float Bright, int32 Depth)
	{
		const FVector Start = A;
		const float SegLen = (B - A).Size() / Segs;
		FVector Off = FVector::ZeroVector;
		for (int32 i = 0; i < Segs; ++i)
		{
			const float T1 = (i + 1.f) / Segs;
			// wander sideways, pulled back toward the straight line so it still lands at B
			const FVector J = RandDir(); Off = Off * 0.55f + FVector(J.X, J.Y, J.Z * 0.3f) * SegLen * 0.55f;
			const FVector Next = i == Segs - 1 ? B : FMath::Lerp(Start, B, T1) + Off * (1.f - T1 * 0.6f);
			const FVector D = Next - A;
			FDragonPuff P;
			P.Kind = EDragonPuff::Glow;
			P.Pos = (A + Next) * 0.5f;
			P.Axis = D.GetSafeNormal(); P.LengthCm = D.Size() + Width;
			P.Size0 = P.Size1 = Width;
			P.Life = 0.32f; P.FadeIn = 0.f;
			P.Colour0 = FLinearColor(0.75f, 0.8f, 1.f) * Bright; P.Colour1 = FLinearColor(0.6f, 0.65f, 1.f) * Bright * 0.5f;
			Spawn(P);
			// a soft halo around the channel
			FDragonPuff H = P; H.Size0 = H.Size1 = Width * 9.f; H.LengthCm = D.Size() + Width * 9.f;
			H.Colour0 = FLinearColor(0.45f, 0.5f, 1.f) * Bright * 0.08f; H.Colour1 = H.Colour0 * 0.3f;
			Spawn(H);
			if (Depth < 1 && i > 1 && i < Segs - 3 && Rand() < 0.22f)
			{
				const FVector Fork = Next + (RandDir() * 0.6f + FVector(0, 0, -1)).GetSafeNormal() * SegLen * (2.f + 3.f * Rand());
				FVector FA = Next;
				const int32 FS = 4;
				for (int32 k = 0; k < FS; ++k)
				{
					const FVector FN = FMath::Lerp(Next, Fork, (k + 1.f) / FS) + RandDir() * SegLen * 0.3f;
					FDragonPuff Q = P;
					Q.Pos = (FA + FN) * 0.5f; Q.Axis = (FN - FA).GetSafeNormal(); Q.LengthCm = (FN - FA).Size() + Width;
					Q.Size0 = Q.Size1 = Width * 0.5f; Q.Colour0 = P.Colour0 * 0.5f; Q.Colour1 = P.Colour1 * 0.3f;
					Spawn(Q);
					FA = FN;
				}
			}
			A = Next;
		}
	};
	Channel(Cloud, To, 18, WidthCm, 60.f, 0);
	Flash(FMath::Lerp(Cloud, To, 0.4f), 4.0e7f, 600000.f, 0.35f, FLinearColor(0.75f, 0.8f, 1.f), FLinearColor(0.6f, 0.65f, 1.f));
}

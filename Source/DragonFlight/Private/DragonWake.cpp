#include "DragonWake.h"
#include "DragonFlight.h"
#include "DragonSea.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Misc/CommandLine.h"

namespace { constexpr float STEP = 1.f / 60.f; constexpr float BIAS = 0.5f; constexpr int32 MAX_SRC = 8; }

ADragonWake::ADragonWake()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;   // after the pawn has made its disturbances
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

ADragonWake* ADragonWake::Get(const UObject* Ctx)
{
	UWorld* W = Ctx ? Ctx->GetWorld() : nullptr;
	if (!W || !W->IsGameWorld()) return nullptr;
	{ TActorIterator<ADragonWake> It(W); if (It) return *It; }
	FActorSpawnParameters SP; SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return W->SpawnActor<ADragonWake>(ADragonWake::StaticClass(), FTransform::Identity, SP);
}

void ADragonWake::BeginPlay()
{
	Super::BeginPlay();
	UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/World/Materials/M_WakeSim.M_WakeSim"));
	if (!M) { UE_LOG(LogDragon, Warning, TEXT("Wake: no M_WakeSim; the sea will not ripple")); SetActorTickEnabled(false); return; }
	SimMID = UMaterialInstanceDynamic::Create(M, this);
	for (int32 i = 0; i < 2; ++i)
	{
		RT[i] = NewObject<UTextureRenderTarget2D>(this);
		RT[i]->RenderTargetFormat = RTF_RGBA16f;
		RT[i]->ClearColor = FLinearColor(BIAS, BIAS, 0.f, 1.f);
		RT[i]->Filter = TF_Bilinear;
		RT[i]->AddressX = TA_Clamp; RT[i]->AddressY = TA_Clamp;
		RT[i]->bAutoGenerateMips = false;
		RT[i]->InitCustomFormat(Cells, Cells, PF_FloatRGBA, true);   // half floats, linear: millimetre ripples survive
		RT[i]->UpdateResourceImmediate(true);
	}
	SimMID->SetScalarParameterValue(TEXT("Texel"), 1.f / Cells);
	const float C = WaveSpeed * STEP / CellM;
	SimMID->SetScalarParameterValue(TEXT("C2"), FMath::Min(C * C, 0.45f));
	SimMID->SetScalarParameterValue(TEXT("Damp"), Damping);
	SimMID->SetScalarParameterValue(TEXT("FoamDecay"), FoamDecay);
}

void ADragonWake::Disturb(const FVector& AtCm, float RadiusCm, float Strength, float Foam)
{
	if (FMath::Abs(Strength) < 1e-3f && Foam < 1e-4f) return;
	Pending.Add({ FVector2D(AtCm.X, AtCm.Y), FMath::Max(RadiusCm, CellM * 100.f * 0.75f), Strength, Foam });
	Idle = 0.f;
}

void ADragonWake::BindSea()
{
	for (TActorIterator<ADragonSea> It(GetWorld()); It; ++It)
	{
		if (UMaterialInstanceDynamic* MID = It->GetSeaMID())
		{
			MID->SetTextureParameterValue(TEXT("WakeTex"), RT[Cur]);
			MID->SetScalarParameterValue(TEXT("WakeSize"), Cells * CellM * 100.f);
			MID->SetVectorParameterValue(TEXT("WakeCentre"), FLinearColor(CentreCm.X, CentreCm.Y, 0.f, 0.f));
			bBound = true;
		}
	}
}

void ADragonWake::Step(const FVector2D& OffsetUV, const TArray<FSource>& Src, float StepDt)
{
	const float Size = Cells * CellM * 100.f;
	SimMID->SetTextureParameterValue(TEXT("Prev"), RT[Cur]);
	SimMID->SetVectorParameterValue(TEXT("Offset"), FLinearColor(OffsetUV.X, OffsetUV.Y, 0.f, 0.f));
	for (int32 i = 0; i < MAX_SRC; ++i)
	{
		FLinearColor S(0.f, 0.f, 1e-6f, 0.f), Fm(0.f, 0.f, 0.f, 0.f);
		if (Src.IsValidIndex(i))
		{
			const FSource& Q = Src[i];
			const FVector2D UV = (Q.AtCm - CentreCm) / Size + FVector2D(0.5f, 0.5f);
			const float R = Q.RadiusCm / Size;
			S = FLinearColor(UV.X, UV.Y, R * R, FMath::Clamp(Q.Strength, -1.6f, 1.6f));
			Fm.R = Q.Foam * StepDt;
		}
		SimMID->SetVectorParameterValue(*FString::Printf(TEXT("S%d"), i), S);
		SimMID->SetScalarParameterValue(*FString::Printf(TEXT("F%d"), i), Fm.R);
	}
	const int32 Next = 1 - Cur;
	UKismetRenderingLibrary::DrawMaterialToRenderTarget(this, RT[Next], SimMID);
	Cur = Next;
}

void ADragonWake::Tick(float Dt)
{
	Super::Tick(Dt);
	if (!SimMID || !RT[0]) return;
	Idle += Dt;
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager) return;
	// Nothing has touched the water for a while and every ripple has died: rest.
	if (Idle > 20.f) { Pending.Reset(); return; }

	// Follow the camera, in whole cells so the field stays put in the world.
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	const float CellCm = CellM * 100.f, Size = Cells * CellCm;
	const FVector2D Want(FMath::RoundToDouble(Cam.X / CellCm) * CellCm, FMath::RoundToDouble(Cam.Y / CellCm) * CellCm);
	if (!bCentred) { CentreCm = Want; bCentred = true; }
	else if (FVector2D::Distance(Want, CentreCm) > 8.f * CellCm)
	{
		PendingShift += (Want - CentreCm) / Size;
		CentreCm = Want;
	}

	// The strongest sources this frame (they are spread over the frame's steps).
	Pending.Sort([](const FSource& A, const FSource& B) { return FMath::Abs(A.Strength) + A.Foam > FMath::Abs(B.Strength) + B.Foam; });
	if (Pending.Num() > MAX_SRC) Pending.SetNum(MAX_SRC);
	Accum = FMath::Min(Accum + Dt, STEP * 3.f);
	int32 Steps = 0;
	while (Accum >= STEP)
	{
		Accum -= STEP;
		// A source acts for the whole frame: each step gets its share at the step's length.
		Step(PendingShift, Pending, STEP);
		PendingShift = FVector2D::ZeroVector;
		++Steps;
	}
	if (Steps > 0) Pending.Reset();
	BindSea();
	// Diagnostics (-DragonTelemetry): the field under the camera and around it.
	static const bool bDiag = FParse::Param(FCommandLine::Get(), TEXT("DragonTelemetry"));
	DiagT += Dt;
	if (bDiag && DiagT > 1.f && RT[Cur])
	{
		DiagT = 0.f;
		FString Line;
		for (const float Off : { 0.f, 0.01f, 0.03f, 0.1f, 0.3f })
		{
			const FLinearColor Px = UKismetRenderingLibrary::ReadRenderTargetUV(this, RT[Cur], 0.5f + Off, 0.5f);
			Line += FString::Printf(TEXT(" [%.3f %.3f %.3f]"), (Px.R - 0.5f) * 4.f, (Px.G - 0.5f) * 4.f, Px.B);
		}
		UE_LOG(LogDragon, Display, TEXT("[Wake] h,hprev,foam at du=0,.01,.03,.1,.3:%s"), *Line);
	}
}

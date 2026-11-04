#include "DragonGameMode.h"
#include "DragonPhotoDirector.h"
#include "DragonVillage.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "DragonFlight.h"
#include "DragonPawn.h"
#include "DragonHUD.h"
#include "DragonSky.h"
#include "DragonWorldSubsystem.h"
#include "GameFramework/GameUserSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"

ADragonGameMode::ADragonGameMode()
{
	DefaultPawnClass = ADragonPawn::StaticClass();
	HUDClass = ADragonHUD::StaticClass();
	PlayerControllerClass = ADragonPlayerController::StaticClass();
}

void ADragonGameMode::StartPlay()
{
	Super::StartPlay();

	if (UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		// First run: benchmark, then go one step above what it suggests if it suggests Epic —
		// this game is meant to be seen on max.
		if (S->GetOverallScalabilityLevel() < 0 || S->GetLastCPUBenchmarkResult() < 0.f)
		{
			S->RunHardwareBenchmark();
			S->ApplyHardwareBenchmarkResults();
			if (S->GetOverallScalabilityLevel() >= 3) S->SetOverallScalabilityLevel(4);
			S->SetFrameRateLimit(0.f);
			S->ApplySettings(false);
			S->SaveSettings();
			UE_LOG(LogDragon, Log, TEXT("First run: graphics preset %d"), S->GetOverallScalabilityLevel());
		}
	}

	// Make sure there is a sky even in an empty test map.
	if (!ADragonSky::Get(this))
	{
		GetWorld()->SpawnActor<ADragonSky>();
	}
	// The villages, from the world bake's house plots (unless the map already has them).
	if (!TActorIterator<ADragonVillage>(GetWorld()))
	{
		GetWorld()->SpawnActor<ADragonVillage>();
	}
	// Unattended screenshots (-AutoShot): see ADragonPhotoDirector.
	if (FParse::Param(FCommandLine::Get(), TEXT("AutoShot")))
	{
		GetWorld()->SpawnActor<ADragonPhotoDirector>();
	}
}

APawn* ADragonGameMode::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
	UDragonWorldSubsystem* WS = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	if (WS && WS->IsLoaded())
	{
		FVector At = WS->GetSpawn();
		At.Z = SpawnHeightM * DragonUnits::M;
		// Face the home island.
		const FVector ToHome = WS->GetHome() - At;
		const FRotator Rot(0.f, ToHome.Rotation().Yaw, 0.f);
		FActorSpawnParameters P;
		P.Instigator = GetInstigator();
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return GetWorld()->SpawnActor<APawn>(GetDefaultPawnClassForController(NewPlayer), At, Rot, P);
	}
	return Super::SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
}

// ---------------------------------------------------------------------------

ADragonPlayerController::ADragonPlayerController()
{
	bShowMouseCursor = false;
}

void ADragonPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	if (WasInputKeyJustPressed(EKeys::Slash) || WasInputKeyJustPressed(EKeys::F1)) bShowHelp = !bShowHelp;
	if (WasInputKeyJustPressed(EKeys::F2))
	{
		if (UGameUserSettings* S = GEngine->GetGameUserSettings()) Quality((S->GetOverallScalabilityLevel() + 1) % 5);
	}
	if (WasInputKeyJustPressed(EKeys::F3))
	{
		if (ADragonSky* Sky = ADragonSky::Get(this)) Sky->Hours = FMath::Fmod(Sky->Hours + 3.f, 24.f);
	}
	if (WasInputKeyJustPressed(EKeys::F4))
	{
		if (ADragonSky* Sky = ADragonSky::Get(this)) Sky->SetWeather((EDragonWeather)(((int32)Sky->Weather + 1) % 6));
	}
	if (WasInputKeyJustPressed(EKeys::F6)) bShowHud = !bShowHud;
}

void ADragonPlayerController::Quality(int32 Level)
{
	if (UGameUserSettings* S = GEngine->GetGameUserSettings())
	{
		S->SetOverallScalabilityLevel(FMath::Clamp(Level, 0, 4));
		S->ApplySettings(false);
		S->SaveSettings();
	}
}

void ADragonPlayerController::TimeOfDay(float Hours)
{
	if (ADragonSky* Sky = ADragonSky::Get(this)) Sky->Hours = FMath::Fmod(FMath::Max(0.f, Hours), 24.f);
}

void ADragonPlayerController::Weather(int32 Preset)
{
	if (ADragonSky* Sky = ADragonSky::Get(this)) Sky->SetWeather((EDragonWeather)FMath::Clamp(Preset, 0, 5));
}

void ADragonPlayerController::GoTo(const FString& IslandName)
{
	UDragonWorldSubsystem* WS = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	ADragonPawn* P = Cast<ADragonPawn>(GetPawn());
	if (!WS || !P) return;
	for (const FDragonIsland& I : WS->GetIslands())
	{
		if (I.Name.Contains(IslandName))
		{
			FVector At = I.Centre / DragonUnits::M;
			At.Z = FMath::Max(I.Peak / DragonUnits::M, At.Z) + 150.0;
			P->TeleportToM(At, true);
			return;
		}
	}
}

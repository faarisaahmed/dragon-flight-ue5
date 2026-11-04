#include "DragonGameMode.h"
#include "DragonPhotoDirector.h"
#include "DragonVillage.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "DragonFlight.h"
#include "DragonPawn.h"
#include "DragonHUD.h"
#include "DragonSky.h"
#include "DragonWorldSubsystem.h"
#include "GameFramework/GameUserSettings.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
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

	ADragonPlayerController::ApplyRenderScale();

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
	// Frame time in the log every 10 s, for judging performance on a given machine.
	{
		static double Acc = 0.0, Worst = 0.0; static int32 Frames = 0;
		Acc += DeltaTime; Worst = FMath::Max(Worst, (double)DeltaTime); ++Frames;
		if (Acc >= 10.0)
		{
			const UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
			static const IConsoleVariable* SP = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
			UE_LOG(LogDragon, Display, TEXT("Perf: %.1f fps avg (%.1f ms), worst frame %.1f ms, preset %d, screen %% %.0f"),
				Frames / Acc, Acc / Frames * 1000.0, Worst * 1000.0, S ? S->GetOverallScalabilityLevel() : -1, SP ? SP->GetFloat() : -1.f);
			// -PerfFile: the same line into Saved/Perf.txt (Shipping builds have no log).
			if (FParse::Param(FCommandLine::Get(), TEXT("PerfFile")))
			{
				const FString Line = FString::Printf(TEXT("%.1f fps avg (%.1f ms), worst %.1f ms, screen %% %.0f\n"),
					Frames / Acc, Acc / Frames * 1000.0, Worst * 1000.0, SP ? SP->GetFloat() : -1.f);
				FFileHelper::SaveStringToFile(Line, *(FPaths::ProjectSavedDir() / TEXT("Perf.txt")), FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
			}
			Acc = 0.0; Worst = 0.0; Frames = 0;
			// -PerfProfile: one GPU profile into the log once things have settled (third report).
			static int32 Reports = 0;
			if (++Reports == 3 && FParse::Param(FCommandLine::Get(), TEXT("PerfProfile"))) ConsoleCommand(TEXT("ProfileGPU"));
		}
	}
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
		ApplyRenderScale();
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

void ADragonPlayerController::ApplyRenderScale()
{
	// Render a fixed number of lines whatever the display (TSR upscales to the output): 810 on
	// Epic and below, 1080 on Max. A 4K screen at native would cost ~5x a 1080p one.
	const UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	FVector2D Size(1920, 1080);
	if (GEngine && GEngine->GameViewport) GEngine->GameViewport->GetViewportSize(Size);
	const float Lines = (S && S->GetOverallScalabilityLevel() >= 4) ? 1080.f : 810.f;
	const float Pct = FMath::Clamp(Lines / FMath::Max(Size.Y, 1.0) * 100.f, 33.f, 100.f);
	if (IConsoleVariable* SP = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage")))
		SP->Set(Pct, ECVF_SetByGameSetting);
}

#include "DragonGameMode.h"
#include "DragonPhotoDirector.h"
#include "DragonReport.h"
#include "DragonVillage.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
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
	DragonReport::CheckPreviousCrash();

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

void ADragonPlayerController::SetPanelOpen(bool bOpen)
{
	bPanelOpen = bOpen;
	bShowMouseCursor = bOpen;
	bEnableClickEvents = bOpen;
	if (bOpen)
	{
		FInputModeGameAndUI Mode;
		Mode.SetHideCursorDuringCapture(false);
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways);
		SetInputMode(Mode);
		// Start the cursor over the panel.
		int32 SX = 0, SY = 0; GetViewportSize(SX, SY);
		SetMouseLocation(SX - 200, 140);
	}
	else
	{
		SetInputMode(FInputModeGameOnly());
	}
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
	if (WasInputKeyJustPressed(EKeys::Tab) || WasInputKeyJustPressed(EKeys::T)) SetPanelOpen(!bPanelOpen);
	if (bPanelOpen && WasInputKeyJustPressed(EKeys::Escape)) SetPanelOpen(false);
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
	// Keep the render height right when the window is resized.
	{
		static FVector2D LastSize = FVector2D::ZeroVector;
		FVector2D Size = FVector2D::ZeroVector;
		if (GEngine && GEngine->GameViewport) GEngine->GameViewport->GetViewportSize(Size);
		if (Size != LastSize && Size.Y > 0.0) { LastSize = Size; ApplyRenderScale(); }
	}
	if (WasInputKeyJustPressed(EKeys::F5)) RenderRes((GetRenderResMode() + 1) % 6);
	if (WasInputKeyJustPressed(EKeys::F6)) bShowHud = !bShowHud;
	// F8: a screenshot plus where you are and your settings, sent to Faaris. (-ReportSelfTest
	// sends one by itself after 20 s, for testing.)
	static double SelfTestAt = FParse::Param(FCommandLine::Get(), TEXT("ReportSelfTest")) ? FPlatformTime::Seconds() + 20.0 : 0.0;
	const bool bSelfTest = SelfTestAt > 0.0 && FPlatformTime::Seconds() > SelfTestAt;
	if (bSelfTest) SelfTestAt = 0.0;
	if (WasInputKeyJustPressed(EKeys::F8) || bSelfTest)
	{
		FString Where = TEXT("Where: unknown");
		const ADragonPawn* P = Cast<ADragonPawn>(GetPawn());
		const UDragonWorldSubsystem* WS = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
		if (P && WS)
		{
			double Dist = 0.0;
			const FString Island = WS->NearestIsland(P->GetActorLocation(), Dist);
			const FVector L = P->GetActorLocation() / 100.0;
			Where = FString::Printf(TEXT("Where: %s%s, at (%.0f, %.0f) m, alt %.0f m, %s"), Dist < 0.0 ? TEXT("over ") : TEXT("near "), *Island, L.X, L.Y, P->GetAltitudeM(), *P->GetModeName());
		}
		if (const ADragonSky* Sky = ADragonSky::Get(this)) Where += FString::Printf(TEXT(", %.1f h, weather %d"), Sky->Hours, (int32)Sky->Weather);
		DragonReport::SendSnapshot(Where);
	}
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

namespace
{
	const TCHAR* RenderResSection = TEXT("/Script/DragonFlight.DragonGraphics");
	const float RenderResLines[] = { 0.f, 720.f, 900.f, 1080.f, 1440.f, 100000.f };
	const TCHAR* RenderResNames[] = { TEXT("Auto"), TEXT("720p"), TEXT("900p"), TEXT("1080p"), TEXT("1440p"), TEXT("Native") };

	float RenderLinesFor(int32 Mode)
	{
		if (Mode > 0) return RenderResLines[Mode];
		const UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
		return (S && S->GetOverallScalabilityLevel() >= 4) ? 1080.f : 810.f;
	}
}

int32 ADragonPlayerController::GetRenderResMode()
{
	int32 Mode = 0;
	if (GConfig) GConfig->GetInt(RenderResSection, TEXT("RenderRes"), Mode, GGameUserSettingsIni);
	return FMath::Clamp(Mode, 0, 5);
}

FString ADragonPlayerController::GetRenderResLabel()
{
	const int32 Mode = GetRenderResMode();
	if (Mode == 5) return RenderResNames[5];
	const FString Lines = FString::Printf(TEXT("%dp"), FMath::RoundToInt(RenderLinesFor(Mode)));
	return Mode == 0 ? FString::Printf(TEXT("Auto (%s)"), *Lines) : Lines;
}

void ADragonPlayerController::RenderRes(int32 Mode)
{
	if (!GConfig) return;
	GConfig->SetInt(RenderResSection, TEXT("RenderRes"), FMath::Clamp(Mode, 0, 5), GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
	ApplyRenderScale();
}

void ADragonPlayerController::ApplyRenderScale()
{
	// Render a fixed number of lines whatever the display (TSR upscales to the output). Auto is
	// 810 on Epic and below, 1080 on Max; F5 picks a fixed height or Native for a faster machine.
	// A 4K screen at native costs ~5x a 1080p one.
	FVector2D Size(1920, 1080);
	if (GEngine && GEngine->GameViewport) GEngine->GameViewport->GetViewportSize(Size);
	const float Lines = RenderLinesFor(GetRenderResMode());
	const float Pct = FMath::Clamp(Lines / FMath::Max(Size.Y, 1.0) * 100.f, 33.f, 100.f);
	if (IConsoleVariable* SP = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage")))
		SP->Set(Pct, ECVF_SetByGameSetting);
}

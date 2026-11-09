#include "DragonGameMode.h"
#include "DragonPhotoDirector.h"
#include "DragonReport.h"
#include "DragonVillage.h"
#include "DragonVents.h"
#include "DragonSurfSpray.h"
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
#include "Framework/Application/SlateApplication.h"
#include "UnrealClient.h"
#include "RHI.h"
#if PLATFORM_MAC
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace
{
	/**
	 * While the game has the mouse, keep the (hidden) system cursor parked mid-screen. On the
	 * mini's Retina screen the engine's "keep it in the window" clamp put it on the bottom
	 * edge, where the Dock rose under it and clicks opened apps. In captured mode the engine
	 * reads raw motion and the cursor does not follow the mouse, so one warp holds.
	 */
	void ParkSystemCursor()
	{
#if PLATFORM_MAC
		const CGRect B = CGDisplayBounds(CGMainDisplayID());
		const CGPoint Mid = CGPointMake(B.origin.x + B.size.width * 0.5, B.origin.y + B.size.height * 0.5);
		CGEventRef E = CGEventCreate(nullptr);
		if (!E) return;
		const CGPoint At = CGEventGetLocation(E);   // top-left origin, points
		CFRelease(E);
		if (FMath::Abs(At.x - Mid.x) > 40.0 || FMath::Abs(At.y - Mid.y) > 40.0)
		{
			CGWarpMouseCursorPosition(Mid);
			CGAssociateMouseAndMouseCursorPosition(false);   // still captured: no drift, no 0.25 s event pause
		}
#endif
	}
}

ADragonGameMode::ADragonGameMode()
{
	DefaultPawnClass = ADragonPawn::StaticClass();
	HUDClass = ADragonHUD::StaticClass();
	PlayerControllerClass = ADragonPlayerController::StaticClass();
}

void ADragonGameMode::StartPlay()
{
	// Steam on the volcanic isles.
	if (UWorld* W = GetWorld())
	{
		FActorSpawnParameters SP; SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		W->SpawnActor<ADragonVents>(ADragonVents::StaticClass(), FTransform::Identity, SP);
		W->SpawnActor<ADragonSurfSpray>(ADragonSurfSpray::StaticClass(), FTransform::Identity, SP);
	}
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
	// The menu and the chart pause the game; the controller must still read keys under them.
	SetTickableWhenPaused(true);
	bShouldPerformFullTickWhenPaused = true;
}

void ADragonPlayerController::SetScreen(int32 NewScreen, bool bPause)
{
	Screen = NewScreen;
	const bool bOpen = Screen != 0;
	bPanelOpen = bOpen;
	bEnableClickEvents = bOpen;
	UGameplayStatics::SetGamePaused(this, bOpen && bPause);
	// The menu and chart use the game's own pointer, moved by the same mouse movement that
	// steers him, so the system cursor stays captured and hidden. A freed system cursor got
	// stuck at the screen edge (seen when playing over a remote/streamed screen). Only when
	// another app has the focus is the system cursor handed back.
	if (bOpen && !bWasForeground) ReleasePointer(); else CapturePointer();
}

void ADragonPlayerController::CapturePointer()
{
	bShowMouseCursor = false;
	SetInputMode(FInputModeGameOnly());
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
		if (UGameViewportClient* VC = GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
			if (FViewport* V = VC->Viewport) { V->CaptureMouse(true); V->LockMouseToViewport(true); }
	}
}

void ADragonPlayerController::ReleasePointer()
{
	bShowMouseCursor = true;
	FInputModeGameAndUI Mode;
	Mode.SetHideCursorDuringCapture(false);
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(Mode);
}

namespace { const TCHAR* ControlsSection = TEXT("/Script/DragonFlight.DragonControls"); }

float ADragonPlayerController::GetMouseScale()
{
	float V = 1.f;
	if (GConfig) GConfig->GetFloat(ControlsSection, TEXT("MouseScale"), V, GGameUserSettingsIni);
	return FMath::Clamp(V, 0.25f, 3.f);
}

bool ADragonPlayerController::GetInvertY()
{
	bool B = false;
	if (GConfig) GConfig->GetBool(ControlsSection, TEXT("InvertY"), B, GGameUserSettingsIni);
	return B;
}

void ADragonPlayerController::SetMouseScale(float Scale)
{
	if (!GConfig) return;
	GConfig->SetFloat(ControlsSection, TEXT("MouseScale"), FMath::Clamp(Scale, 0.25f, 3.f), GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
	ApplyControls();
}

void ADragonPlayerController::SetInvertY(bool bInvert)
{
	if (!GConfig) return;
	GConfig->SetBool(ControlsSection, TEXT("InvertY"), bInvert, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
	ApplyControls();
}

void ADragonPlayerController::ApplyControls()
{
	if (ADragonPawn* P = Cast<ADragonPawn>(GetPawn()))
	{
		P->MouseSensitivity = 0.0035f * GetMouseScale();
		P->bInvertY = GetInvertY();
	}
}

void ADragonPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	ApplyControls();
}

// Frame pacing. Running the GPU flat out on this game's long cloud pass makes every
// tenth frame or so take two or three times as long on macOS (measured: 47 fps average
// with 50 ms hitches at Wrathholm; capped at 45 the same view is a flat 45, worst 22 ms).
// So: start uncapped; when hitches show up, cap a step under the rate the GPU actually
// manages; step back up only when the GPU has clear room. A limit the player set wins.
void ADragonPlayerController::TickPacer(float Dt)
{
	static const bool bOff = FParse::Param(FCommandLine::Get(), TEXT("NoPacer")) || FCString::Strifind(FCommandLine::Get(), TEXT("MaxFPS")) != nullptr;
	UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	if (bOff || !S || S->GetFrameRateLimit() > 0.f || Dt <= 0.f) return;
	if (UGameplayStatics::IsGamePaused(this)) return;

	constexpr float MinCap = 40.f, MaxCap = 60.f, Window = 3.f;
	static float Cap = 0.f;            // 0 = uncapped
	static float WinT = 0.f, CalmT = 0.f, GpuMs = 0.f, AvgMs = 0.f;
	static int32 Frames = 0, Hitches = 0;
	const float Ms = Dt * 1000.f;
	// a hitch: a frame far slower than the ones around it (a load or a menu isn't one)
	if (AvgMs > 0.f && Ms > 1.7f * AvgMs && Ms < 250.f) ++Hitches;
	if (Ms < 250.f) AvgMs = AvgMs > 0.f ? AvgMs + (Ms - AvgMs) * 0.05f : Ms;
	const float G = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
	if (G > 0.f) GpuMs = GpuMs > 0.f ? GpuMs + (G - GpuMs) * 0.05f : G;
	WinT += Dt; ++Frames;
	if (WinT < Window) return;

	const float Fps = Frames / WinT;
	float NewCap = Cap;
	if (Hitches >= 3)
	{
		const float Under = FMath::FloorToFloat(Fps / 5.f) * 5.f;
		NewCap = FMath::Max(MinCap, FMath::Min(Cap > 0.f ? Cap - 5.f : MaxCap, Under));
		CalmT = 0.f;
	}
	else if (Hitches == 0 && Cap > 0.f && (CalmT += WinT) >= 15.f && GpuMs * 1.03f < 1000.f / (Cap + 5.f))
	{
		NewCap = Cap + 5.f >= MaxCap ? 0.f : Cap + 5.f;
		CalmT = 0.f;
	}
	if (NewCap != Cap)
	{
		Cap = NewCap;
		GEngine->SetMaxFPS(Cap);
		UE_LOG(LogDragon, Display, TEXT("Pacer: %s (%.0f fps, %d hitches, gpu %.1f ms)"), Cap > 0.f ? *FString::Printf(TEXT("cap %.0f"), Cap) : TEXT("uncapped"), Fps, Hitches, GpuMs);
	}
	WinT = 0.f; Frames = 0; Hitches = 0;
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
	// Keep the render height right when the window is resized.
	{
		static FVector2D LastSize = FVector2D::ZeroVector;
		FVector2D Size = FVector2D::ZeroVector;
		if (GEngine && GEngine->GameViewport) GEngine->GameViewport->GetViewportSize(Size);
		if (Size != LastSize && Size.Y > 0.0) { LastSize = Size; ApplyRenderScale(); }
	}
	TickPacer(DeltaTime);
	if (IsBenchmarking()) { TickBench(DeltaTime); return; }
	// Switching to another app while flying pauses into the menu and frees the pointer.
	{
		const bool bFg = !FSlateApplication::IsInitialized() || FSlateApplication::Get().IsActive();
		if (bWasForeground && !bFg && Screen == 0 && !FParse::Param(FCommandLine::Get(), TEXT("AutoShot")) && !IsBenchmarking())
		{
			bWasForeground = false;
			MenuPage = 0;
			SetScreen(1);
		}
		// Back in the game with the menu up: take the pointer again (the game draws its own).
		if (!bWasForeground && bFg && Screen != 0) CapturePointer();
		bWasForeground = bFg;
		if (bFg && !bShowMouseCursor && !FParse::Param(FCommandLine::Get(), TEXT("NoCursorPark"))) ParkSystemCursor();
	}
	// Esc: menu (or back to the game from anything); Tab / M: the chart; T: weather & time; F1: controls.
	if (WasInputKeyJustPressed(EKeys::Escape)) SetScreen(Screen == 0 ? 1 : 0);
	if (WasInputKeyJustPressed(EKeys::Tab) || WasInputKeyJustPressed(EKeys::M)) SetScreen(Screen == 2 ? 0 : 2);
	if (WasInputKeyJustPressed(EKeys::T)) { if (Screen == 1 && MenuPage == 1) SetScreen(0); else { MenuPage = 1; SetScreen(1); } }
	if (WasInputKeyJustPressed(EKeys::Slash) || WasInputKeyJustPressed(EKeys::F1)) { if (Screen == 1 && MenuPage == 2) SetScreen(0); else { MenuPage = 2; SetScreen(1); } }
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
			Where += FString::Printf(TEXT(", facing %.0f"), P->GetCompassDegrees());
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
	// On a 4K (HiDPI) screen the upscaler and all post-processing would run at the full 2160
	// lines: ~10 ms a frame on the mini for detail nobody sees. Upscale to at most 1440 lines
	// (1080 below Max) and let a cheap final pass stretch that to the window.
	const float OutLines = FMath::Max(Lines, GetRenderResMode() == 5 ? (float)Size.Y : (Lines > 1080.f ? 1440.f : 1080.f));
	const float Secondary = FMath::Clamp(OutLines / FMath::Max(Size.Y, 1.0) * 100.f, 25.f, 100.f);
	if (IConsoleVariable* SS = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SecondaryScreenPercentage.GameViewport")))
		SS->Set(Secondary, ECVF_SetByGameSetting);
	const float Pct = FMath::Clamp(Lines / FMath::Max(Size.Y * Secondary / 100.f, 1.0) * 100.f, 33.f, 100.f);
	if (IConsoleVariable* SP = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage")))
		SP->Set(Pct, ECVF_SetByGameSetting);
}

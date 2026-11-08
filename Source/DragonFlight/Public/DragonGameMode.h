#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "DragonGameMode.generated.h"

/**
 * Spawns the dragon where the web game did (300 m over the home island's
 * approach), applies the graphics preset on first run (the highest the
 * hardware benchmark allows — Cinematic on a strong PC), and provides the
 * debug/quality hotkeys.
 */
UCLASS()
class DRAGONFLIGHT_API ADragonGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ADragonGameMode();

	virtual void StartPlay() override;
	virtual APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot) override;

	/** Height above the sea to start at, metres (web: 300). */
	UPROPERTY(EditAnywhere, Category = "Dragon") float SpawnHeightM = 300.f;
};

UCLASS()
class DRAGONFLIGHT_API ADragonPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ADragonPlayerController();
	virtual void PlayerTick(float DeltaTime) override;
	/** -Bench: a scripted flight that measures frame rate per place (DragonBench.cpp). */
	static bool IsBenchmarking();
	void TickBench(float Dt);

	/** 0 Low .. 4 Cinematic ("Max"). */
	UFUNCTION(Exec) void Quality(int32 Level);
	/** Sets r.ScreenPercentage from the render-resolution setting (Auto: ~810 lines, 1080 on Max). */
	static void ApplyRenderScale();
	/** Render resolution: 0 Auto, 1 720p, 2 900p, 3 1080p, 4 1440p, 5 Native. Saved. */
	UFUNCTION(Exec) void RenderRes(int32 Mode);
	static int32 GetRenderResMode();
	static FString GetRenderResLabel();
	UFUNCTION(Exec) void TimeOfDay(float Hours);
	UFUNCTION(Exec) void Weather(int32 Preset);
	UFUNCTION(Exec) void GoTo(const FString& IslandName);

	bool bShowHelp = false;
	bool bShowHud = true;
	/** A screen over the game has the mouse (menu or chart); the game is paused under it. */
	bool bPanelOpen = false;
	void SetPanelOpen(bool bOpen) { SetScreen(bOpen ? 1 : 0); }

	/** 0 = flying, 1 = menu (Esc), 2 = chart (Tab / M). */
	int32 Screen = 0;
	bool bWasForeground = true;   // the app had focus last frame
	/** Menu page: 0 Settings, 1 Weather & time, 2 Controls. */
	int32 MenuPage = 0;
	void SetScreen(int32 NewScreen, bool bPause = true);

	/** Saved controls: mouse sensitivity as a multiple of the default (0.25..3), invert Y. */
	static float GetMouseScale();
	static bool GetInvertY();
	void SetMouseScale(float Scale);
	void SetInvertY(bool bInvert);
	void ApplyControls();

	virtual void OnPossess(APawn* InPawn) override;
};

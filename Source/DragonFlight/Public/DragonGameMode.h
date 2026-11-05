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

	bool bShowHelp = true;
	bool bShowHud = true;
};

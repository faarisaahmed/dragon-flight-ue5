#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "DragonHUD.generated.h"

/**
 * A quiet HUD drawn straight on the canvas: where you are, how fast, how high,
 * the context prompt (which always names the failing condition and the key that
 * fixes it), a compass, and the controls card (F1 or / toggles it).
 */
UCLASS()
class DRAGONFLIGHT_API ADragonHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void Text(const FString& S, float X, float Y, const FLinearColor& C, float Scale = 1.f, bool bCentre = false, UFont* Font = nullptr);
	void DrawCompass(float Deg);
	void DrawHelp();
	/** Weather & time panel, top right: compact when closed, clickable when open (Tab). */
	void DrawPanel(class ADragonPlayerController* PC);
	void Box(float X, float Y, float W, float H, const FLinearColor& C);
	bool Button(const FString& Label, float X, float Y, float W, float H, bool bOn, bool bLive);

	FString LastIsland;
	bool bDragTime = false;
	bool bClick = false;     // left button went down this frame
	FVector2D Mouse = FVector2D::ZeroVector;
	float SavedDayLength = 48.f;
	float UI = 1.f;          // panel scale for the screen height
	float IslandBanner = 0.f;
};

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

	FString LastIsland;
	float IslandBanner = 0.f;
};

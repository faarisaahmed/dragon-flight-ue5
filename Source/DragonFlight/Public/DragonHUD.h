#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "Fonts/SlateFontInfo.h"
#include "DragonHUD.generated.h"

class UTexture2D;
class ADragonPlayerController;
class ADragonPawn;

/**
 * The game's whole interface, drawn on the canvas.
 *
 * Look: dark glass, hairlines in aged bronze, frost-white type. Titles in Marcellus SC (an
 * engraved, inscriptional capital), numbers and labels in Barlow / Barlow Condensed. The only
 * ornament is northern interlace used sparingly: a triquetra knot, a knotted divider, linked
 * rings at frame corners, an interlace band round the compass rose. (Content/UI, drawn by
 * Tools/WorldBake/ui_art.py; the chart by Tools/WorldBake/map.py.)
 *
 *  - Flight: location (top left), compass strip (top centre), clock + weather (top right),
 *    speed / height / mode / plasma (bottom left), the context prompt, arrival banner.
 *  - Esc: the menu (game paused): Settings, Weather & Time, Controls, Quit.
 *  - Tab (or M): the chart (game paused): zoom with the wheel, drag to pan, hover an island.
 */
UCLASS()
class DRAGONFLIGHT_API ADragonHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	// --- style ----------------------------------------------------------------------------------
	enum class EFace : uint8 { Title, Body, Medium, Cond, CondBold };
	const FSlateFontInfo& Font(EFace Face, float Px);
	FVector2D Measure(const FString& S, const FSlateFontInfo& F) const;
	/** Draws text; Align 0 left, 0.5 centre, 1 right. Returns its size. */
	FVector2D Txt(const FString& S, float X, float Y, EFace Face, float Px, const FLinearColor& C, float Align = 0.f, bool bShadow = true);
	void Rect(float X, float Y, float W, float H, const FLinearColor& C);
	void Line(float X0, float Y0, float X1, float Y1, const FLinearColor& C, float Thick = 1.f);
	void Img(UTexture2D* T, float X, float Y, float W, float H, const FLinearColor& C, FVector2D UV0 = FVector2D(0, 0), FVector2D UV1 = FVector2D(1, 1), float RotDeg = 0.f);
	void Diamond(float CX, float CY, float R, const FLinearColor& C, bool bFilled);
	/** Dark glass panel with a bronze hairline and knotted corners. */
	void Panel(float X, float Y, float W, float H, float Alpha = 1.f);
	bool Button(const FString& Label, float X, float Y, float W, float H, bool bOn, float Px = 0.f);
	/** A row of choices; returns the index clicked or -1. */
	int32 Segmented(const TArray<FString>& Items, int32 Current, float X, float Y, float W, float H);
	/** A horizontal slider over [0,1]; returns true while being dragged (V updated). */
	bool Slider(int32 Id, float& V, float X, float Y, float W);
	bool Toggle(const FString& Label, bool bOn, float X, float Y, float W);
	bool Hover(float X, float Y, float W, float H) const { return Mouse.X >= X && Mouse.X <= X + W && Mouse.Y >= Y && Mouse.Y <= Y + H; }

	// --- screens --------------------------------------------------------------------------------
	void DrawFlight(ADragonPawn* P, ADragonPlayerController* PC, float Dt);
	void DrawCompass(float Deg);
	void DrawMenu(ADragonPlayerController* PC);
	void DrawSettings(ADragonPlayerController* PC, float X, float Y, float W);
	void DrawWeather(float X, float Y, float W);
	void DrawControls(float X, float Y, float W);
	void DrawMap(ADragonPawn* P, ADragonPlayerController* PC, float Dt);
	void LoadArt();

	UPROPERTY() TObjectPtr<UTexture2D> TexMap;
	UPROPERTY() TObjectPtr<UTexture2D> TexKnot;
	UPROPERTY() TObjectPtr<UTexture2D> TexRule;
	UPROPERTY() TObjectPtr<UTexture2D> TexCorner;
	UPROPERTY() TObjectPtr<UTexture2D> TexRing;
	bool bArtLoaded = false;

	TMap<int32, FSlateFontInfo> Fonts;
	float U = 1.f;            // 1080p pixels -> screen pixels
	FVector2D Mouse = FVector2D::ZeroVector;
	FVector2D LastMouse = FVector2D::ZeroVector;
	bool bClick = false;      // left button went down this frame
	bool bHeld = false;
	bool bPointerWasUI = false;
	bool bSlateLeftWas = false;
	FVector2D SlateMouse = FVector2D::ZeroVector;
	/** Screen pixels (at 1080p) per unit of raw mouse movement for the game's own pointer. */
	float PointerSpeed = 1.2f;
	void DrawPointer();
	int32 DragSlider = -1;
	FString LastIsland;
	float IslandBanner = 0.f;
	float SavedDayLength = 48.f;
	float Clock = 0.f;        // real seconds, for UI animation (runs while paused)

	// chart view: centre (0..1 map UV) and zoom (1 = whole map)
	FVector2D MapCentre = FVector2D(0.5, 0.5);
	float MapZoom = 1.f;
	bool bMapDrag = false;
	bool bMapWasOpen = false;
};

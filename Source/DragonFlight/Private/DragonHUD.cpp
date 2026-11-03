#include "DragonHUD.h"
#include "DragonPawn.h"
#include "DragonGameMode.h"
#include "DragonSky.h"
#include "DragonWorldSubsystem.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameUserSettings.h"

namespace
{
	const FLinearColor Ink(0.93f, 0.94f, 0.9f, 0.92f);
	const FLinearColor Dim(0.75f, 0.78f, 0.8f, 0.7f);
	const FLinearColor Warm(1.f, 0.78f, 0.45f, 0.95f);
	const TCHAR* QualityNames[] = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic"), TEXT("Max (Cinematic)") };
}

void ADragonHUD::Text(const FString& S, float X, float Y, const FLinearColor& C, float Scale, bool bCentre, UFont* Font)
{
	if (!Canvas) return;
	UFont* F = Font ? Font : GEngine->GetMediumFont();
	float W = 0, H = 0;
	Canvas->TextSize(F, S, W, H, Scale, Scale);
	const float PX = bCentre ? X - W * 0.5f : X;
	FCanvasTextItem Shadow(FVector2D(PX + 1.5f, Y + 1.5f), FText::FromString(S), F, FLinearColor(0, 0, 0, C.A * 0.6f));
	Shadow.Scale = FVector2D(Scale);
	Canvas->DrawItem(Shadow);
	FCanvasTextItem Item(FVector2D(PX, Y), FText::FromString(S), F, C);
	Item.Scale = FVector2D(Scale);
	Canvas->DrawItem(Item);
}

void ADragonHUD::DrawCompass(float Deg)
{
	const float CX = Canvas->ClipX * 0.5f, Y = 26.f, Span = 260.f;
	static const TCHAR* Names[] = { TEXT("N"), TEXT("NE"), TEXT("E"), TEXT("SE"), TEXT("S"), TEXT("SW"), TEXT("W"), TEXT("NW") };
	for (int32 i = 0; i < 8; ++i)
	{
		float D = FMath::Fmod(i * 45.f - Deg + 540.f, 360.f) - 180.f;
		if (FMath::Abs(D) > 75.f) continue;
		const float A = 1.f - FMath::Abs(D) / 75.f;
		Text(Names[i], CX + D / 75.f * Span * 0.5f, Y, FLinearColor(Ink.R, Ink.G, Ink.B, A * 0.9f), i % 2 ? 0.8f : 1.f, true);
	}
	Text(TEXT("|"), CX, Y + 18.f, Dim, 0.8f, true);
}

void ADragonHUD::DrawHelp()
{
	static const TCHAR* Lines[] = {
		TEXT("FLYING"),
		TEXT("W / S          speed up / back (let go = hover)"),
		TEXT("A / D          turn"),
		TEXT("Space / K      climb / sink  (double-tap + hold: zoom climb / dive)"),
		TEXT("Shift          sprint     J or B  flat out (double-tap: turbo)"),
		TEXT("O / P          sideslip   L / ;   knife edge (double-tap: barrel roll)"),
		TEXT("Mouse          look       Q recentre   H face the camera"),
		TEXT("LMB / U        fire       RMB / E aim"),
		TEXT("Hold I or K    land       Space  take off"),
		TEXT(""),
		TEXT("F1 help  F2 graphics  F3 time +3h  F4 weather  F6 hide HUD"),
		TEXT("~ console: GoTo <island>, TimeOfDay <h>, Weather <0-5>, Quality <0-4>"),
	};
	float Y = Canvas->ClipY - 40.f - UE_ARRAY_COUNT(Lines) * 18.f;
	for (const TCHAR* L : Lines)
	{
		Text(L, Canvas->ClipX - 560.f, Y, Dim, 0.85f, false, GEngine->GetSmallFont());
		Y += 18.f;
	}
}

void ADragonHUD::DrawHUD()
{
	Super::DrawHUD();
	ADragonPawn* P = Cast<ADragonPawn>(GetOwningPawn());
	ADragonPlayerController* PC = Cast<ADragonPlayerController>(GetOwningPlayerController());
	if (!P || !Canvas) return;
	if (PC && !PC->bShowHud) return;
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	const float Dt = GetWorld()->GetDeltaSeconds();

	// Where am I — a banner when you arrive somewhere new.
	if (UDragonWorldSubsystem* WS = GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>())
	{
		double Dist = 0.0;
		const FString Island = WS->NearestIsland(P->GetActorLocation(), Dist);
		const bool bOver = Dist < 0.0;
		if (bOver && Island != LastIsland) { LastIsland = Island; IslandBanner = 5.f; }
		if (!bOver && Dist > 1500.0 * 100.0) LastIsland.Reset();
		if (IslandBanner > 0.f)
		{
			IslandBanner -= Dt;
			const float A = FMath::Clamp(IslandBanner, 0.f, 1.f) * FMath::Clamp((5.f - IslandBanner) * 2.f, 0.f, 1.f);
			Text(Island, W * 0.5f, H * 0.18f, FLinearColor(Warm.R, Warm.G, Warm.B, A), 1.6f, true, GEngine->GetLargeFont());
		}
		Text(bOver ? Island : FString::Printf(TEXT("Open sea — %s %.1f km"), *Island, Dist / 100000.0), 28.f, 24.f, Dim, 0.9f);
	}

	DrawCompass(P->GetCompassDegrees());

	// Speed / height / mode, bottom left.
	const float Speed = P->GetSpeedMS();
	Text(FString::Printf(TEXT("%d mph"), FMath::RoundToInt(FMath::Abs(Speed) / 0.44704f)), 28.f, H - 96.f, Ink, 1.5f, false, GEngine->GetLargeFont());
	Text(FString::Printf(TEXT("%s    alt %d m    agl %d m"), *P->GetModeName(), FMath::RoundToInt(P->GetAltitudeM()), FMath::RoundToInt(P->GetAGL())), 30.f, H - 50.f, Dim, 0.9f);

	// Shots: six pips.
	for (int32 i = 0; i < 6; ++i)
	{
		const bool bFull = i < P->GetShots();
		FCanvasTileItem Pip(FVector2D(30.f + i * 14.f, H - 120.f), FVector2D(9.f, 9.f), bFull ? FLinearColor(0.55f, 0.65f, 1.f, 0.9f) : FLinearColor(1, 1, 1, 0.15f));
		Pip.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Pip);
	}

	// Health only when hurt; wing stamina only when used.
	if (P->GetHealth() < 99.5f)
	{
		FCanvasTileItem Back(FVector2D(30.f, H - 136.f), FVector2D(160.f, 5.f), FLinearColor(0, 0, 0, 0.4f));
		Back.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Back);
		FCanvasTileItem Bar(FVector2D(30.f, H - 136.f), FVector2D(1.6f * P->GetHealth(), 5.f), FLinearColor(0.9f, 0.3f, 0.25f, 0.85f));
		Bar.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Bar);
	}
	if (P->GetKnifeCharge01() < 0.999f)
	{
		FCanvasTileItem Bar(FVector2D(W * 0.5f - 60.f, H * 0.62f), FVector2D(120.f * P->GetKnifeCharge01(), 3.f), FLinearColor(1, 1, 1, 0.5f));
		Bar.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Bar);
	}

	if (P->IsAiming())
	{
		Text(TEXT("+"), W * 0.5f, H * 0.5f - 12.f, Ink, 1.4f, true, GEngine->GetLargeFont());
	}

	const FString Prompt = P->GetPrompt();
	if (!Prompt.IsEmpty()) Text(Prompt, W * 0.5f, H * 0.72f, Ink, 1.1f, true);

	if (PC && PC->bShowHelp) DrawHelp();

	if (UGameUserSettings* S = GEngine->GetGameUserSettings())
	{
		const int32 Q = FMath::Clamp(S->GetOverallScalabilityLevel(), 0, 4);
		Text(FString::Printf(TEXT("Graphics: %s"), QualityNames[Q]), W - 200.f, 24.f, Dim, 0.75f, false, GEngine->GetSmallFont());
	}
	if (ADragonSky* Sky = ADragonSky::Get(this))
	{
		const int32 Hh = FMath::FloorToInt(Sky->Hours), Mm = FMath::FloorToInt((Sky->Hours - Hh) * 60.f);
		static const TCHAR* WNames[] = { TEXT("Clear"), TEXT("Fair"), TEXT("Overcast"), TEXT("Rain"), TEXT("Storm"), TEXT("Fog") };
		Text(FString::Printf(TEXT("%02d:%02d  %s"), Hh, Mm, WNames[(int32)Sky->Weather]), W - 200.f, 42.f, Dim, 0.75f, false, GEngine->GetSmallFont());
	}
}

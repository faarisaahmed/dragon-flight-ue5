#include "DragonHUD.h"
#include "DragonPawn.h"
#include "DragonGameMode.h"
#include "DragonSky.h"
#include "DragonReport.h"
#include "DragonWorldSubsystem.h"
#include "Engine/Canvas.h"
#include "CanvasItem.h"
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
	GetTextSize(S, W, H, F, Scale); // AHUD wrapper with float out-params; same measurement as Canvas->TextSize
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
		TEXT("Tab / T  weather & time panel (click to choose)"),
		TEXT("F1 help  F2 graphics  F5 resolution  F3 time +3h  F4 weather  F6 hide HUD"),
		TEXT("F8 send Faaris a screenshot (something looks wrong / a bug)"),
		TEXT("~ console: GoTo <island>, TimeOfDay <h>, Weather <0-5>, Quality <0-4>, RenderRes <0-5>"),
	};
	float Y = Canvas->ClipY - 40.f - UE_ARRAY_COUNT(Lines) * 18.f;
	for (const TCHAR* L : Lines)
	{
		Text(L, Canvas->ClipX - 560.f, Y, Dim, 0.85f, false, GEngine->GetSmallFont());
		Y += 18.f;
	}
}

void ADragonHUD::Box(float X, float Y, float W, float H, const FLinearColor& C)
{
	FCanvasTileItem T(FVector2D(X, Y), FVector2D(W, H), C);
	T.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(T);
}

bool ADragonHUD::Button(const FString& Label, float X, float Y, float W, float H, bool bOn, bool bLive)
{
	const bool bHover = bLive && Mouse.X >= X && Mouse.X <= X + W && Mouse.Y >= Y && Mouse.Y <= Y + H;
	Box(X, Y, W, H, bOn ? FLinearColor(1.f, 0.78f, 0.45f, 0.32f) : FLinearColor(1, 1, 1, bHover ? 0.16f : 0.07f));
	if (bOn) Box(X, Y + H - 2.f * UI, W, 2.f * UI, FLinearColor(1.f, 0.78f, 0.45f, 0.9f));
	Text(Label, X + W * 0.5f, Y + H * 0.5f - 8.f * UI, bOn ? Ink : Dim, 0.85f * UI, true, GEngine->GetSmallFont());
	return bHover && bClick;
}

void ADragonHUD::DrawPanel(ADragonPlayerController* PC)
{
	ADragonSky* Sky = ADragonSky::Get(this);
	if (!Sky || !Canvas) return;
	const bool bOpen = PC && PC->bPanelOpen;
	// Sizes are in "900p pixels", scaled up on bigger screens so it reads the same on a 4K monitor.
	UI = FMath::Clamp(Canvas->ClipY / 900.f, 1.f, 2.4f);
	if (PC)
	{
		float MX = 0, MY = 0;
		if (PC->GetMousePosition(MX, MY)) Mouse = FVector2D(MX, MY);
		bClick = bOpen && PC->WasInputKeyJustPressed(EKeys::LeftMouseButton);
		if (!bOpen || !PC->IsInputKeyDown(EKeys::LeftMouseButton)) bDragTime = false;
	}
	const float PW = 360.f * UI, X0 = Canvas->ClipX - PW - 20.f * UI, Y0 = 56.f * UI, Pad = 14.f * UI;
	const int32 Hh = FMath::FloorToInt(Sky->Hours), Mm = FMath::FloorToInt((Sky->Hours - Hh) * 60.f);
	UFont* Small = GEngine->GetSmallFont();

	// What the four choices mean, in the sky's own presets.
	struct FChoice { const TCHAR* Name; EDragonWeather W; };
	static const FChoice Choices[] = { { TEXT("Sunny"), EDragonWeather::Clear }, { TEXT("Cloudy"), EDragonWeather::Overcast },
		{ TEXT("Rainy"), EDragonWeather::Rain }, { TEXT("Thunderstorm"), EDragonWeather::Storm } };
	auto NameOf = [](EDragonWeather W) -> const TCHAR*
	{
		switch (W)
		{
		case EDragonWeather::Clear: return TEXT("Sunny");
		case EDragonWeather::Fair: return TEXT("Fair");
		case EDragonWeather::Overcast: return TEXT("Cloudy");
		case EDragonWeather::Rain: return TEXT("Rainy");
		case EDragonWeather::Storm: return TEXT("Thunderstorm");
		default: return TEXT("Fog");
		}
	};

	if (!bOpen)
	{
		const float CW = 250.f * UI;
		Box(X0 + PW - CW, Y0, CW, 28.f * UI, FLinearColor(0, 0, 0, 0.3f));
		Text(FString::Printf(TEXT("%02d:%02d   %s"), Hh, Mm, NameOf(Sky->Weather)), X0 + PW - CW + 10.f * UI, Y0 + 5.f * UI, Ink, 0.9f * UI, false, Small);
		Text(TEXT("Tab"), X0 + PW - 38.f * UI, Y0 + 6.f * UI, Dim, 0.75f * UI, false, Small);
		return;
	}

	const float PH = 282.f * UI;
	Box(X0, Y0, PW, PH, FLinearColor(0.02f, 0.03f, 0.04f, 0.66f));
	Text(TEXT("Weather & time"), X0 + Pad, Y0 + 10.f * UI, Warm, 1.f * UI, false, Small);
	Text(TEXT("Tab to close"), X0 + PW - 96.f * UI, Y0 + 12.f * UI, Dim, 0.75f * UI, false, Small);

	// Weather.
	float Y = Y0 + 40.f * UI;
	const float Gap = 6.f * UI, BW = (PW - 2.f * Pad - 3.f * Gap) / 4.f;
	for (int32 i = 0; i < 4; ++i)
	{
		const bool bOn = Sky->Weather == Choices[i].W;
		if (Button(Choices[i].Name, X0 + Pad + i * (BW + Gap), Y, BW, 32.f * UI, bOn, true))
		{
			Sky->bAutoWeather = false;
			Sky->SetWeather(Choices[i].W, false, 8.f);   // rolls in over a few seconds, not a minute
		}
	}
	Y += 40.f * UI;
	if (Button(Sky->bAutoWeather ? TEXT("Changes by itself: on") : TEXT("Changes by itself: off"), X0 + Pad, Y, PW - 2.f * Pad, 26.f * UI, Sky->bAutoWeather, true))
	{
		Sky->bAutoWeather = !Sky->bAutoWeather;
	}

	// Time of day: a slider over the day, with the sun's hours shaded.
	Y += 42.f * UI;
	Text(FString::Printf(TEXT("Time  %02d:%02d"), Hh, Mm), X0 + Pad, Y, Ink, 0.95f * UI, false, Small);
	Y += 24.f * UI;
	const float SX = X0 + Pad, SW = PW - 2.f * Pad, SH = 16.f * UI;
	for (int32 k = 0; k < 48; ++k)
	{
		// day / night shading: light over the ~05:30-20:00 northern summer day
		const float Hr = (k + 0.5f) * 0.5f;
		const float Day = FMath::Clamp(FMath::Min(Hr - 4.5f, 21.f - Hr) / 1.5f, 0.f, 1.f);
		Box(SX + k * SW / 48.f, Y, SW / 48.f + 0.5f, SH, FMath::Lerp(FLinearColor(0.08f, 0.1f, 0.2f, 0.7f), FLinearColor(0.55f, 0.62f, 0.7f, 0.5f), Day));
	}
	const bool bOverSlider = Mouse.X >= SX - 6.f * UI && Mouse.X <= SX + SW + 6.f * UI && Mouse.Y >= Y - 8.f * UI && Mouse.Y <= Y + SH + 8.f * UI;
	if (bClick && bOverSlider) bDragTime = true;
	if (bDragTime) Sky->Hours = FMath::Clamp((Mouse.X - SX) / SW, 0.f, 1.f) * 23.99f;
	const float HX = SX + Sky->Hours / 24.f * SW;
	Box(HX - 3.f * UI, Y - 5.f * UI, 6.f * UI, SH + 10.f * UI, FLinearColor(1.f, 0.85f, 0.55f, 0.95f));
	for (int32 h = 0; h <= 24; h += 6)
		Text(FString::Printf(TEXT("%02d"), h % 24), SX + h / 24.f * SW, Y + SH + 4.f * UI, Dim, 0.7f * UI, true, Small);

	// Quick times and the clock.
	Y += SH + 28.f * UI;
	struct FQuick { const TCHAR* Name; float H; };
	static const FQuick Quick[] = { { TEXT("Dawn"), 5.4f }, { TEXT("Morning"), 9.f }, { TEXT("Noon"), 12.75f }, { TEXT("Evening"), 18.5f }, { TEXT("Sunset"), 20.f }, { TEXT("Night"), 0.5f } };
	const float QG = 4.f * UI, QW = (PW - 2.f * Pad - 5.f * QG) / 6.f;
	for (int32 i = 0; i < 6; ++i)
		if (Button(Quick[i].Name, X0 + Pad + i * (QW + QG), Y, QW, 26.f * UI, FMath::Abs(Sky->Hours - Quick[i].H) < 0.05f, true)) Sky->Hours = Quick[i].H;
	Y += 34.f * UI;
	const bool bRunning = Sky->DayLengthMinutes > 0.f;
	if (Button(bRunning ? TEXT("Clock running") : TEXT("Clock stopped"), X0 + Pad, Y, PW - 2.f * Pad, 26.f * UI, bRunning, true))
	{
		if (bRunning) { SavedDayLength = Sky->DayLengthMinutes; Sky->DayLengthMinutes = 0.f; }
		else Sky->DayLengthMinutes = SavedDayLength > 0.f ? SavedDayLength : 48.f;
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
	const FString Notice = DragonReport::Notice();
	if (!Notice.IsEmpty()) Text(Notice, W * 0.5f, H * 0.1f, Warm, 1.1f, true);

	if (UGameUserSettings* S = GEngine->GetGameUserSettings())
	{
		const int32 Q = FMath::Clamp(S->GetOverallScalabilityLevel(), 0, 4);
		Text(FString::Printf(TEXT("Graphics: %s   Resolution: %s"), QualityNames[Q], *ADragonPlayerController::GetRenderResLabel()), W - 360.f, 24.f, Dim, 0.75f, false, GEngine->GetSmallFont());
	}
	DrawPanel(PC);
}

#include "DragonHUD.h"
#include "DragonPawn.h"
#include "DragonGameMode.h"
#include "DragonSky.h"
#include "DragonReport.h"
#include "DragonWorldSubsystem.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "CanvasItem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "GameFramework/GameUserSettings.h"
#include "ImageUtils.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "RenderUtils.h"
#include "TextureResource.h"

namespace
{
	// Palette: frost-white type, mist for secondary text, aged bronze for the lines and accents,
	// a cold plasma blue for the dragon's fire, ember red for hurt.
	const FLinearColor Frost(0.93f, 0.95f, 0.96f, 0.96f);
	const FLinearColor Mist(0.72f, 0.77f, 0.80f, 0.78f);
	const FLinearColor Faint(0.72f, 0.77f, 0.80f, 0.42f);
	const FLinearColor Bronze(0.80f, 0.64f, 0.42f, 0.95f);
	const FLinearColor BronzeDim(0.80f, 0.64f, 0.42f, 0.45f);
	const FLinearColor Glass(0.012f, 0.02f, 0.03f, 0.86f);
	const FLinearColor Plasma(0.56f, 0.70f, 1.0f, 0.95f);
	const FLinearColor Ember(0.95f, 0.38f, 0.25f, 0.9f);

	FLinearColor WithA(FLinearColor C, float A) { C.A *= A; return C; }

	const TCHAR* QualityNames[] = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic"), TEXT("Cinematic") };
	const TCHAR* ResNames[] = { TEXT("Auto"), TEXT("720p"), TEXT("900p"), TEXT("1080p"), TEXT("1440p"), TEXT("Native") };

	const TCHAR* WeatherName(EDragonWeather W)
	{
		switch (W)
		{
		case EDragonWeather::Clear: return TEXT("Clear");
		case EDragonWeather::Fair: return TEXT("Fair");
		case EDragonWeather::Overcast: return TEXT("Overcast");
		case EDragonWeather::Rain: return TEXT("Rain");
		case EDragonWeather::Storm: return TEXT("Storm");
		default: return TEXT("Fog");
		}
	}

	FString UpperSpaced(const FString& S) { return S.ToUpper(); }
}

// --- style ------------------------------------------------------------------------------------

const FSlateFontInfo& ADragonHUD::Font(EFace Face, float Px)
{
	const int32 Size = FMath::Max(6, FMath::RoundToInt(Px));
	const int32 Key = (int32)Face * 1000 + Size;
	if (const FSlateFontInfo* F = Fonts.Find(Key)) return *F;
	static const TCHAR* Files[] = { TEXT("MarcellusSC-Regular.ttf"), TEXT("Barlow-Regular.ttf"), TEXT("Barlow-Medium.ttf"),
		TEXT("BarlowCondensed-Medium.ttf"), TEXT("BarlowCondensed-SemiBold.ttf") };
	const FString Path = FPaths::ProjectContentDir() / TEXT("UI/Fonts") / Files[(int32)Face];
	// Slate sizes are in points at 96 dpi: px = pt * 96/72.
	FSlateFontInfo Info = FPaths::FileExists(Path)
		? FSlateFontInfo(Path, Size * 0.75f)
		: FSlateFontInfo(FCoreStyle::GetDefaultFontStyle("Regular", FMath::RoundToInt(Size * 0.75f)));
	return Fonts.Add(Key, Info);
}

FVector2D ADragonHUD::Measure(const FString& S, const FSlateFontInfo& F) const
{
	if (!FSlateApplication::IsInitialized()) return FVector2D(S.Len() * F.Size * 0.6f, F.Size * 1.3f);
	return FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(S, F);
}

FVector2D ADragonHUD::Txt(const FString& S, float X, float Y, EFace Face, float Px, const FLinearColor& C, float Align, bool bShadow)
{
	if (!Canvas || S.IsEmpty()) return FVector2D::ZeroVector;
	const FSlateFontInfo& F = Font(Face, Px * U);
	const FVector2D Sz = Measure(S, F);
	const float PX = X - Sz.X * Align;
	// The canvas only draws text that has a UFont; with a Slate font set it is just the carrier
	// (it must be a runtime-cache font, as the engine's own are).
	UFont* Carrier = GEngine->GetMediumFont();
	if (bShadow)
	{
		FCanvasTextItem Sh(FVector2D(PX + 1.f * U, Y + 1.2f * U), FText::FromString(S), F, FLinearColor(0.f, 0.f, 0.f, C.A * 0.55f));
		Sh.Font = Carrier;
		Canvas->DrawItem(Sh);
	}
	FCanvasTextItem It(FVector2D(PX, Y), FText::FromString(S), F, C);
	It.Font = Carrier;
	Canvas->DrawItem(It);
	return Sz;
}

void ADragonHUD::Rect(float X, float Y, float W, float H, const FLinearColor& C)
{
	FCanvasTileItem T(FVector2D(X, Y), FVector2D(W, H), C);
	T.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(T);
}

void ADragonHUD::Line(float X0, float Y0, float X1, float Y1, const FLinearColor& C, float Thick)
{
	// Canvas lines ignore alpha; straight hairlines are drawn as thin tiles, which blend.
	const float T = FMath::Max(Thick, 1.f);
	if (FMath::Abs(Y0 - Y1) < 0.01f) { Rect(FMath::Min(X0, X1), Y0 - T * 0.5f, FMath::Abs(X1 - X0), T, C); return; }
	if (FMath::Abs(X0 - X1) < 0.01f) { Rect(X0 - T * 0.5f, FMath::Min(Y0, Y1), T, FMath::Abs(Y1 - Y0), C); return; }
	FCanvasLineItem L(FVector2D(X0, Y0), FVector2D(X1, Y1));
	L.SetColor(C);
	L.LineThickness = Thick;
	L.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(L);
}

void ADragonHUD::Img(UTexture2D* T, float X, float Y, float W, float H, const FLinearColor& C, FVector2D UV0, FVector2D UV1, float RotDeg)
{
	if (!T || !T->GetResource()) return;
	FCanvasTileItem It(FVector2D(X, Y), T->GetResource(), FVector2D(W, H), UV0, UV1, C);
	It.BlendMode = SE_BLEND_Translucent;
	if (RotDeg != 0.f) { It.Rotation = FRotator(0.f, RotDeg, 0.f); It.PivotPoint = FVector2D(0.5f, 0.5f); }
	Canvas->DrawItem(It);
}

void ADragonHUD::Diamond(float CX, float CY, float R, const FLinearColor& C, bool bFilled)
{
	const FVector2D A(CX, CY - R), B(CX + R, CY), Cc(CX, CY + R), D(CX - R, CY);
	if (bFilled)
	{
		TArray<FCanvasUVTri> Tris;
		FCanvasUVTri T1; T1.V0_Pos = A; T1.V1_Pos = B; T1.V2_Pos = Cc; T1.V0_Color = T1.V1_Color = T1.V2_Color = C;
		FCanvasUVTri T2; T2.V0_Pos = A; T2.V1_Pos = Cc; T2.V2_Pos = D; T2.V0_Color = T2.V1_Color = T2.V2_Color = C;
		Tris.Add(T1); Tris.Add(T2);
		FCanvasTriangleItem It(Tris, GWhiteTexture);
		It.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(It);
	}
	else
	{
		Line(A.X, A.Y, B.X, B.Y, C, 1.2f * U); Line(B.X, B.Y, Cc.X, Cc.Y, C, 1.2f * U);
		Line(Cc.X, Cc.Y, D.X, D.Y, C, 1.2f * U); Line(D.X, D.Y, A.X, A.Y, C, 1.2f * U);
	}
}

void ADragonHUD::Panel(float X, float Y, float W, float H, float Alpha)
{
	Rect(X, Y, W, H, WithA(Glass, Alpha));
	// a faint lift at the top edge, like light on glass
	Rect(X, Y, W, 1.f * U, WithA(FLinearColor(1, 1, 1, 0.06f), Alpha));
	const FLinearColor Edge = WithA(BronzeDim, Alpha);
	const float I = 6.f * U;   // the hairline frame sits just inside the glass
	Line(X + I, Y + I, X + W - I, Y + I, Edge); Line(X + I, Y + H - I, X + W - I, Y + H - I, Edge);
	Line(X + I, Y + I, X + I, Y + H - I, Edge); Line(X + W - I, Y + I, X + W - I, Y + H - I, Edge);
	// linked-ring corners (the texture's knot sits at its top-left; mirror it for the others)
	const float C = 46.f * U;
	const FLinearColor K = WithA(Bronze, Alpha * 0.9f);
	Img(TexCorner, X - 2.f * U, Y - 2.f * U, C, C, K, FVector2D(0, 0), FVector2D(1, 1));
	Img(TexCorner, X + W - C + 2.f * U, Y - 2.f * U, C, C, K, FVector2D(1, 0), FVector2D(0, 1));
	Img(TexCorner, X - 2.f * U, Y + H - C + 2.f * U, C, C, K, FVector2D(0, 1), FVector2D(1, 0));
	Img(TexCorner, X + W - C + 2.f * U, Y + H - C + 2.f * U, C, C, K, FVector2D(1, 1), FVector2D(0, 0));
}

bool ADragonHUD::Button(const FString& Label, float X, float Y, float W, float H, bool bOn, float Px)
{
	const bool bHover = Hover(X, Y, W, H);
	Rect(X, Y, W, H, bOn ? FLinearColor(0.80f, 0.64f, 0.42f, 0.16f) : FLinearColor(1, 1, 1, bHover ? 0.07f : 0.025f));
	const FLinearColor Edge = bOn ? Bronze : bHover ? WithA(Bronze, 0.7f) : FLinearColor(1, 1, 1, 0.10f);
	Line(X, Y, X + W, Y, Edge); Line(X, Y + H, X + W, Y + H, Edge);
	Line(X, Y, X, Y + H, Edge); Line(X + W, Y, X + W, Y + H, Edge);
	const float P = Px > 0.f ? Px : 16.f;
	Txt(Label, X + W * 0.5f, Y + H * 0.5f - P * 0.62f * U, EFace::Medium, P, bOn ? Frost : bHover ? Frost : Mist, 0.5f, false);
	return bHover && bClick;
}

int32 ADragonHUD::Segmented(const TArray<FString>& Items, int32 Current, float X, float Y, float W, float H)
{
	int32 Hit = -1;
	const float G = 4.f * U, BW = (W - G * (Items.Num() - 1)) / FMath::Max(1, Items.Num());
	for (int32 i = 0; i < Items.Num(); ++i)
		if (Button(Items[i], X + i * (BW + G), Y, BW, H, i == Current, 15.f)) Hit = i;
	return Hit;
}

bool ADragonHUD::Slider(int32 Id, float& V, float X, float Y, float W)
{
	const float H = 22.f * U, CY = Y + H * 0.5f;
	if (bClick && Hover(X - 8.f * U, Y, W + 16.f * U, H)) DragSlider = Id;
	if (DragSlider == Id && !bHeld) DragSlider = -1;
	if (DragSlider == Id) V = FMath::Clamp((Mouse.X - X) / W, 0.f, 1.f);
	Line(X, CY, X + W, CY, FLinearColor(1, 1, 1, 0.16f), 2.f * U);
	Line(X, CY, X + W * V, CY, Bronze, 2.f * U);
	Diamond(X + W * V, CY, 7.f * U, DragSlider == Id || Hover(X + W * V - 10.f * U, Y, 20.f * U, H) ? Frost : Bronze, true);
	return DragSlider == Id;
}

bool ADragonHUD::Toggle(const FString& Label, bool bOn, float X, float Y, float W)
{
	const float H = 30.f * U;
	const bool bHover = Hover(X, Y, W, H);
	Txt(Label, X, Y + 5.f * U, EFace::Body, 16.f, bHover ? Frost : Mist);
	// a small switch: a hairline track with a diamond that slides across
	const float TW = 44.f * U, TX = X + W - TW, CY = Y + H * 0.5f;
	Line(TX, CY, TX + TW, CY, bOn ? Bronze : FLinearColor(1, 1, 1, 0.2f), 2.f * U);
	Diamond(bOn ? TX + TW - 7.f * U : TX + 7.f * U, CY, 7.f * U, bOn ? Bronze : Mist, bOn);
	return bHover && bClick;
}

void ADragonHUD::LoadArt()
{
	if (bArtLoaded) return;
	bArtLoaded = true;
	const FString Dir = FPaths::ProjectContentDir() / TEXT("UI");
	auto Load = [&](const TCHAR* Name) -> UTexture2D*
	{
		UTexture2D* T = FImageUtils::ImportFileAsTexture2D(Dir / Name);
		if (T)
		{
			T->Filter = TF_Trilinear;
			T->LODGroup = TEXTUREGROUP_UI;
			T->SRGB = true;
			T->UpdateResource();
		}
		return T;
	};
	TexMap = Load(TEXT("Map_4096.jpg"));
	TexKnot = Load(TEXT("Knot.png"));
	TexRule = Load(TEXT("Rule.png"));
	TexCorner = Load(TEXT("Corner.png"));
	TexRing = Load(TEXT("Ring.png"));
}

// --- flight -----------------------------------------------------------------------------------

void ADragonHUD::DrawCompass(float Deg)
{
	const float CX = Canvas->ClipX * 0.5f, Y = 28.f * U, Span = 640.f * U, Range = 110.f;
	// the base: the knotted rule, its knot marking straight ahead
	Img(TexRule, CX - Span * 0.5f, Y + 30.f * U, Span, Span / 16.f, WithA(Bronze, 0.85f));
	for (int32 d = 0; d < 360; d += 15)
	{
		const float Off = FMath::Fmod(d - Deg + 540.f, 360.f) - 180.f;
		if (FMath::Abs(Off) > Range * 0.5f) continue;
		const float X = CX + Off / Range * Span;
		const float A = 1.f - FMath::Pow(FMath::Abs(Off) / (Range * 0.5f), 2.f);
		if (d % 90 == 0)
		{
			static const TCHAR* Card[] = { TEXT("N"), TEXT("E"), TEXT("S"), TEXT("W") };
			Txt(Card[d / 90], X, Y - 2.f * U, EFace::Title, 27.f, WithA(d == 0 ? Bronze : Frost, A), 0.5f);
		}
		else if (d % 45 == 0)
		{
			static const TCHAR* Inter[] = { TEXT("NE"), TEXT("SE"), TEXT("SW"), TEXT("NW") };
			Txt(Inter[d / 90], X, Y + 4.f * U, EFace::CondBold, 17.f, WithA(Frost, A * 0.8f), 0.5f);
		}
		else
		{
			Line(X, Y + 12.f * U, X, Y + 24.f * U, WithA(Frost, A * 0.6f), 1.5f * U);
		}
	}
	Txt(FString::Printf(TEXT("%03d"), FMath::RoundToInt(FMath::Fmod(Deg + 360.f, 360.f)) % 360), CX, Y + 52.f * U, EFace::CondBold, 17.f, Frost, 0.5f);
}

void ADragonHUD::DrawFlight(ADragonPawn* P, ADragonPlayerController* PC, float Dt)
{
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	// Soft dark bands at the top and bottom edges so the type reads over a bright sky or sea.
	// Each band is a stack of gradient quads that share their edges exactly (per-vertex alpha,
	// no overlap): overlapping strips doubled up into visible scanlines across the screen.
	{
		TArray<FCanvasUVTri> Tris;
		auto Band = [&](float Y0, float Hgt, float AMax, bool bFromTop)
		{
			const int32 NS = 12;
			for (int32 k = 0; k < NS; ++k)
			{
				const float T0 = k / (float)NS, T1 = (k + 1) / (float)NS;
				const float A0 = AMax * FMath::Square(1.f - T0), A1 = AMax * FMath::Square(1.f - T1);
				const float Ya = bFromTop ? Y0 + T0 * Hgt : Y0 - T0 * Hgt, Yb = bFromTop ? Y0 + T1 * Hgt : Y0 - T1 * Hgt;
				const FLinearColor Ca(0.f, 0.01f, 0.02f, A0), Cb(0.f, 0.01f, 0.02f, A1);
				FCanvasUVTri T; T.V0_Pos = FVector2D(0, Ya); T.V1_Pos = FVector2D(W, Ya); T.V2_Pos = FVector2D(W, Yb);
				T.V0_Color = Ca; T.V1_Color = Ca; T.V2_Color = Cb; Tris.Add(T);
				FCanvasUVTri T2; T2.V0_Pos = FVector2D(0, Ya); T2.V1_Pos = FVector2D(W, Yb); T2.V2_Pos = FVector2D(0, Yb);
				T2.V0_Color = Ca; T2.V1_Color = Cb; T2.V2_Color = Cb; Tris.Add(T2);
			}
		};
		Band(0.f, 130.f * U, 0.30f, true);
		Band(H, 200.f * U, 0.27f, false);
		FCanvasTriangleItem It(Tris, GWhiteTexture);
		It.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(It);
	}

	// Where: the island you are over (or the nearest), top left.
	if (UDragonWorldSubsystem* WS = GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>())
	{
		double Dist = 0.0;
		const FString Island = WS->NearestIsland(P->GetActorLocation(), Dist);
		const bool bOver = Dist < 0.0;
		if (bOver && Island != LastIsland) { LastIsland = Island; IslandBanner = 5.5f; }
		if (!bOver && Dist > 1500.0 * 100.0) LastIsland.Reset();
		const float X = 44.f * U, Y = 34.f * U;
		Txt(bOver ? Island : TEXT("Open Sea"), X, Y, EFace::Title, 26.f, Frost);
		Line(X, Y + 40.f * U, X + 150.f * U, Y + 40.f * U, BronzeDim, 1.f * U);
		Diamond(X + 150.f * U, Y + 40.f * U, 3.f * U, Bronze, true);
		Txt(bOver ? FString(TEXT("Overhead")) : FString::Printf(TEXT("%s  ·  %.1f km"), *Island, Dist / 100000.0), X, Y + 48.f * U, EFace::Body, 15.f, Mist);

		// Arrival: the name, large and quiet, with the knotted rule beneath it.
		if (IslandBanner > 0.f)
		{
			IslandBanner -= Dt;
			const float A = FMath::Clamp(IslandBanner / 1.2f, 0.f, 1.f) * FMath::Clamp((5.5f - IslandBanner) * 1.5f, 0.f, 1.f);
			Txt(Island, W * 0.5f, H * 0.20f, EFace::Title, 54.f, WithA(Frost, A), 0.5f);
			Img(TexRule, W * 0.5f - 260.f * U, H * 0.20f + 70.f * U, 520.f * U, 32.5f * U, WithA(Bronze, A * 0.9f));
		}
	}

	DrawCompass(P->GetCompassDegrees());

	// Clock and sky, top right; the two keys that open everything else beneath.
	if (ADragonSky* Sky = ADragonSky::Get(this))
	{
		const int32 Hh = FMath::FloorToInt(Sky->Hours), Mm = FMath::FloorToInt((Sky->Hours - Hh) * 60.f);
		const float X = W - 44.f * U, Y = 34.f * U;
		Txt(FString::Printf(TEXT("%02d:%02d"), Hh, Mm), X, Y - 4.f * U, EFace::CondBold, 30.f, Frost, 1.f);
		Txt(WeatherName(Sky->Weather), X, Y + 34.f * U, EFace::Title, 18.f, Bronze, 1.f);
		Txt(TEXT("Esc  Menu     Tab  Chart"), X, Y + 62.f * U, EFace::CondBold, 15.f, Mist, 1.f);
	}

	// Flight block, bottom left: a bronze stem with the knot at its head, then the numbers.
	{
		const float X = 44.f * U, Base = H - 44.f * U;
		const float Top = Base - 150.f * U;
		Line(X, Top + 34.f * U, X, Base, BronzeDim, 1.f * U);
		Img(TexKnot, X - 15.f * U, Top, 30.f * U, 30.f * U, Bronze);
		const float TX = X + 22.f * U;

		// plasma: six diamonds
		for (int32 i = 0; i < 6; ++i)
		{
			const bool bFull = i < P->GetShots();
			Diamond(TX + 6.f * U + i * 17.f * U, Top + 15.f * U, 5.f * U, bFull ? Plasma : FLinearColor(1, 1, 1, 0.22f), bFull);
		}
		Txt(UpperSpaced(P->GetModeName()), TX + 112.f * U, Top + 6.f * U, EFace::Cond, 14.f, Bronze);

		const float Mph = FMath::Abs(P->GetSpeedMS()) / 0.44704f;
		const FVector2D S = Txt(FString::Printf(TEXT("%d"), FMath::RoundToInt(Mph)), TX, Top + 26.f * U, EFace::CondBold, 64.f, Frost);
		Txt(TEXT("MPH"), TX + S.X + 8.f * U, Top + 62.f * U, EFace::Cond, 16.f, Mist);

		const float RY = Base - 30.f * U;
		const FVector2D A1 = Txt(TEXT("ALT"), TX, RY + 4.f * U, EFace::Cond, 13.f, Faint);
		const FVector2D A2 = Txt(FString::Printf(TEXT("%d m"), FMath::RoundToInt(P->GetAltitudeM())), TX + A1.X + 6.f * U, RY, EFace::Cond, 19.f, Frost);
		const float BX = TX + A1.X + A2.X + 26.f * U;
		const FVector2D B1 = Txt(TEXT("GROUND"), BX, RY + 4.f * U, EFace::Cond, 13.f, Faint);
		Txt(FString::Printf(TEXT("%d m"), FMath::RoundToInt(P->GetAGL())), BX + B1.X + 6.f * U, RY, EFace::Cond, 19.f, Frost);

		if (P->GetHealth() < 99.5f)
		{
			const float HW = 200.f * U, HY = Base + 8.f * U;
			Line(TX, HY, TX + HW, HY, FLinearColor(1, 1, 1, 0.12f), 3.f * U);
			Line(TX, HY, TX + HW * FMath::Clamp(P->GetHealth() / 100.f, 0.f, 1.f), HY, Ember, 3.f * U);
		}
	}

	if (P->IsAiming())
	{
		// A thin ring of four arcs and a point.
		const float CX = W * 0.5f, CY = H * 0.5f, R = 18.f * U;
		for (int32 q = 0; q < 4; ++q)
		{
			const float A0 = q * HALF_PI + 0.35f, A1 = (q + 1) * HALF_PI - 0.35f;
			for (int32 k = 0; k < 6; ++k)
			{
				const float a = FMath::Lerp(A0, A1, k / 6.f), b = FMath::Lerp(A0, A1, (k + 1) / 6.f);
				Line(CX + R * FMath::Cos(a), CY + R * FMath::Sin(a), CX + R * FMath::Cos(b), CY + R * FMath::Sin(b), Frost, 1.5f * U);
			}
		}
		Rect(CX - 1.5f * U, CY - 1.5f * U, 3.f * U, 3.f * U, Frost);
	}

	// The prompt: one line, low and central, on a soft dark band.
	const FString Prompt = P->GetPrompt();
	if (!Prompt.IsEmpty())
	{
		const FVector2D Sz = Measure(Prompt, Font(EFace::Medium, 19.f * U));
		const float Y = H * 0.76f;
		Rect(W * 0.5f - Sz.X * 0.5f - 40.f * U, Y - 8.f * U, Sz.X + 80.f * U, Sz.Y + 16.f * U, FLinearColor(0.f, 0.f, 0.f, 0.32f));
		Diamond(W * 0.5f - Sz.X * 0.5f - 22.f * U, Y + Sz.Y * 0.5f, 3.5f * U, Bronze, true);
		Diamond(W * 0.5f + Sz.X * 0.5f + 22.f * U, Y + Sz.Y * 0.5f, 3.5f * U, Bronze, true);
		Txt(Prompt, W * 0.5f, Y, EFace::Medium, 19.f, Frost, 0.5f);
	}

	// First minute: where the controls are.
	if (Clock < 40.f)
		Txt(TEXT("F1  Controls"), W - 44.f * U, H - 60.f * U, EFace::Cond, 16.f, WithA(Mist, FMath::Clamp((40.f - Clock) / 5.f, 0.f, 1.f)), 1.f);
}

// --- menu -------------------------------------------------------------------------------------

void ADragonHUD::DrawMenu(ADragonPlayerController* PC)
{
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	Rect(0, 0, W, H, FLinearColor(0.f, 0.005f, 0.01f, 0.62f));
	const float PW = 980.f * U, PH = 640.f * U, X0 = (W - PW) * 0.5f, Y0 = (H - PH) * 0.5f;
	// A click outside the panel is a click back into the game.
	if (bClick && DragSlider < 0 && !Hover(X0, Y0, PW, PH)) { bClick = false; PC->SetScreen(0); return; }
	Panel(X0, Y0, PW, PH);
	Txt(TEXT("Esc or click outside to return to the sky"), W * 0.5f, Y0 + PH + 16.f * U, EFace::Cond, 15.f, FLinearColor(0.80f, 0.86f, 0.92f, 0.55f), 0.5f);

	// Title and the knotted rule.
	Txt(TEXT("Dragon Flight"), X0 + PW * 0.5f, Y0 + 30.f * U, EFace::Title, 40.f, Frost, 0.5f);
	Img(TexRule, X0 + PW * 0.5f - 300.f * U, Y0 + 86.f * U, 600.f * U, 37.5f * U, Bronze);

	// Left: the pages.
	const float LX = X0 + 44.f * U, LY = Y0 + 150.f * U, LW = 220.f * U, LH = 44.f * U;
	static const TCHAR* Pages[] = { TEXT("Settings"), TEXT("Weather & Time"), TEXT("Controls") };
	for (int32 i = 0; i < 3; ++i)
	{
		const float Y = LY + i * (LH + 8.f * U);
		const bool bOn = PC->MenuPage == i, bHov = Hover(LX, Y, LW, LH);
		if (bOn) { Rect(LX, Y, LW, LH, FLinearColor(0.80f, 0.64f, 0.42f, 0.12f)); Line(LX, Y, LX, Y + LH, Bronze, 2.f * U); }
		Txt(Pages[i], LX + 20.f * U, Y + 9.f * U, EFace::Title, 20.f, bOn || bHov ? Frost : Mist);
		if (bHov && bClick) PC->MenuPage = i;
	}
	const float BY = Y0 + PH - 150.f * U;
	if (Button(TEXT("Resume"), LX, BY, LW, 42.f * U, false, 17.f)) { PC->SetScreen(0); return; }
	if (Button(TEXT("Quit to Desktop"), LX, BY + 54.f * U, LW, 42.f * U, false, 17.f))
	{
		UKismetSystemLibrary::QuitGame(this, PC, EQuitPreference::Quit, false);
		return;
	}

	// A hairline between the columns.
	const float DX = LX + LW + 36.f * U;
	Line(DX, LY, DX, Y0 + PH - 50.f * U, FLinearColor(1, 1, 1, 0.08f));

	const float CX = DX + 40.f * U, CW = X0 + PW - 50.f * U - CX;
	if (PC->MenuPage == 0) DrawSettings(PC, CX, LY, CW);
	else if (PC->MenuPage == 1) DrawWeather(CX, LY, CW);
	else DrawControls(CX, LY, CW);

	Txt(TEXT("Esc  back to the sky"), X0 + PW - 44.f * U, Y0 + PH - 44.f * U, EFace::Cond, 14.f, Faint, 1.f);
}

void ADragonHUD::DrawSettings(ADragonPlayerController* PC, float X, float Y, float W)
{
	UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	auto Label = [&](const TCHAR* L, float LY) { Txt(UpperSpaced(L), X, LY, EFace::Cond, 14.f, Bronze); };

	Label(TEXT("Graphics"), Y);
	TArray<FString> Q; for (const TCHAR* N : QualityNames) Q.Add(N);
	const int32 Cur = S ? FMath::Clamp(S->GetOverallScalabilityLevel(), 0, 4) : 2;
	const int32 Hit = Segmented(Q, Cur, X, Y + 24.f * U, W, 36.f * U);
	if (Hit >= 0) PC->Quality(Hit);

	Y += 84.f * U;
	Label(TEXT("Render resolution"), Y);
	TArray<FString> R; for (const TCHAR* N : ResNames) R.Add(N);
	const int32 RHit = Segmented(R, ADragonPlayerController::GetRenderResMode(), X, Y + 24.f * U, W, 36.f * U);
	if (RHit >= 0) PC->RenderRes(RHit);
	Txt(FString::Printf(TEXT("Now rendering at %s and upscaled to the screen"), *ADragonPlayerController::GetRenderResLabel()), X, Y + 66.f * U, EFace::Body, 14.f, Faint);

	Y += 106.f * U;
	Label(TEXT("Mouse sensitivity"), Y);
	float V = (ADragonPlayerController::GetMouseScale() - 0.25f) / 2.75f;
	const float Before = V;
	Slider(1, V, X, Y + 26.f * U, W - 70.f * U);
	if (V != Before) PC->SetMouseScale(0.25f + V * 2.75f);
	Txt(FString::Printf(TEXT("%.2fx"), ADragonPlayerController::GetMouseScale()), X + W, Y + 24.f * U, EFace::Cond, 17.f, Frost, 1.f);

	Y += 70.f * U;
	if (Toggle(TEXT("Invert vertical look"), ADragonPlayerController::GetInvertY(), X, Y, W)) PC->SetInvertY(!ADragonPlayerController::GetInvertY());
	Y += 38.f * U;
	if (Toggle(TEXT("Show the flight display"), PC->bShowHud, X, Y, W)) PC->bShowHud = !PC->bShowHud;
	Y += 38.f * U;
	if (S)
	{
		const bool bFull = S->GetFullscreenMode() != EWindowMode::Windowed;
		if (Toggle(TEXT("Full screen"), bFull, X, Y, W))
		{
			S->SetFullscreenMode(bFull ? EWindowMode::Windowed : EWindowMode::WindowedFullscreen);
			S->ApplySettings(false);
			S->SaveSettings();
		}
	}
}

void ADragonHUD::DrawWeather(float X, float Y, float W)
{
	ADragonSky* Sky = ADragonSky::Get(this);
	if (!Sky) return;
	auto Label = [&](const TCHAR* L, float LY) { Txt(UpperSpaced(L), X, LY, EFace::Cond, 14.f, Bronze); };

	Label(TEXT("Sky"), Y);
	struct FChoice { const TCHAR* Name; EDragonWeather Wx; };
	static const FChoice Choices[] = { { TEXT("Clear"), EDragonWeather::Clear }, { TEXT("Overcast"), EDragonWeather::Overcast },
		{ TEXT("Rain"), EDragonWeather::Rain }, { TEXT("Storm"), EDragonWeather::Storm }, { TEXT("Fog"), EDragonWeather::Fog } };
	TArray<FString> Names; int32 Cur = -1;
	const EDragonWeather Now = Sky->Weather == EDragonWeather::Fair ? EDragonWeather::Clear : Sky->Weather;
	for (int32 i = 0; i < 5; ++i) { Names.Add(Choices[i].Name); if (Choices[i].Wx == Now) Cur = i; }
	const int32 Hit = Segmented(Names, Cur, X, Y + 24.f * U, W, 36.f * U);
	if (Hit >= 0) { Sky->bAutoWeather = false; Sky->SetWeather(Choices[Hit].Wx, false, 8.f); }
	Y += 72.f * U;
	if (Toggle(TEXT("Weather changes by itself"), Sky->bAutoWeather, X, Y, W)) Sky->bAutoWeather = !Sky->bAutoWeather;

	// Time of day: a band through the day, night shaded, the sun's mark on it.
	Y += 58.f * U;
	const int32 Hh = FMath::FloorToInt(Sky->Hours), Mm = FMath::FloorToInt((Sky->Hours - Hh) * 60.f);
	Label(TEXT("Time of day"), Y);
	Txt(FString::Printf(TEXT("%02d:%02d"), Hh, Mm), X + W, Y - 8.f * U, EFace::CondBold, 26.f, Frost, 1.f);
	const float SY = Y + 34.f * U, SH = 14.f * U;
	for (int32 k = 0; k < 96; ++k)
	{
		const float Hr = (k + 0.5f) * 0.25f;
		const float Day = FMath::Clamp(FMath::Min(Hr - 4.5f, 21.5f - Hr) / 1.5f, 0.f, 1.f);
		const float Gold = FMath::Clamp(1.f - FMath::Abs(FMath::Min(FMath::Abs(Hr - 5.5f), FMath::Abs(Hr - 20.8f))) / 1.2f, 0.f, 1.f);
		FLinearColor C = FMath::Lerp(FLinearColor(0.05f, 0.07f, 0.16f, 0.9f), FLinearColor(0.45f, 0.58f, 0.70f, 0.75f), Day);
		C = FMath::Lerp(C, FLinearColor(0.85f, 0.55f, 0.32f, 0.85f), Gold * 0.7f);
		Rect(X + k * W / 96.f, SY, W / 96.f + 0.6f, SH, C);
	}
	float T = Sky->Hours / 24.f;
	if (Slider(2, T, X, SY - 4.f * U, W)) Sky->Hours = FMath::Clamp(T, 0.f, 0.9999f) * 24.f;
	for (int32 h = 0; h <= 24; h += 6)
		Txt(FString::Printf(TEXT("%02d"), h % 24), X + h / 24.f * W, SY + SH + 8.f * U, EFace::Cond, 13.f, Faint, 0.5f);

	Y = SY + 52.f * U;
	struct FQuick { const TCHAR* Name; float H; };
	static const FQuick Quick[] = { { TEXT("Dawn"), 5.4f }, { TEXT("Morning"), 9.f }, { TEXT("Noon"), 12.75f }, { TEXT("Evening"), 18.5f }, { TEXT("Sunset"), 20.6f }, { TEXT("Night"), 0.5f } };
	TArray<FString> QN; int32 QC = -1;
	for (int32 i = 0; i < 6; ++i) { QN.Add(Quick[i].Name); if (FMath::Abs(Sky->Hours - Quick[i].H) < 0.05f) QC = i; }
	const int32 QH = Segmented(QN, QC, X, Y, W, 34.f * U);
	if (QH >= 0) Sky->Hours = Quick[QH].H;
	Y += 54.f * U;
	const bool bRunning = Sky->DayLengthMinutes > 0.f;
	if (Toggle(TEXT("Time passes"), bRunning, X, Y, W))
	{
		if (bRunning) { SavedDayLength = Sky->DayLengthMinutes; Sky->DayLengthMinutes = 0.f; }
		else Sky->DayLengthMinutes = SavedDayLength > 0.f ? SavedDayLength : 48.f;
	}
}

void ADragonHUD::DrawControls(float X, float Y, float W)
{
	struct FRow { const TCHAR* Keys; const TCHAR* What; };
	static const FRow Fly[] = {
		{ TEXT("W  S"), TEXT("Faster / slower") },
		{ TEXT("A  D"), TEXT("Turn") },
		{ TEXT("Space  K"), TEXT("Climb / sink") },
		{ TEXT("Space×2  K×2"), TEXT("Zoom climb / dive (hold)") },
		{ TEXT("Shift"), TEXT("Sprint") },
		{ TEXT("J"), TEXT("Flat out  (×2 turbo)") },
		{ TEXT("L  ;"), TEXT("Knife edge left / right") },
		{ TEXT("L×2  ;×2"), TEXT("Barrel roll") },
		{ TEXT("O  P"), TEXT("Sideslip") },
	};
	static const FRow Other[] = {
		{ TEXT("Mouse"), TEXT("Look") },
		{ TEXT("LMB  U"), TEXT("Plasma blast") },
		{ TEXT("RMB  E"), TEXT("Aim") },
		{ TEXT("Hold I"), TEXT("Land  (on water: swim)") },
		{ TEXT("Space"), TEXT("Take off") },
		{ TEXT("Q  H"), TEXT("Recentre / face camera") },
		{ TEXT("Tab  T"), TEXT("Chart / weather") },
		{ TEXT("F6  F8"), TEXT("Hide display / screenshot") },
	};
	const float CW = (W - 30.f * U) * 0.5f;
	auto Column = [&](const TCHAR* Title, const FRow* Rows, int32 N, float CX)
	{
		Txt(UpperSpaced(Title), CX, Y, EFace::Cond, 14.f, Bronze);
		float RY = Y + 30.f * U;
		for (int32 i = 0; i < N; ++i)
		{
			// key caps: each key in a hairline box
			TArray<FString> Keys; FString(Rows[i].Keys).ParseIntoArray(Keys, TEXT("  "));
			float KX = CX;
			for (const FString& K : Keys)
			{
				const FVector2D Sz = Measure(K, Font(EFace::CondBold, 14.f * U));
				const float BW = FMath::Max(26.f * U, Sz.X + 14.f * U);
				Rect(KX, RY, BW, 24.f * U, FLinearColor(1, 1, 1, 0.05f));
				Line(KX, RY, KX + BW, RY, FLinearColor(1, 1, 1, 0.16f)); Line(KX, RY + 24.f * U, KX + BW, RY + 24.f * U, FLinearColor(1, 1, 1, 0.16f));
				Line(KX, RY, KX, RY + 24.f * U, FLinearColor(1, 1, 1, 0.16f)); Line(KX + BW, RY, KX + BW, RY + 24.f * U, FLinearColor(1, 1, 1, 0.16f));
				Txt(K, KX + BW * 0.5f, RY + 2.f * U, EFace::CondBold, 14.f, Frost, 0.5f, false);
				KX += BW + 5.f * U;
			}
			Txt(Rows[i].What, CX + 132.f * U, RY + 2.f * U, EFace::Body, 15.f, Mist);
			RY += 33.f * U;
		}
	};
	Column(TEXT("In the air"), Fly, UE_ARRAY_COUNT(Fly), X);
	Column(TEXT("Everything else"), Other, UE_ARRAY_COUNT(Other), X + CW + 30.f * U);
}

// --- chart ------------------------------------------------------------------------------------

void ADragonHUD::DrawMap(ADragonPawn* P, ADragonPlayerController* PC, float Dt)
{
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	UDragonWorldSubsystem* WS = GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>();
	const double S = WS ? WS->GetTerrainSize() : 4500000.0;

	Rect(0, 0, W, H, FLinearColor(0.004f, 0.008f, 0.014f, 0.94f));
	const float Sz = FMath::Min(H * 0.84f, W * 0.62f);
	const float MX = (W - Sz) * 0.5f, MY = (H - Sz) * 0.5f + 12.f * U;

	// Opening the chart centres it on the dragon at a comfortable zoom.
	const FVector Me = P->GetActorLocation();
	const FVector2D MeUV((Me.X + S * 0.5) / S, (Me.Y + S * 0.5) / S);
	if (!bMapWasOpen) { MapZoom = 1.f; MapCentre = FVector2D(0.5, 0.5); }
	bMapWasOpen = true;

	// Zoom about the cursor with the wheel; drag to pan.
	const float Span0 = 1.f / MapZoom;
	auto ToUV = [&](FVector2D Scr) { return MapCentre + (Scr - FVector2D(MX + Sz * 0.5f, MY + Sz * 0.5f)) / Sz * Span0; };
	const bool bOverMap = Hover(MX, MY, Sz, Sz);
	if (PC && bOverMap)
	{
		float Wheel = 0.f;
		if (PC->WasInputKeyJustPressed(EKeys::MouseScrollUp)) Wheel += 1.f;
		if (PC->WasInputKeyJustPressed(EKeys::MouseScrollDown)) Wheel -= 1.f;
		if (Wheel != 0.f)
		{
			const FVector2D At = ToUV(Mouse);
			const float NewZoom = FMath::Clamp(MapZoom * FMath::Pow(1.25f, Wheel), 1.f, 8.f);
			MapCentre = At + (MapCentre - At) * (MapZoom / NewZoom);
			MapZoom = NewZoom;
		}
		if (bClick) bMapDrag = true;
	}
	if (!bHeld) bMapDrag = false;
	if (bMapDrag) MapCentre -= (Mouse - LastMouse) / Sz / MapZoom;
	const float Span = 1.f / MapZoom;
	MapCentre.X = FMath::Clamp(MapCentre.X, Span * 0.5f, 1.f - Span * 0.5f);
	MapCentre.Y = FMath::Clamp(MapCentre.Y, Span * 0.5f, 1.f - Span * 0.5f);
	const FVector2D UV0 = MapCentre - FVector2D(Span * 0.5f), UV1 = MapCentre + FVector2D(Span * 0.5f);
	auto ToScreen = [&](double UVx, double UVy) { return FVector2D(MX + (UVx - UV0.X) / Span * Sz, MY + (UVy - UV0.Y) / Span * Sz); };
	auto Inside = [&](FVector2D Pt, float M) { return Pt.X > MX + M && Pt.X < MX + Sz - M && Pt.Y > MY + M && Pt.Y < MY + Sz - M; };

	// The chart, its frame and corners.
	Img(TexMap, MX, MY, Sz, Sz, FLinearColor::White, UV0, UV1);
	const FLinearColor Edge = BronzeDim;
	Line(MX - 8.f * U, MY - 8.f * U, MX + Sz + 8.f * U, MY - 8.f * U, Edge); Line(MX - 8.f * U, MY + Sz + 8.f * U, MX + Sz + 8.f * U, MY + Sz + 8.f * U, Edge);
	Line(MX - 8.f * U, MY - 8.f * U, MX - 8.f * U, MY + Sz + 8.f * U, Edge); Line(MX + Sz + 8.f * U, MY - 8.f * U, MX + Sz + 8.f * U, MY + Sz + 8.f * U, Edge);
	Line(MX, MY, MX + Sz, MY, FLinearColor(1, 1, 1, 0.10f)); Line(MX, MY + Sz, MX + Sz, MY + Sz, FLinearColor(1, 1, 1, 0.10f));
	Line(MX, MY, MX, MY + Sz, FLinearColor(1, 1, 1, 0.10f)); Line(MX + Sz, MY, MX + Sz, MY + Sz, FLinearColor(1, 1, 1, 0.10f));
	const float C = 64.f * U;
	Img(TexCorner, MX - 14.f * U, MY - 14.f * U, C, C, Bronze, FVector2D(0, 0), FVector2D(1, 1));
	Img(TexCorner, MX + Sz - C + 14.f * U, MY - 14.f * U, C, C, Bronze, FVector2D(1, 0), FVector2D(0, 1));
	Img(TexCorner, MX - 14.f * U, MY + Sz - C + 14.f * U, C, C, Bronze, FVector2D(0, 1), FVector2D(1, 0));
	Img(TexCorner, MX + Sz - C + 14.f * U, MY + Sz - C + 14.f * U, C, C, Bronze, FVector2D(1, 1), FVector2D(0, 0));

	// Island names: sized by the island, a hover shows a line about it.
	FString HoverName; FString HoverLine;
	if (WS)
	{
		const FVector2D MouseUV = ToUV(Mouse);
		for (const FDragonIsland& I : WS->GetIslands())
		{
			const FVector2D Pt = ToScreen((I.Centre.X + S * 0.5) / S, (I.Centre.Y + S * 0.5) / S);
			if (!Inside(Pt, 30.f * U)) continue;
			const float Rpx = (float)(I.Radius / S / Span * Sz);
			const float Px = FMath::Clamp(Rpx * 0.20f / U, 15.f, 30.f);
			const bool bHov = FVector2D::Distance(Mouse, Pt) < FMath::Max(Rpx * 0.7f, 26.f * U) && bOverMap && !bMapDrag;
			const FString Name = Px > 17.f ? UpperSpaced(I.Name) : I.Name;
			for (int32 o = 0; o < 8; ++o)   // a soft dark halo so names read over land and sea alike
			{
				const float a = o * PI / 4.f;
				Txt(Name, Pt.X + FMath::Cos(a) * 1.6f * U, Pt.Y - Px * 0.6f * U + FMath::Sin(a) * 1.6f * U, EFace::Title, Px, FLinearColor(0.f, 0.02f, 0.04f, 0.55f), 0.5f, false);
			}
			Txt(Name, Pt.X, Pt.Y - Px * 0.6f * U, EFace::Title, Px, bHov ? Bronze : Frost, 0.5f, false);
			if (bHov)
			{
				HoverName = I.Name;
				const double Km = FVector::Dist2D(I.Centre, Me) / 100000.0;
				HoverLine = FString::Printf(TEXT("Highest point %d m  ·  %.1f km from you"), FMath::RoundToInt(I.Peak / 100.0), Km);
			}
		}
		(void)MouseUV;
		// home: the knot at the village
		const FVector Home = WS->GetHome();
		const FVector2D HP = ToScreen((Home.X + S * 0.5) / S, (Home.Y + S * 0.5) / S);
		if (Inside(HP, 10.f * U)) Img(TexKnot, HP.X - 11.f * U, HP.Y - 11.f * U, 22.f * U, 22.f * U, Bronze);
	}

	// The dragon: a bronze-edged arrowhead pointing his way, with a slow pulse ring.
	{
		FVector2D Pt = ToScreen(MeUV.X, MeUV.Y);
		Pt.X = FMath::Clamp(Pt.X, MX + 8.f * U, MX + Sz - 8.f * U);
		Pt.Y = FMath::Clamp(Pt.Y, MY + 8.f * U, MY + Sz - 8.f * U);
		const float Yaw = FMath::DegreesToRadians(P->GetActorRotation().Yaw);
		const FVector2D F(FMath::Cos(Yaw), FMath::Sin(Yaw)), Rt(-F.Y, F.X);
		const float L = 15.f * U;
		const float Pulse = FMath::Fmod(Clock, 2.f) / 2.f;
		const float PR = (10.f + 26.f * Pulse) * U;
		for (int32 k = 0; k < 32; ++k)
		{
			const float a = k / 32.f * 2.f * PI, b = (k + 1) / 32.f * 2.f * PI;
			Line(Pt.X + PR * FMath::Cos(a), Pt.Y + PR * FMath::Sin(a), Pt.X + PR * FMath::Cos(b), Pt.Y + PR * FMath::Sin(b), WithA(Frost, 0.5f * (1.f - Pulse)), 1.2f * U);
		}
		const FVector2D Tip = Pt + F * L, BL = Pt - F * L * 0.6f + Rt * L * 0.6f, BR = Pt - F * L * 0.6f - Rt * L * 0.6f, Notch = Pt - F * L * 0.25f;
		TArray<FCanvasUVTri> Tris;
		auto Tri = [&](FVector2D A, FVector2D B, FVector2D Cc, FLinearColor Col) { FCanvasUVTri T; T.V0_Pos = A; T.V1_Pos = B; T.V2_Pos = Cc; T.V0_Color = T.V1_Color = T.V2_Color = Col; Tris.Add(T); };
		Tri(Tip, BL, Notch, Frost); Tri(Tip, Notch, BR, FLinearColor(0.78f, 0.82f, 0.86f, 0.96f));
		FCanvasTriangleItem It(Tris, GWhiteTexture); It.BlendMode = SE_BLEND_Translucent; Canvas->DrawItem(It);
		Line(Tip.X, Tip.Y, BL.X, BL.Y, Bronze, 1.5f * U); Line(BL.X, BL.Y, Notch.X, Notch.Y, Bronze, 1.5f * U);
		Line(Notch.X, Notch.Y, BR.X, BR.Y, Bronze, 1.5f * U); Line(BR.X, BR.Y, Tip.X, Tip.Y, Bronze, 1.5f * U);
	}

	// Rose, top right of the chart: the interlace ring, the knot, and north.
	{
		const float R = 62.f * U, RX = MX + Sz - R - 26.f * U, RY = MY + R + 26.f * U;
		Img(TexRing, RX - R, RY - R, 2.f * R, 2.f * R, WithA(Bronze, 0.9f));
		Img(TexKnot, RX - 20.f * U, RY - 20.f * U, 40.f * U, 40.f * U, WithA(Bronze, 0.95f));
		Txt(TEXT("N"), RX, RY - R - 28.f * U, EFace::Title, 20.f, Frost, 0.5f);
		Line(RX, RY - R + 4.f * U, RX, RY - R - 4.f * U, Frost, 1.5f * U);
	}

	// Scale bar, bottom left of the chart.
	{
		const double KmPerPx = S / 100000.0 * Span / Sz;
		const float Km = KmPerPx * 120.f * U > 4.f ? 5.f : KmPerPx * 120.f * U > 1.6f ? 2.f : KmPerPx * 120.f * U > 0.8f ? 1.f : 0.5f;
		const float L = Km / KmPerPx;
		const float BX = MX + 28.f * U, BY = MY + Sz - 30.f * U;
		Line(BX, BY, BX + L, BY, Frost, 1.5f * U);
		Line(BX, BY - 6.f * U, BX, BY + 2.f * U, Frost, 1.5f * U); Line(BX + L, BY - 6.f * U, BX + L, BY + 2.f * U, Frost, 1.5f * U);
		Txt(Km >= 1.f ? FString::Printf(TEXT("%d km"), (int32)Km) : TEXT("500 m"), BX + L * 0.5f, BY - 28.f * U, EFace::Cond, 15.f, Frost, 0.5f);
	}

	// Title above, help below.
	Txt(TEXT("Chart of the Isles"), MX, MY - 66.f * U, EFace::Title, 30.f, Frost);
	if (!HoverName.IsEmpty())
	{
		Txt(HoverName, MX + Sz, MY - 66.f * U, EFace::Title, 22.f, Bronze, 1.f);
		Txt(HoverLine, MX + Sz, MY - 36.f * U, EFace::Body, 15.f, Mist, 1.f);
	}
	Txt(TEXT("Scroll  zoom      Drag  pan      Tab  close"), W * 0.5f, MY + Sz + 22.f * U, EFace::Cond, 15.f, Faint, 0.5f);
}

// --- frame ------------------------------------------------------------------------------------

void ADragonHUD::DrawHUD()
{
	Super::DrawHUD();
	ADragonPawn* P = Cast<ADragonPawn>(GetOwningPawn());
	ADragonPlayerController* PC = Cast<ADragonPlayerController>(GetOwningPlayerController());
	if (!P || !Canvas) return;
	LoadArt();
	U = FMath::Clamp(Canvas->ClipY / 1080.f, 0.6f, 2.4f);
	const float Dt = FApp::GetDeltaTime();   // real time: the menu and chart run while paused
	Clock += Dt;

	LastMouse = Mouse;
	if (PC)
	{
		const bool bUI = PC->Screen != 0;
		if (PC->bShowMouseCursor)
		{
			float X = 0, Y = 0;
			if (PC->GetMousePosition(X, Y)) Mouse = FVector2D(X, Y);
		}
		else if (bUI)
		{
			// The game's own pointer: starts mid-screen, moved by the raw mouse movement.
			if (!bPointerWasUI) Mouse = FVector2D(Canvas->ClipX * 0.5f, Canvas->ClipY * 0.5f);
			float DX = 0.f, DY = 0.f;
			PC->GetInputMouseDelta(DX, DY);
			const float K = PointerSpeed * Canvas->ClipY / 1080.f;
			Mouse.X = FMath::Clamp(Mouse.X + DX * K, 0.f, Canvas->ClipX - 1.f);
			Mouse.Y = FMath::Clamp(Mouse.Y - DY * K, 0.f, Canvas->ClipY - 1.f);
		}
		bPointerWasUI = bUI;
		bClick = bUI && PC->WasInputKeyJustPressed(EKeys::LeftMouseButton);
		bHeld = bUI && PC->IsInputKeyDown(EKeys::LeftMouseButton);
		if (PC->Screen != 2) bMapWasOpen = false;
	}

	if (PC && PC->Screen == 2) { DrawMap(P, PC, Dt); }
	else
	{
		if (!PC || PC->bShowHud) DrawFlight(P, PC, Dt);
		if (PC && PC->Screen == 1) DrawMenu(PC);
	}

	if (PC && PC->Screen != 0 && !PC->bShowMouseCursor) DrawPointer();

	const FString Notice = DragonReport::Notice();
	if (!Notice.IsEmpty()) Txt(Notice, Canvas->ClipX * 0.5f, Canvas->ClipY * 0.1f, EFace::Medium, 18.f, Bronze, 0.5f);
}

void ADragonHUD::DrawPointer()
{
	// An arrow: dark outline, light fill, tip exactly at the hot spot.
	auto Arrow = [this](float S, const FLinearColor& C, const FVector2D& O)
	{
		const FVector2D Tip = Mouse + O;
		const FVector2D A = Tip, B = Tip + FVector2D(0.f, 19.f) * S, Cc = Tip + FVector2D(5.f, 14.5f) * S,
			D = Tip + FVector2D(13.5f, 13.5f) * S;
		TArray<FCanvasUVTri> Tris;
		FCanvasUVTri T1; T1.V0_Pos = A; T1.V1_Pos = B; T1.V2_Pos = Cc; T1.V0_Color = T1.V1_Color = T1.V2_Color = C;
		FCanvasUVTri T2; T2.V0_Pos = A; T2.V1_Pos = Cc; T2.V2_Pos = D; T2.V0_Color = T2.V1_Color = T2.V2_Color = C;
		Tris.Add(T1); Tris.Add(T2);
		FCanvasTriangleItem It(Tris, GWhiteTexture);
		It.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(It);
	};
	Arrow(1.25f * U, FLinearColor(0.f, 0.f, 0.f, 0.75f), FVector2D(-1.5f, -2.5f) * U);
	Arrow(1.0f * U, FLinearColor(0.96f, 0.94f, 0.90f, 1.f), FVector2D::ZeroVector);
}

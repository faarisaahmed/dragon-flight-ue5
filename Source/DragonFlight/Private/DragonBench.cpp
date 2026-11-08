// -Bench: a fixed flight through the world that measures the frame rate where it matters.
// Each leg: put the dragon somewhere, let streaming settle, then time 15 s of flight.
// Writes Saved/Bench.txt (Shipping has no log): avg fps, 1% low, worst, and the game / render
// / GPU thread times, so a drop can be pinned on the CPU or the GPU and on a place.

#include "DragonGameMode.h"
#include "DragonPawn.h"
#include "DragonSky.h"
#include "DragonWorldSubsystem.h"
#include "DragonFlight.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "RenderCore.h"
#include "RHI.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"

namespace
{
	struct FLeg { FString Name; FVector PosM; float Yaw; float AglM; float Hours; int32 Weather; bool bGround; float Speed; };
	TArray<FLeg> Legs;
	int32 LegIdx = -1;
	double LegT = 0.0;
	TArray<float> FrameMs;
	double GameMs = 0, RenderMs = 0, GpuMs = 0; int32 Samples = 0;
	FString Out;
	double SETTLE = 5.0, MEASURE = 15.0;
}

bool ADragonPlayerController::IsBenchmarking()
{
	static const bool b = FParse::Param(FCommandLine::Get(), TEXT("Bench"));
	return b;
}

void ADragonPlayerController::TickBench(float Dt)
{
	ADragonPawn* P = Cast<ADragonPawn>(GetPawn());
	UDragonWorldSubsystem* W = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	ADragonSky* Sky = ADragonSky::Get(this);
	if (!P || !W || !W->IsLoaded() || !Sky) return;

	if (LegIdx < 0)
	{
		static double Boot = 0.0; Boot += Dt;
		if (Boot < 10.0) return;
		FParse::Value(FCommandLine::Get(), TEXT("BenchSettle="), SETTLE);
		FParse::Value(FCommandLine::Get(), TEXT("BenchMeasure="), MEASURE);
		// Build the route: open sea, the home island low and on foot, the biggest islands, dusk, rain.
		const FVector H = W->GetHome() / 100.0;
		const FVector Sp = W->GetSpawn() / 100.0;
		Legs.Add({ TEXT("sea_high"), FVector(Sp.X, Sp.Y, 0), 0.f, 150.f, 13.f, 1, false, 40.f });
		Legs.Add({ TEXT("home_low"), FVector(H.X - 800.0, H.Y, 0), 0.f, 45.f, 13.f, 1, false, 35.f });
		Legs.Add({ TEXT("home_ground"), FVector(H.X, H.Y + 300.0, 0), 90.f, 0.f, 13.f, 1, true, 0.f });
		TArray<FDragonIsland> Is = W->GetIslands();
		Is.Sort([](const FDragonIsland& A, const FDragonIsland& B) { return A.Radius > B.Radius; });
		for (int32 i = 0; i < FMath::Min(6, Is.Num()); ++i)
		{
			const FVector C = Is[i].Centre / 100.0;
			const double R = Is[i].Radius / 100.0;
			Legs.Add({ FString::Printf(TEXT("isle_%s"), *Is[i].Name.Replace(TEXT(" "), TEXT(""))), FVector(C.X - R * 0.9, C.Y, 0), 0.f, 70.f, 13.f, 1, false, 40.f });
		}
		Legs.Add({ TEXT("home_dusk"), FVector(H.X - 800.0, H.Y, 0), 0.f, 60.f, 19.6f, 1, false, 35.f });
		Legs.Add({ TEXT("home_rain"), FVector(H.X - 800.0, H.Y, 0), 0.f, 60.f, 14.f, 3, false, 35.f });
		FString Only;
		if (FParse::Value(FCommandLine::Get(), TEXT("BenchOnly="), Only))
		{
			TArray<FString> Keep; Only.ParseIntoArray(Keep, TEXT("+"));
			Legs.RemoveAll([&](const FLeg& Lg) { for (const FString& K : Keep) if (Lg.Name.StartsWith(K)) return false; return true; });
		}
		UGameUserSettings* S = GEngine->GetGameUserSettings();
		FVector2D VSize(0, 0); if (GEngine->GameViewport) GEngine->GameViewport->GetViewportSize(VSize);
		const IConsoleVariable* CSP = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
		const IConsoleVariable* CSS = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SecondaryScreenPercentage.GameViewport"));
		Out = FString::Printf(TEXT("viewport %.0fx%.0f  screen%% %.0f  secondary%% %.0f\n"), VSize.X, VSize.Y, CSP ? CSP->GetFloat() : -1.f, CSS ? CSS->GetFloat() : -1.f);
		Out += FString::Printf(TEXT("Bench %s  preset %d  res %dx%d  %s\n%-22s %7s %7s %7s %7s %7s %7s\n"),
			*FDateTime::Now().ToString(), S ? S->GetOverallScalabilityLevel() : -1,
			S ? S->GetScreenResolution().X : 0, S ? S->GetScreenResolution().Y : 0, *GetRenderResLabel(),
			TEXT("leg"), TEXT("avg"), TEXT("1%low"), TEXT("worst"), TEXT("game"), TEXT("render"), TEXT("gpu"));
		LegIdx = 0; LegT = -1.0;
	}

	if (LegIdx >= Legs.Num()) return;
	const FLeg& L = Legs[LegIdx];
	if (LegT < 0.0)
	{
		Sky->bAutoWeather = false;
		Sky->SetWeather((EDragonWeather)L.Weather, true);
		Sky->Hours = L.Hours;
		const double G = FMath::Max(0.0, W->HeightAt(L.PosM.X * 100.0, L.PosM.Y * 100.0) / 100.0);
		if (L.bGround) P->PhotoGround(FVector(L.PosM.X, L.PosM.Y, G), L.Yaw);
		else { P->PhotoPose(FVector(L.PosM.X, L.PosM.Y, G + L.AglM), L.Yaw); P->PhotoSpeed(L.Speed); }
		ADragonPawn::FDragonScript Sc; Sc.bOn = true; Sc.WalkFwd = L.bGround ? 1.f : 0.f; Sc.Turn = L.bGround ? 0.f : 0.12f;
		P->SetScript(Sc);
		LegT = 0.0; FrameMs.Reset(); GameMs = RenderMs = GpuMs = 0; Samples = 0;
		return;
	}
	const double Prev = LegT;
	LegT += Dt;
#if !UE_BUILD_SHIPPING
	// -BenchProfile: one GPU profile per leg, into the log.
	if (Prev < SETTLE + 7.0 && LegT >= SETTLE + 7.0 && FParse::Param(FCommandLine::Get(), TEXT("BenchProfile")))
	{
		UE_LOG(LogDragon, Display, TEXT("Bench profile: %s"), *L.Name);
		ConsoleCommand(TEXT("ProfileGPU"));
	}
#endif
	if (LegT < SETTLE) return;
	FrameMs.Add(Dt * 1000.f);
	GameMs += FPlatformTime::ToMilliseconds(GGameThreadTime);
	RenderMs += FPlatformTime::ToMilliseconds(GRenderThreadTime);
	GpuMs += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
	++Samples;
	if (LegT < SETTLE + MEASURE) return;

	TArray<float> Sorted = FrameMs; Sorted.Sort();
	double Sum = 0; for (float f : FrameMs) Sum += f;
	const float Avg = Sum / FMath::Max(1, FrameMs.Num());
	const float Low = Sorted[FMath::Clamp(int32(Sorted.Num() * 0.99f), 0, Sorted.Num() - 1)];
	Out += FString::Printf(TEXT("%-22s %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f\n"), *L.Name, 1000.0 / Avg, 1000.0 / Low, Sorted.Last(),
		GameMs / Samples, RenderMs / Samples, GpuMs / Samples);
	UE_LOG(LogDragon, Display, TEXT("Bench %s: %.1f fps, 1%% low %.1f"), *L.Name, 1000.0 / Avg, 1000.0 / Low);
	++LegIdx; LegT = -1.0;
	if (LegIdx >= Legs.Num())
	{
		FFileHelper::SaveStringToFile(Out, *(FPaths::ProjectSavedDir() / TEXT("Bench.txt")));
		FFileHelper::SaveStringToFile(Out, *(FPaths::ProjectSavedDir() / FString::Printf(TEXT("Bench_%s.txt"), *FDateTime::Now().ToString())));
		UE_LOG(LogDragon, Display, TEXT("%s"), *Out);
		if (FParse::Param(FCommandLine::Get(), TEXT("BenchQuit"))) FPlatformMisc::RequestExit(false, TEXT("Bench done"));
	}
}

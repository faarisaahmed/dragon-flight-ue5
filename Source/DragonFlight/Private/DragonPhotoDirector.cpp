#include "DragonPhotoDirector.h"
#include "DragonFlight.h"
#include "DragonGameMode.h"
#include "DragonSky.h"
#include "DragonWorldSubsystem.h"
#include "DragonScatter.h"
#include "EngineUtils.h"
#include "LandscapeSubsystem.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "RenderUtils.h"
#if WITH_EDITOR
#include "ShaderCompiler.h"
#include "TextureCompiler.h"
#include "StaticMeshCompiler.h"
#endif

ADragonPhotoDirector::ADragonPhotoDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
}

void ADragonPhotoDirector::BeginPlay()
{
	Super::BeginPlay();
	OutDir = FPaths::ProjectSavedDir() / TEXT("Screenshots/Auto");
	UE_LOG(LogDragon, Display, TEXT("[Photo] platform %s: Nanite %d (runtime %d), VSM %d, Lumen GI %d"),
		*LexToString(GMaxRHIShaderPlatform), UseNanite(GMaxRHIShaderPlatform), DoesRuntimeSupportNanite(GMaxRHIShaderPlatform, true, true),
		UseVirtualShadowMaps(GMaxRHIShaderPlatform, GMaxRHIFeatureLevel), DoesPlatformSupportLumenGI(GMaxRHIShaderPlatform));
	IFileManager::Get().MakeDirectory(*OutDir, true);

	int32 Quality = 3;
	FParse::Value(FCommandLine::Get(), TEXT("AutoShotQuality="), Quality);
	if (UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		S->SetOverallScalabilityLevel(FMath::Clamp(Quality, 0, 4));
		S->ApplySettings(false);
	}

	if (ADragonPlayerController* PC = Cast<ADragonPlayerController>(UGameplayStatics::GetPlayerController(this, 0)))
	{
		PC->bShowHud = false;
		PC->bShowHelp = false;
	}
	// Hold the dragon still while shaders compile, so it is still in the air for the chase shot.
	if (APawn* P = UGameplayStatics::GetPlayerPawn(this, 0)) P->CustomTimeDilation = 0.f;

	// Photos don't need a steady frame rate: let the vegetation fill in quickly.
	for (TActorIterator<ADragonScatter> It(GetWorld()); It; ++It) It->BudgetMs = 60.f;
	Camera = GetWorld()->SpawnActor<ACameraActor>();
	if (Camera)
	{
		Camera->GetCameraComponent()->SetFieldOfView(60.f);
		Camera->GetCameraComponent()->bConstrainAspectRatio = false;
	}
	PlanShots();
	// -AutoShotOnly=00+05 keeps only shots whose names start with those prefixes.
	FString Only;
	if (FParse::Value(FCommandLine::Get(), TEXT("AutoShotOnly="), Only))
	{
		TArray<FString> Keep; Only.ParseIntoArray(Keep, TEXT("+"));
		Shots.RemoveAll([&](const FShot& S) { for (const FString& K : Keep) if (S.Name.StartsWith(K)) return false; return true; });
	}
	FParse::Value(FCommandLine::Get(), TEXT("AutoShotTag="), Tag);
	UE_LOG(LogDragon, Display, TEXT("[Photo] %d shots planned -> %s"), Shots.Num(), *OutDir);
}

void ADragonPhotoDirector::PlanShots()
{
	Shots.Reset();
	FShot Chase; Chase.Name = TEXT("00_chase"); Chase.bChase = true; Chase.Hours = 15.5f;
	Shots.Add(Chase);
	FShot Close; Close.Name = TEXT("00b_dragon"); Close.bDragonClose = true; Close.Hours = 15.5f;
	Shots.Add(Close);

	const UDragonWorldSubsystem* W = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	if (!W) return;

	TArray<FDragonIsland> Isl = W->GetIslands();
	Isl.Sort([](const FDragonIsland& A, const FDragonIsland& B) { return A.Radius > B.Radius; });

	auto Look = [](const FVector& From, const FVector& To) { return (To - From).Rotation(); };
	const float Hours[] = { 15.5f, 19.4f, 11.0f, 17.5f, 6.2f, 14.0f };
	const int32 N = FMath::Min(Isl.Num(), 6);
	for (int32 i = 0; i < N; ++i)
	{
		const FDragonIsland& I = Isl[i];
		const double Ang = FMath::DegreesToRadians(200.0 + i * 67.0);
		const FVector Dir(FMath::Cos(Ang), FMath::Sin(Ang), 0.0);
		const double Top = FMath::Max(I.Centre.Z, 5000.0);

		// Aerial vista: the whole island, a dragon's-eye height.
		FShot A;
		A.Name = FString::Printf(TEXT("%02d_aerial_%s"), Shots.Num(), *I.Name.Replace(TEXT(" "), TEXT("")));
		A.Location = FVector(I.Centre.X, I.Centre.Y, 0.0) + Dir * I.Radius * 1.7 + FVector(0, 0, Top * 0.6 + 12000.0);
		A.Rotation = Look(A.Location, FVector(I.Centre.X, I.Centre.Y, Top * 0.35));
		A.Hours = Hours[i % UE_ARRAY_COUNT(Hours)];
		Shots.Add(A);

		// Low coast: just above the waves, looking up at the shore.
		if (i < 3)
		{
			FShot C;
			C.Name = FString::Printf(TEXT("%02d_coast_%s"), Shots.Num(), *I.Name.Replace(TEXT(" "), TEXT("")));
			C.Location = FVector(I.Centre.X, I.Centre.Y, 0.0) + Dir.RotateAngleAxis(25.0, FVector::UpVector) * I.Radius * 1.15 + FVector(0, 0, 2500.0);
			C.Rotation = Look(C.Location, FVector(I.Centre.X, I.Centre.Y, Top * 0.25));
			C.Hours = Hours[(i + 1) % UE_ARRAY_COUNT(Hours)];
			Shots.Add(C);
		}
	}

	// Home: the village bay from a hillside.
	const FVector Home = W->GetHome();
	FShot H;
	H.Name = FString::Printf(TEXT("%02d_home"), Shots.Num());
	H.Location = Home + FVector(-60000.0, -40000.0, 0.0);
	H.Location.Z = W->HeightAt(H.Location.X, H.Location.Y) + 15000.0;
	H.Rotation = Look(H.Location, Home);
	H.Hours = 16.5f;
	Shots.Add(H);
	// Ground: standing in the grass above the village, eye height, looking across the land.
	for (int32 k = 0; k < 48; ++k)
	{
		const double A = k * 0.7, R = 25000.0 + k * 6000.0;
		const FVector P = Home + FVector(FMath::Cos(A) * R, FMath::Sin(A) * R, 0.0);
		const double Z = W->HeightAt(P.X, P.Y);
		if (Z < 800.0 || Z > 15000.0) continue;
		// Gentle ground only: compare heights 20 m around.
		double Mx = Z, Mn = Z;
		for (int32 d = 0; d < 4; ++d)
		{
			const double Zd = W->HeightAt(P.X + (d == 0 ? 2000 : d == 1 ? -2000 : 0), P.Y + (d == 2 ? 2000 : d == 3 ? -2000 : 0));
			Mx = FMath::Max(Mx, Zd); Mn = FMath::Min(Mn, Zd);
		}
		if (Mx - Mn > 700.0) continue;
		FShot G;
		G.Name = TEXT("98_ground");
		G.Location = FVector(P.X, P.Y, Z + 170.0);
		FRotator Rt = Look(G.Location, FVector(Home.X, Home.Y, G.Location.Z));
		Rt.Pitch = -6.f;
		G.Rotation = Rt;
		G.Hours = 16.0f;
		Shots.Add(G);
		break;
	}
	// River: the longest one, from above its middle reach, looking downstream.
	{
		const FDragonRiver* Best = nullptr;
		for (const FDragonRiver& R : W->GetRivers()) if (!Best || R.Points.Num() > Best->Points.Num()) Best = &R;
		if (Best && Best->Points.Num() > 6)
		{
			const int32 Mid = Best->Points.Num() / 3;
			const FVector A = Best->Points[Mid], B = Best->Points[FMath::Min(Mid + 4, Best->Points.Num() - 1)];
			FVector Dir = B - A; Dir.Z = 0; Dir.Normalize();
			FShot Rv;
			Rv.Name = TEXT("97_river");
			Rv.Location = A - Dir * 8000.0 + FVector(0, 0, 0);
			Rv.Location.Z = FMath::Max(W->HeightAt(Rv.Location.X, Rv.Location.Y), A.Z) + 6000.0;
			Rv.Rotation = Look(Rv.Location, B);
			Rv.Hours = 14.5f;
			Shots.Add(Rv);
		}
	}
	// The chase again, last, once everything the dragon uses has certainly compiled.
	FShot Late = Chase; Late.Name = TEXT("99_chase_late");
	Shots.Add(Late);
}

bool ADragonPhotoDirector::Busy() const
{
#if WITH_EDITOR
	// Textures and meshes compile on first use in an editor build; until they finish
	// the renderer shows placeholders (the grey checker), so a shot would be a lie.
	if (GShaderCompilingManager && GShaderCompilingManager->GetNumRemainingJobs() > 0) return true;
	if (FTextureCompilingManager::Get().GetNumRemainingTextures() > 0) return true;
	if (FStaticMeshCompilingManager::Get().GetNumRemainingMeshes() > 0) return true;
#endif
	return false;
}

void ADragonPhotoDirector::Frame(const FShot& S)
{
	if (ADragonSky* Sky = ADragonSky::Get(this))
	{
		Sky->Hours = S.Hours;
		Sky->DayLengthMinutes = 0.f;      // hold the light still for the shot
		Sky->bAutoWeather = false;
		Sky->SetWeather(EDragonWeather::Fair, true);
	}
	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (!PC) return;
	if (S.bDragonClose && Camera)
	{
		if (APawn* P = PC->GetPawn())
		{
			P->CustomTimeDilation = 0.f; // hold the pose for a sharp portrait
			const FVector At = P->GetActorLocation() + FVector(0, 0, 100);
			const FVector F = P->GetActorForwardVector(), R = P->GetActorRightVector();
			const FVector From = At + F * 1100.0 + R * 850.0 + FVector(0, 0, 260);
			Camera->SetActorLocationAndRotation(From, (At - From).Rotation());
			Camera->GetCameraComponent()->SetFieldOfView(40.f);
			PC->SetViewTarget(Camera);
		}
	}
	else if (S.bChase)
	{
		if (APawn* P = PC->GetPawn())
		{
			P->CustomTimeDilation = 1.f;
			PC->SetViewTarget(P);
		}
	}
	else if (Camera)
	{
		Camera->GetCameraComponent()->SetFieldOfView(60.f);
		Camera->SetActorLocationAndRotation(S.Location, S.Rotation);
		PC->SetViewTarget(Camera);
	}
	// Diagnostics: what is under the view, and how high.
	FVector At = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : S.Location;
	if (!S.bChase) At = S.Location;
	TArray<FHitResult> Hits;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(PhotoProbe), true);
	GetWorld()->LineTraceMultiByChannel(Hits, FVector(At.X, At.Y, 300000.0), FVector(At.X, At.Y, -100000.0), ECC_Visibility, Q);
	FString Under;
	for (const FHitResult& H : Hits) Under += FString::Printf(TEXT(" [%s z=%.1fm]"), H.GetActor() ? *H.GetActor()->GetName() : TEXT("?"), H.ImpactPoint.Z / 100.0);
	const UDragonWorldSubsystem* W = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	UE_LOG(LogDragon, Display, TEXT("[Photo] %s cam z=%.0fm, heightmap z=%.1fm, traces:%s"), *S.Name, At.Z / 100.0,
		W ? W->HeightAt(At.X, At.Y) / 100.0 : -1.0, *Under);
}

void ADragonPhotoDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Busy())
	{
		Warmup = 0.f;
		static double LastLog = 0.0;
		const double Now = FPlatformTime::Seconds();
#if WITH_EDITOR
		if (Now - LastLog > 30.0 && GShaderCompilingManager)
		{
			LastLog = Now;
			UE_LOG(LogDragon, Display, TEXT("[Photo] waiting: %d shaders, %d textures, %d meshes"), GShaderCompilingManager->GetNumRemainingJobs(),
				FTextureCompilingManager::Get().GetNumRemainingTextures(), FStaticMeshCompilingManager::Get().GetNumRemainingMeshes());
		}
#endif
		return;
	}
	Warmup += DeltaSeconds;
	if (Warmup < 5.f) return;

	if (Index < 0)
	{
		Index = 0;
		Timer = 0.f;
		bShotRequested = false;
		Frame(Shots[0]);
		return;
	}
	if (!Shots.IsValidIndex(Index))
	{
		UE_LOG(LogDragon, Display, TEXT("[Photo] all shots done"));
		FPlatformMisc::RequestExit(false, TEXT("AutoShot done"));
		SetActorTickEnabled(false);
		return;
	}

	// Hold each view until the vegetation around it has been generated.
	bool bScatterBusy = false;
	for (TActorIterator<ADragonScatter> It(GetWorld()); It; ++It) bScatterBusy |= It->IsBusy();
	if (!bShotRequested && bScatterBusy && Timer < 60.f) { Timer += DeltaSeconds * 0.25f; return; }
	Timer += DeltaSeconds;
	const FShot& S = Shots[Index];
	const float Settle = S.bChase ? 4.f : SettleSeconds;
	// Landscape grass builds lazily around the camera; build this view's synchronously.
	if (!bShotRequested && !bGrassBuilt && Timer >= Settle - 2.f)
	{
		bGrassBuilt = true;
		if (ULandscapeSubsystem* LS = GetWorld()->GetSubsystem<ULandscapeSubsystem>())
		{
			const APlayerController* PC = GetWorld()->GetFirstPlayerController();
			TArray<FVector> Cams;
			if (PC && PC->PlayerCameraManager) Cams.Add(PC->PlayerCameraManager->GetCameraLocation());
			LS->RegenerateGrass(false, true, Cams.Num() ? TOptional<TArrayView<FVector>>(TArrayView<FVector>(Cams)) : TOptional<TArrayView<FVector>>());
		}
	}
	if (!bShotRequested && Timer >= Settle)
	{
		const FString File = OutDir / (S.Name + Tag + TEXT(".png"));
		FScreenshotRequest::RequestScreenshot(File, false, false);
		bShotRequested = true;
		UE_LOG(LogDragon, Display, TEXT("[Photo] %s"), *File);
	}
	else if (bShotRequested && Timer >= Settle + 1.5f)
	{
		++Index;
		Timer = 0.f;
		bShotRequested = false;
		bGrassBuilt = false;
		if (Shots.IsValidIndex(Index)) Frame(Shots[Index]);
	}
}

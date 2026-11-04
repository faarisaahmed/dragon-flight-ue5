#include "DragonPhotoDirector.h"
#include "DragonFlight.h"
#include "DragonGameMode.h"
#include "DragonSky.h"
#include "DragonWorldSubsystem.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
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

	Camera = GetWorld()->SpawnActor<ACameraActor>();
	if (Camera)
	{
		Camera->GetCameraComponent()->SetFieldOfView(60.f);
		Camera->GetCameraComponent()->bConstrainAspectRatio = false;
	}
	PlanShots();
	UE_LOG(LogDragon, Display, TEXT("[Photo] %d shots planned -> %s"), Shots.Num(), *OutDir);
}

void ADragonPhotoDirector::PlanShots()
{
	Shots.Reset();
	FShot Chase; Chase.Name = TEXT("00_chase"); Chase.bChase = true; Chase.Hours = 15.5f;
	Shots.Add(Chase);

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
	if (S.bChase)
	{
		if (APawn* P = PC->GetPawn())
		{
			P->CustomTimeDilation = 1.f;
			PC->SetViewTarget(P);
		}
	}
	else if (Camera)
	{
		Camera->SetActorLocationAndRotation(S.Location, S.Rotation);
		PC->SetViewTarget(Camera);
	}
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

	Timer += DeltaSeconds;
	const FShot& S = Shots[Index];
	const float Settle = S.bChase ? 4.f : SettleSeconds;
	if (!bShotRequested && Timer >= Settle)
	{
		const FString File = OutDir / (S.Name + TEXT(".png"));
		FScreenshotRequest::RequestScreenshot(File, false, false);
		bShotRequested = true;
		UE_LOG(LogDragon, Display, TEXT("[Photo] %s"), *File);
	}
	else if (bShotRequested && Timer >= Settle + 1.5f)
	{
		++Index;
		Timer = 0.f;
		bShotRequested = false;
		if (Shots.IsValidIndex(Index)) Frame(Shots[Index]);
	}
}

#include "DragonPhotoDirector.h"
#include "DragonPawn.h"
#include "DragonFlight.h"
#include "DragonGameMode.h"
#include "DragonSky.h"
#include "DragonWorldSubsystem.h"
#include "DragonScatter.h"
#include "DragonPlasma.h"
#include "Dom/JsonObject.h"
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
	// Village: the densest cluster of house plots, from a low hillside 150 m off.
	if (const TSharedPtr<FJsonObject> J = W->GetJson())
	{
		const TArray<TSharedPtr<FJsonValue>>* Plots = nullptr;
		if (J->TryGetArrayField(TEXT("village"), Plots) && Plots->Num() > 0)
		{
			TArray<FVector> Pts;
			for (const TSharedPtr<FJsonValue>& V : *Plots)
				if (const TSharedPtr<FJsonObject> P = V->AsObject())
					Pts.Add(FVector(P->GetNumberField(TEXT("x")) * 100.0, P->GetNumberField(TEXT("z")) * 100.0, P->GetNumberField(TEXT("h")) * 100.0));
			int32 BestI = 0, BestN = -1;
			for (int32 i = 0; i < Pts.Num(); i += 3)
			{
				int32 Near = 0;
				for (const FVector& Q : Pts) Near += FVector::Dist2D(Q, Pts[i]) < 15000.0;
				if (Near > BestN) { BestN = Near; BestI = i; }
			}
			const FVector C = Pts[BestI];
			for (int32 k = 0; k < 8; ++k)
			{
				const double A = k * PI / 4.0;
				FVector L = C + FVector(FMath::Cos(A), FMath::Sin(A), 0.0) * 15000.0;
				const double G = W->HeightAt(L.X, L.Y);
				if (G < 0.0) continue;
				L.Z = FMath::Max(G, C.Z) + 4500.0;
				FShot Vg; Vg.Name = TEXT("96_village"); Vg.Location = L; Vg.Rotation = Look(L, C); Vg.Hours = 15.0f;
				Shots.Add(Vg);
				break;
			}
		}
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
	// Plasma: a bolt bursting on a hillside above the home bay (late afternoon and dusk), and
	// one in flight, from about where the chase camera would be.
	{
		FVector T = Home + FVector(-30000.0, -22000.0, 0.0);
		T.Z = W->HeightAt(T.X, T.Y);
		if (T.Z < 300.0) { T = Home; T.Z = W->HeightAt(T.X, T.Y); }
		const FVector In = FVector(-16000.0, 14000.0, 11000.0).GetSafeNormal();     // toward where the bolt comes from
		const FVector Side = FVector::CrossProduct(In, FVector::UpVector).GetSafeNormal();
		FShot Pb; Pb.Name = TEXT("20_plasma_blast"); Pb.bPlasma = true; Pb.Target = T; Pb.PlasmaLead = 0.22f;
		Pb.Location = T + In * 8000.0 + Side * 2500.0 + FVector(0, 0, 600.0);
		Pb.Rotation = Look(Pb.Location, T + FVector(0, 0, 500.0)); Pb.Hours = 17.5f;
		Shots.Add(Pb);
		FShot Pl = Pb; Pl.Name = TEXT("20a_plasma_late"); Pl.PlasmaLead = 0.9f;   // fire and smoke a second on
		Shots.Add(Pl);
		FShot Pd = Pb; Pd.Name = TEXT("20b_plasma_dusk"); Pd.Hours = 21.6f; Pd.PlasmaLead = 0.12f;
		Shots.Add(Pd);
		// In flight: the camera beside the path, 60 m short of where the bolt is at the shutter.
		FShot Pf = Pb; Pf.Name = TEXT("20c_plasma_flight"); Pf.PlasmaLead = -0.18f; Pf.Hours = 16.f;
		const FVector AtShutter = T + In * 24000.0 * (0.18f / 0.4f);
		Pf.Location = AtShutter + In * 4500.0 + Side * 1800.0 + FVector(0, 0, 400.0);
		Pf.Rotation = Look(Pf.Location, AtShutter);
		Shots.Add(Pf);
	}
	// The cold north: the two most arctic islands of size.
	{
		TArray<FDragonIsland> Cold = W->GetIslands();
		Cold.Sort([W](const FDragonIsland& A, const FDragonIsland& B) { return W->ColdAt(A.Centre.X, A.Centre.Y) > W->ColdAt(B.Centre.X, B.Centre.Y); });
		for (int32 i = 0; i < FMath::Min(2, Cold.Num()); ++i)
		{
			const FDragonIsland& I = Cold[i];
			FShot A; A.Name = FString::Printf(TEXT("21_arctic_%s"), *I.Name.Replace(TEXT(" "), TEXT("")));
			const FVector Dir(FMath::Cos(2.2 + i), FMath::Sin(2.2 + i), 0.0);
			A.Location = FVector(I.Centre.X, I.Centre.Y, 0.0) + Dir * I.Radius * 1.6 + FVector(0, 0, 32000.0);
			A.Rotation = Look(A.Location, FVector(I.Centre.X, I.Centre.Y, 8000.0)); A.Hours = 14.f;
			Shots.Add(A);
		}
	}
	// One island of each character (World.json "biomes"): volcanic, woodland, moorland.
	if (const TSharedPtr<FJsonObject> J = W->GetJson())
	{
		const TSharedPtr<FJsonObject>* Bio = nullptr;
		if (J->TryGetObjectField(TEXT("biomes"), Bio))
		{
			TSet<FString> Done;
			for (const FDragonIsland& I : W->GetIslands())
			{
				FString B;
				if (!(*Bio)->TryGetStringField(I.Name, B) || Done.Contains(B)) continue;
				Done.Add(B);
				FShot A; A.Name = FString::Printf(TEXT("24_%s_%s"), *B, *I.Name.Replace(TEXT(" "), TEXT("")).Replace(TEXT("'"), TEXT("")));
				const FVector Dir(FMath::Cos(0.8 + Done.Num()), FMath::Sin(0.8 + Done.Num()), 0.0);
				A.Location = FVector(I.Centre.X, I.Centre.Y, 0.0) + Dir * I.Radius * 1.25 + FVector(0, 0, 0);
				A.Location.Z = FMath::Max(0.0, W->HeightAt(A.Location.X, A.Location.Y)) + 26000.0;
				A.Rotation = Look(A.Location, FVector(I.Centre.X, I.Centre.Y, 6000.0)); A.Hours = 14.5f;
				Shots.Add(A);
			}
		}
	}
	// Showcase: the player's view flying in on one island of each kind (home, volcanic, woodland,
	// moorland, the arctic), and the dragon firing at a sea cliff, in the light that suits each.
	{
		TArray<const FDragonIsland*> Pick;
		TArray<FString> Kind;
		auto AddIsle = [&](const FDragonIsland* I, const FString& K) { if (I && !Pick.Contains(I)) { Pick.Add(I); Kind.Add(K); } };
		const FDragonIsland* HomeIsle = nullptr;
		for (const FDragonIsland& I : W->GetIslands())
			if (!HomeIsle || FVector::Dist2D(I.Centre, Home) < FVector::Dist2D(HomeIsle->Centre, Home)) HomeIsle = &I;
		AddIsle(HomeIsle, TEXT("home"));
		if (const TSharedPtr<FJsonObject> J = W->GetJson())
		{
			const TSharedPtr<FJsonObject>* Bio = nullptr;
			if (J->TryGetObjectField(TEXT("biomes"), Bio))
			{
				TSet<FString> Done;
				for (const FDragonIsland& I : W->GetIslands())
				{
					FString B;
					if ((*Bio)->TryGetStringField(I.Name, B) && !Done.Contains(B) && I.Radius > 60000.0) { Done.Add(B); AddIsle(&I, B); }
				}
			}
		}
		{
			const FDragonIsland* Cold = nullptr;
			for (const FDragonIsland& I : W->GetIslands())
				if (I.Radius > 60000.0 && (!Cold || W->ColdAt(I.Centre.X, I.Centre.Y) > W->ColdAt(Cold->Centre.X, Cold->Centre.Y))) Cold = &I;
			AddIsle(Cold, TEXT("arctic"));
		}
		const float Light[] = { 17.5f, 19.8f, 15.0f, 18.5f, 14.0f, 16.5f };
		for (int32 k = 0; k < Pick.Num(); ++k)
		{
			const FDragonIsland& I = *Pick[k];
			const FString Nm = I.Name.Replace(TEXT(" "), TEXT("")).Replace(TEXT("'"), TEXT(""));
			// Of 16 bearings, the one whose approach is the widest open water (no fjords), then walk
			// out from the centre to the coast along it.
			auto CoastAlong = [&](const FVector& Dir)
			{
				FVector C = FVector(I.Centre.X, I.Centre.Y, 0.0);
				for (double r = 0.0; r < I.Radius * 3.0; r += 1000.0)
				{
					C = FVector(I.Centre.X, I.Centre.Y, 0.0) + Dir * r;
					if (r > I.Radius * 0.3 && W->HeightAt(C.X, C.Y) < -200.0) break;
				}
				return C;
			};
			FVector D(1, 0, 0); double BestScore = -1e9;
			for (int32 a = 0; a < 16; ++a)
			{
				const double Ang = 0.3 * k + a * UE_DOUBLE_TWO_PI / 16.0;
				const FVector Dir(FMath::Cos(Ang), FMath::Sin(Ang), 0.0);
				const FVector Side(-Dir.Y, Dir.X, 0.0);
				const FVector C = CoastAlong(Dir);
				double Score = 0.0;
				for (double Out = 5000.0; Out <= 60000.0; Out += 5000.0)
					for (double Sd = -40000.0; Sd <= 40000.0; Sd += 10000.0)
					{
						const FVector Q = C + Dir * Out + Side * Sd;
						Score += W->HeightAt(Q.X, Q.Y) < -500.0 ? 1.0 : 0.0;
					}
				// and something worth looking at: high ground just inland
				const FVector In = C - Dir * 20000.0;
				Score += FMath::Clamp(W->HeightAt(In.X, In.Y) / 2000.0, 0.0, 15.0);
				if (Score > BestScore) { BestScore = Score; D = Dir; }
			}
			const FVector Coast = CoastAlong(D);
			// Flying in, angled 18 degrees off the centre line so the coast runs across the view.
			FShot Fl; Fl.Name = FString::Printf(TEXT("30_fly_%s_%s"), *Kind[k], *Nm); Fl.bChase = true; Fl.bPose = true;
			Fl.Hours = Light[k % 6];
			const FVector Start = Coast + D * 90000.0;
			Fl.PoseM = FVector(Start.X, Start.Y, 13000.0) / 100.0;
			Fl.PoseYaw = (-D).Rotation().Yaw + 18.f;
			Shots.Add(Fl);
			// Firing at the cliffs: nearer in, the bolt bursting on the first high ground ahead.
			FShot Fi = Fl; Fi.Name = FString::Printf(TEXT("31_fire_%s_%s"), *Kind[k], *Nm); Fi.bDragonFires = true; Fi.PlasmaLead = 0.15f;
			const FVector Near = Coast + D * 30000.0;
			Fi.PoseM = FVector(Near.X, Near.Y, 7000.0) / 100.0;
			Fi.PoseYaw = (-D).Rotation().Yaw;
			FVector T = Coast;
			for (double r = 0.0; r < I.Radius; r += 1000.0)
			{
				T = Coast - D * r;
				if (W->HeightAt(T.X, T.Y) > 1500.0) break;
			}
			T.Z = W->HeightAt(T.X, T.Y) + 800.0;
			Fi.Target = T;
			Shots.Add(Fi);
			// Low over the island itself: turf, rocks, shrubs and trees from the player's height.
			{
				FShot Ov = Fl; Ov.Name = FString::Printf(TEXT("32_over_%s_%s"), *Kind[k], *Nm);
				const FVector In = Coast - D * 45000.0;
				Ov.PoseM = FVector(In.X, In.Y, FMath::Max(0.0, W->HeightAt(In.X, In.Y)) + 4500.0) / 100.0;
				Ov.PoseYaw = (-D).Rotation().Yaw + 35.f;
				Shots.Add(Ov);
			}
			if (k == 0)
			{
				// Mechanics, scripted: skimming the sea, hovering over it, landing, walking, swimming.
				const FVector Sea = Coast + D * 60000.0;                  // 600 m out: open water
				const float Along = (-D).Rotation().Yaw + 90.f;            // parallel to the coast
				FShot Sk = Fl; Sk.Name = TEXT("40_skim_knife"); Sk.PoseM = FVector(Sea.X, Sea.Y, 900.0) / 100.0; Sk.PoseYaw = Along;
				Sk.PoseSpeed = 34.f; Sk.Script.bOn = true; Sk.Script.Move = 1.f; Sk.Script.Vertical = -0.45f; Sk.Script.Knife = 1; Sk.SettleS = 2.0f; Sk.Hours = 16.5f;
				Shots.Add(Sk);
				FShot Sb = Sk; Sb.Name = TEXT("40b_skim_feet"); Sb.Script.Knife = 0; Sb.Script.Vertical = -0.5f; Sb.PoseM.Z = 3.0; Sb.SettleS = 3.f;
				Shots.Add(Sb);
				FShot Hv = Sk; Hv.Name = TEXT("41_hover_water"); Hv.PoseM.Z = 4.0; Hv.PoseSpeed = 0.f; Hv.Script.Move = 0.f; Hv.Script.Knife = 0; Hv.Script.Vertical = 0.f; Hv.SettleS = 4.5f;
				Shots.Add(Hv);
				// A gentle patch of turf inland for the landing and the walk.
				FVector Turf = Coast - D * 45000.0;
				for (int32 Ring = 0; Ring < 30; ++Ring)
				{
					bool bOk = false;
					for (int32 a = 0; a < 12 && !bOk; ++a)
					{
						const FVector Q = Coast - D * 45000.0 + FVector(FMath::Cos(a * 0.5236), FMath::Sin(a * 0.5236), 0.0) * (Ring * 4000.0);
						const double G0 = W->HeightAt(Q.X, Q.Y);
						const double Gx = W->HeightAt(Q.X + 1500.0, Q.Y), Gy = W->HeightAt(Q.X, Q.Y + 1500.0), Gx2 = W->HeightAt(Q.X - 1500.0, Q.Y);
						if (G0 > 800.0 && FMath::Abs(Gx - G0) < 250.0 && FMath::Abs(Gy - G0) < 250.0 && FMath::Abs(Gx2 - G0) < 250.0) { Turf = Q; bOk = true; }
					}
					if (bOk) break;
				}
				const double TurfZ = W->HeightAt(Turf.X, Turf.Y) / 100.0;
				FShot La = Fl; La.Name = TEXT("42a_land_flare"); La.PoseM = FVector(Turf.X / 100.0, Turf.Y / 100.0, TurfZ + 16.0); La.PoseYaw = (-D).Rotation().Yaw + 35.f;
				La.PoseSpeed = 12.f; La.Script.bOn = true; La.Script.bLand = true; La.SettleS = 1.6f; La.Hours = 15.5f;
				Shots.Add(La);
				FShot Lb = La; Lb.Name = TEXT("42b_land_touch"); Lb.SettleS = 3.4f; Shots.Add(Lb);
				FShot Lc = La; Lc.Name = TEXT("42c_land_settled"); Lc.SettleS = 6.f; Shots.Add(Lc);
				FShot Wa = La; Wa.Name = TEXT("43_walk"); Wa.bPoseGround = true; Wa.PoseM.Z = TurfZ; Wa.Script = ADragonPawn::FDragonScript(); Wa.Script.bOn = true; Wa.Script.WalkFwd = 1.f; Wa.SettleS = 4.f;
				Shots.Add(Wa);
				FShot Ga = Wa; Ga.Name = TEXT("43b_gallop"); Ga.Script.bSprint = true; Ga.SettleS = 5.f; Shots.Add(Ga);
				// Standing in thick forest: the trees between the camera and the dragon must clear.
				if (TActorIterator<ADragonScatter> It(GetWorld()); It)
				{
					FVector Best = FVector::ZeroVector; float BestF = 0.f;
					for (int32 Ring = 1; Ring < 40 && BestF < 0.85f; ++Ring)
						for (int32 a = 0; a < 16; ++a)
						{
							const FVector Q = Turf + FVector(FMath::Cos(a * 0.3927), FMath::Sin(a * 0.3927), 0.0) * (Ring * 5000.0);
							const double G0 = W->HeightAt(Q.X, Q.Y);
							if (G0 < 1000.0 || FMath::Abs(W->HeightAt(Q.X + 1500.0, Q.Y) - G0) > 400.0 || FMath::Abs(W->HeightAt(Q.X, Q.Y + 1500.0) - G0) > 400.0) continue;
							const float Fo = It->ForestAt(Q.X, Q.Y);
							if (Fo > BestF) { BestF = Fo; Best = Q; }
						}
					if (BestF > 0.3f)
					{
						FShot Fs = Wa; Fs.Name = TEXT("45_forest"); Fs.PoseM = FVector(Best.X / 100.0, Best.Y / 100.0, W->HeightAt(Best.X, Best.Y) / 100.0);
						Fs.Script = ADragonPawn::FDragonScript(); Fs.Script.bOn = true; Fs.SettleS = 5.f;
						Shots.Add(Fs);
						FShot Fw = Fs; Fw.Name = TEXT("45b_forest_walk"); Fw.Script.WalkFwd = 1.f; Fw.SettleS = 5.f; Shots.Add(Fw);
					}
					UE_LOG(LogDragon, Display, TEXT("[Photo] forest pose: cover %.2f"), BestF);
				}
				const FVector Bay = Coast + D * 15000.0;
				FShot Sw = Sk; Sw.Name = TEXT("44_swim"); Sw.PoseM = FVector(Bay.X, Bay.Y, 500.0) / 100.0; Sw.PoseSpeed = 4.f; Sw.Script = ADragonPawn::FDragonScript();
				Sw.Script.bOn = true; Sw.Script.bLand = true; Sw.Script.WalkFwd = 1.f; Sw.SettleS = 8.f; Shots.Add(Sw);

				// The same coast in rain, and in a storm, from the player's seat.
				FShot Rn = Fl; Rn.Name = FString::Printf(TEXT("33_rain_%s"), *Nm); Rn.Weather = (uint8)EDragonWeather::Rain;
				Shots.Add(Rn);
				FShot Sm = Fi; Sm.Name = FString::Printf(TEXT("33b_storm_%s"), *Nm); Sm.Weather = (uint8)EDragonWeather::Storm; Sm.bDragonFires = false;
				Shots.Add(Sm);
			}
			if (k < 2)
			{
				FShot Fa = Fi; Fa.Name = FString::Printf(TEXT("31c_after_%s_%s"), *Kind[k], *Nm); Fa.PlasmaLead = 1.0f;   // fire and smoke a second on
				Shots.Add(Fa);
				FShot Fm = Fi; Fm.Name = FString::Printf(TEXT("31b_bolt_%s_%s"), *Kind[k], *Nm); Fm.PlasmaLead = -0.3f;   // the bolt on its way
				Shots.Add(Fm);
			}
		}
	}
	// Weather: a storm rolling over the home coast, and rain.
	{
		FShot St; St.Name = TEXT("22_storm"); St.Weather = (uint8)EDragonWeather::Storm; St.Hours = 15.f;
		St.Location = Home + FVector(-90000.0, 60000.0, 0.0);
		St.Location.Z = FMath::Max(0.0, W->HeightAt(St.Location.X, St.Location.Y)) + 22000.0;   // above the ground, wherever that is
		St.Rotation = Look(St.Location, Home + FVector(0, 0, 8000.0));
		Shots.Add(St);
		FShot Rn = St; Rn.Name = TEXT("22b_rain"); Rn.Weather = (uint8)EDragonWeather::Rain;
		Shots.Add(Rn);
		// Evening and dusk in each kind of weather (what F3/F4 get to in play).
		struct FEve { const TCHAR* N; EDragonWeather Wx; float H; };
		const FEve Eve[] = {
			{ TEXT("fair_1830"), EDragonWeather::Fair, 18.5f }, { TEXT("fair_1930"), EDragonWeather::Fair, 19.5f },
			{ TEXT("fair_2030"), EDragonWeather::Fair, 20.5f }, { TEXT("over_1930"), EDragonWeather::Overcast, 19.5f },
			{ TEXT("rain_1930"), EDragonWeather::Rain, 19.5f }, { TEXT("fog_1830"), EDragonWeather::Fog, 18.5f },
			{ TEXT("fog_2000"), EDragonWeather::Fog, 20.0f }, { TEXT("clear_2130"), EDragonWeather::Clear, 21.5f } };
		for (const FEve& E : Eve)
		{
			FShot Ev = St; Ev.Name = FString::Printf(TEXT("24_eve_%s"), E.N); Ev.Weather = (uint8)E.Wx; Ev.Hours = E.H;
			Shots.Add(Ev);
			FShot Ec = Chase; Ec.Name = FString::Printf(TEXT("24b_evechase_%s"), E.N); Ec.Weather = (uint8)E.Wx; Ec.Hours = E.H;
			Shots.Add(Ec);
		}
	}
	// Portraits for the app icon: the dragon coming at the camera, wings out, against the sky.
	{
		struct FIc { const TCHAR* N; FVector Off, Aim; float Fov, H; EDragonWeather Wx; };
		const FIc Ic[] = {
			{ TEXT("a_dusk"),  FVector(2600, 0, -420),  FVector(0, 0, 60),   34.f, 21.4f, EDragonWeather::Clear },
			{ TEXT("b_dusk"),  FVector(1500, 650, -160), FVector(250, 0, 80), 34.f, 21.4f, EDragonWeather::Clear },
			{ TEXT("c_night"), FVector(2600, 0, -420),  FVector(0, 0, 60),   34.f, 23.5f, EDragonWeather::Clear },
			{ TEXT("d_head"),  FVector(900, 380, 60),   FVector(380, 0, 110), 30.f, 21.2f, EDragonWeather::Clear },
			{ TEXT("e_storm"), FVector(2600, 0, -420),  FVector(0, 0, 60),   34.f, 17.0f, EDragonWeather::Storm } };
		for (const FIc& I : Ic)
		{
			FShot Ic1 = Close; Ic1.Name = FString::Printf(TEXT("90_icon_%s"), I.N);
			Ic1.CloseOffset = I.Off; Ic1.CloseAim = I.Aim; Ic1.CloseFov = I.Fov; Ic1.Hours = I.H; Ic1.Weather = (uint8)I.Wx;
			Shots.Add(Ic1);
		}
	}
	// The weather panel open over the chase view.
	{
		FShot Pn = Chase; Pn.Name = TEXT("23_panel"); Pn.bPanel = true;
		Shots.Add(Pn);
		FShot Ph = Chase; Ph.Name = TEXT("23a_hud"); Ph.bPanel = true; Ph.Hours = 17.f;
		Shots.Add(Ph);
		for (int32 Pg = 0; Pg < 3; ++Pg)
		{
			FShot Pm = Chase; Pm.Name = FString::Printf(TEXT("23b_menu%d"), Pg); Pm.bPanel = true; Pm.UiScreen = 1; Pm.UiPage = Pg; Pm.Hours = 17.f;
			Shots.Add(Pm);
		}
		Pn = Chase; Pn.Name = TEXT("23c_chart"); Pn.bPanel = true; Pn.UiScreen = 2; Pn.Hours = 17.f;
		Shots.Add(Pn);
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
		Sky->SetWeather((EDragonWeather)S.Weather, true);
	}
	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (!PC) return;
	if (ADragonPlayerController* DPC = Cast<ADragonPlayerController>(PC))
	{
		DPC->bShowHud = S.bPanel;
		DPC->MenuPage = S.UiPage;
		DPC->SetScreen(S.UiScreen, false);   // no pause: the shot still has to settle
	}
	bFired = false;
	if (S.bDragonClose && Camera)
	{
		if (APawn* P = PC->GetPawn())
		{
			P->CustomTimeDilation = 0.f; // hold the pose for a sharp portrait
			const FVector F = P->GetActorForwardVector(), R = P->GetActorRightVector();
			const FVector At = P->GetActorLocation() + F * S.CloseAim.X + R * S.CloseAim.Y + FVector(0, 0, S.CloseAim.Z);
			const FVector From = P->GetActorLocation() + F * S.CloseOffset.X + R * S.CloseOffset.Y + FVector(0, 0, S.CloseOffset.Z);
			Camera->SetActorLocationAndRotation(From, (At - From).Rotation());
			Camera->GetCameraComponent()->SetFieldOfView(S.CloseFov);
			PC->SetViewTarget(Camera);
		}
	}
	else if (S.bChase)
	{
		if (APawn* P = PC->GetPawn())
		{
			P->CustomTimeDilation = 1.f;
			PosePawn(S);
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

void ADragonPhotoDirector::PosePawn(const FShot& S)
{
	if (ADragonPawn* DP = Cast<ADragonPawn>(UGameplayStatics::GetPlayerPawn(this, 0)))
	{
		if (S.bPose && S.bPoseGround) DP->PhotoGround(S.PoseM, S.PoseYaw);
		else if (S.bPose) DP->PhotoPose(S.PoseM, S.PoseYaw);
		if (S.PoseSpeed >= 0.f) DP->PhotoSpeed(S.PoseSpeed);
		DP->SetScript(S.Script);
	}
}

void ADragonPhotoDirector::HoldPawn(bool bHold)
{
	if (APawn* P = UGameplayStatics::GetPlayerPawn(this, 0)) P->CustomTimeDilation = bHold ? 0.f : 1.f;
}

void ADragonPhotoDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const bool bMech = Shots.IsValidIndex(Index) && (Shots[Index].Script.bOn || Shots[Index].PoseSpeed >= 0.f);
	if (bMech && !bReleased) HoldPawn(true);
	if (Busy())
	{
		if (bMech) HoldPawn(true);   // shaders compiling: everything waits, the dragon too
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
	if (bMech && bReleased) HoldPawn(false);
	Warmup += DeltaSeconds;
	if (Warmup < 5.f) { if (bMech) HoldPawn(true); return; }

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
	if (bMech && !bReleased)
	{
		// Hold him still until the trees and grass are in, then start the clock and let go.
		Wait += DeltaSeconds;
		if (bScatterBusy && Wait < 90.f) return;
		// Start the clock from a fresh pose, whatever happened while we waited.
		bReleased = true; Timer = 0.f; HoldPawn(false);
		PosePawn(Shots[Index]);
	}
	if (!bShotRequested && bScatterBusy && Timer < 60.f && !bMech) { Timer += DeltaSeconds * 0.25f; return; }
	Timer += DeltaSeconds;
	const FShot& S = Shots[Index];
	// Scripted shots run on the dragon's own clock, so the shutter catches the moment asked for.
	if (bMech && S.Script.bOn)
		if (const ADragonPawn* DP = Cast<ADragonPawn>(UGameplayStatics::GetPlayerPawn(this, 0))) Timer = DP->GetScriptTime();
	const float Settle = S.SettleS > 0.f ? S.SettleS : S.bChase ? 4.f : SettleSeconds;
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
	// Plasma shots: launch a bolt so it bursts PlasmaLead seconds before the shutter.
	if (S.bPlasma && !bFired && !bShotRequested)
	{
		const float Flight = 0.4f;                    // seconds of flight (240 m at 600 m/s)
		const float FireAt = Settle - Flight - FMath::Max(0.f, S.PlasmaLead) + FMath::Max(0.f, -S.PlasmaLead);
		if (Timer >= FireAt)
		{
			bFired = true;
			const FVector From = S.Target + FVector(-16000.0, 14000.0, 11000.0).GetSafeNormal() * 24000.0;
			const FVector Dir = (S.Target - From).GetSafeNormal();
			FActorSpawnParameters SP; SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (ADragonPlasma* B = GetWorld()->SpawnActor<ADragonPlasma>(ADragonPlasma::StaticClass(), From, Dir.Rotation(), SP)) B->Launch(Dir * 60000.0);
		}
	}
	// The dragon's own shot: fire when bolt flight + lead = time left to the shutter.
	if (S.bDragonFires && !bFired && !bShotRequested)
	{
		if (ADragonPawn* DP = Cast<ADragonPawn>(UGameplayStatics::GetPlayerPawn(this, 0)))
		{
			const float Flight = (float)(FVector::Dist(DP->GetPosM() * 100.0, S.Target) / 60000.0);
			if (Timer >= Settle - Flight - S.PlasmaLead) { bFired = true; DP->PhotoFire(S.Target / 100.0); }
		}
	}
	if (!bShotRequested && Timer >= Settle)
	{
		const FString File = OutDir / (S.Name + Tag + TEXT(".png"));
		FScreenshotRequest::RequestScreenshot(File, false, false);
		if (const APawn* PP = UGameplayStatics::GetPlayerPawn(this, 0))
		{
			FRotator AttR = FRotator::ZeroRotator;
			for (const UActorComponent* C : PP->GetComponents())
				if (C->GetFName() == TEXT("Attitude")) AttR = Cast<USceneComponent>(C)->GetComponentRotation();
			UE_LOG(LogDragon, Display, TEXT("[Photo] shutter t=%.2f pawn z=%.2fm attitude pitch=%.1f roll=%.1f"), Timer, PP->GetActorLocation().Z / 100.f, AttR.Pitch, AttR.Roll);
		}
		bShotRequested = true;
		UE_LOG(LogDragon, Display, TEXT("[Photo] %s"), *File);
	}
	else if (bShotRequested && Timer >= Settle + 1.5f)
	{
		++Index;
		Timer = 0.f;
		bShotRequested = false;
		bGrassBuilt = false;
		bReleased = false; Wait = 0.f;
		if (Shots.IsValidIndex(Index)) Frame(Shots[Index]);
	}
}

#include "DragonSky.h"
#include "ProceduralMeshComponent.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "DragonWorldSubsystem.h"
#include "DragonSfx.h"
#include "DragonFX.h"
#include "DragonFlight.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"
#include "Materials/MaterialParameterCollection.h"
#include "Kismet/KismetMaterialLibrary.h"

namespace
{
	float Smooth(float X, float A, float B)
	{
		const float T = FMath::Clamp((X - A) / (B - A), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}
}

FDragonWeatherState FDragonWeatherState::Preset(EDragonWeather W)
{
	// Values follow the web game's presets (sky.js), re-expressed for UE's cloud/fog model.
	FDragonWeatherState S;
	switch (W)
	{
	case EDragonWeather::Clear:    S.Cover = 0.12f; S.Density = 0.7f; S.Cumulus = 0.6f; S.SunDim = 0.00f; S.Fog = 0.7f; S.Rain = 0.f; S.Wind = 0.8f; break;
	case EDragonWeather::Fair:     S.Cover = 0.38f; S.Density = 1.0f; S.Cumulus = 0.8f; S.SunDim = 0.05f; S.Fog = 1.0f; S.Rain = 0.f; S.Wind = 1.0f; break;
	case EDragonWeather::Overcast: S.Cover = 0.82f; S.Density = 1.6f; S.Cumulus = 0.3f; S.SunDim = 0.65f; S.Fog = 1.6f; S.Rain = 0.f; S.Wind = 1.3f; break;
	case EDragonWeather::Rain:     S.Cover = 0.90f; S.Density = 2.4f; S.Cumulus = 0.5f; S.SunDim = 0.80f; S.Fog = 2.2f; S.Rain = 0.6f; S.Wind = 1.7f; break;
	case EDragonWeather::Storm:    S.Cover = 0.95f; S.Density = 3.6f; S.Cumulus = 1.0f; S.SunDim = 0.88f; S.Fog = 3.0f; S.Rain = 1.0f; S.Wind = 2.3f; break;
	case EDragonWeather::Fog:      S.Cover = 0.55f; S.Density = 1.2f; S.Cumulus = 0.2f; S.SunDim = 0.45f; S.Fog = 6.0f; S.Rain = 0.f; S.Wind = 0.5f; break;
	}
	return S;
}

FDragonWeatherState FDragonWeatherState::Lerp(const FDragonWeatherState& A, const FDragonWeatherState& B, float T)
{
	FDragonWeatherState S;
	S.Cover = FMath::Lerp(A.Cover, B.Cover, T);
	S.Density = FMath::Lerp(A.Density, B.Density, T);
	S.Cumulus = FMath::Lerp(A.Cumulus, B.Cumulus, T);
	S.SunDim = FMath::Lerp(A.SunDim, B.SunDim, T);
	S.Fog = FMath::Lerp(A.Fog, B.Fog, T);
	S.Rain = FMath::Lerp(A.Rain, B.Rain, T);
	S.Wind = FMath::Lerp(A.Wind, B.Wind, T);
	return S;
}

ADragonSky::ADragonSky()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	Atmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("SkyAtmosphere"));
	Atmosphere->SetupAttachment(Root);
	// Slightly hazier than Earth's default: the haze off a cold sea.
	Atmosphere->MieScatteringScale = 0.006f;
	Atmosphere->AerialPespectiveViewDistanceScale = 1.0f;
	Atmosphere->TransmittanceMinLightElevationAngle = -90.f;

	Clouds = CreateDefaultSubobject<UVolumetricCloudComponent>(TEXT("VolumetricClouds"));
	Clouds->SetupAttachment(Root);
	Clouds->LayerBottomAltitude = 1.2f; // km — the web game's CLOUD_BASE was ~1.2 km
	Clouds->LayerHeight = 6.f;
	Clouds->TracingMaxDistance = 60.f;   // km — the world is 45 km across; beyond this the haze has them
	Clouds->TracingStartMaxDistance = 400.f;
	Clouds->bUsePerSampleAtmosphericLightTransmittance = true;
	// Half the stock ray samples: the cloud layer is far away and soft, and it costs ~7 ms a frame at full.
	Clouds->ViewSampleCountScale = 0.5f;
	Clouds->ShadowViewSampleCountScale = 0.5f;
	Clouds->ShadowTracingDistance = 8.f; // km
	// The sky light's real-time capture re-traces the clouds into a 64x64 cubemap: ~3 ms a face
	// at full samples, for a blurry ambient term nobody can tell from a quarter of the samples.
	Clouds->ReflectionViewSampleCountScaleValue = 0.25f;
	Clouds->ShadowReflectionViewSampleCountScaleValue = 0.25f;
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> CloudMat(TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst"));
	if (CloudMat.Succeeded())
	{
		CloudMaterial = CloudMat.Object;
		Clouds->Material = CloudMat.Object;
	}

	Fog = CreateDefaultSubobject<UExponentialHeightFogComponent>(TEXT("HeightFog"));
	Fog->SetupAttachment(Root);
	Fog->FogDensity = 0.006f;
	Fog->FogHeightFalloff = 0.12f;
	Fog->bEnableVolumetricFog = true;
	Fog->VolumetricFogDistance = 8000.f * DragonUnits::M;
	Fog->VolumetricFogExtinctionScale = 0.6f;
	Fog->FogInscatteringLuminance = FLinearColor(0.f, 0.f, 0.f); // let the atmosphere colour the fog
	Fog->SkyAtmosphereAmbientContributionColorScale = FLinearColor::White;
	// Mist is water droplets: near-white albedo, a touch cool, scattering mostly forward, so it
	// glows warm toward a low sun and stays blue-grey away from it (not lit brown all round).
	Fog->VolumetricFogAlbedo = FColor(226, 232, 240);
	Fog->VolumetricFogScatteringDistribution = 0.55f;

	Sun = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Sun"));
	Sun->SetupAttachment(Root);
	Sun->SetMobility(EComponentMobility::Movable);
	Sun->bAtmosphereSunLight = true;
	Sun->AtmosphereSunLightIndex = 0;
	Sun->Intensity = SunLux;
	Sun->LightSourceAngle = 0.5357f;
	Sun->bCastCloudShadows = true;
	Sun->bCastShadowsOnClouds = true;
	Sun->bCastShadowsOnAtmosphere = true;
	Sun->CloudShadowStrength = 0.85f;
	Sun->CloudShadowExtent = 20.f; // km (around the camera)
	Sun->bUseTemperature = true;
	Sun->Temperature = 5900.f;
	Sun->ForwardShadingPriority = 1;

	Moon = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Moon"));
	Moon->SetupAttachment(Root);
	Moon->SetMobility(EComponentMobility::Movable);
	Moon->bAtmosphereSunLight = true;
	Moon->AtmosphereSunLightIndex = 1;
	Moon->Intensity = 0.f;
	Moon->LightSourceAngle = 0.52f;
	Moon->bUseTemperature = true;
	Moon->Temperature = 8500.f; // cold blue key at night
	Moon->bCastCloudShadows = true;
	Moon->ForwardShadingPriority = 0;

	SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkyLight"));
	SkyLight->SetupAttachment(Root);
	SkyLight->SetMobility(EComponentMobility::Movable);
	SkyLight->bRealTimeCapture = true;
	SkyLight->SourceType = SLS_CapturedScene;
	SkyLight->bLowerHemisphereIsBlack = false;
	SkyLight->LowerHemisphereColor = FLinearColor(0.02f, 0.03f, 0.04f); // the sea under the horizon

	RainCurtain = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("RainCurtain"));
	RainCurtain->SetupAttachment(Root);
	RainCurtain->SetUsingAbsoluteLocation(true);
	RainCurtain->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RainCurtain->SetCastShadow(false);
	RainCurtain->SetVisibility(false);
	RainCurtain->SetRelativeScale3D(FVector(36.f, 36.f, 40.f)); // engine cylinder is 1 m: an 18 m radius, 40 m tall curtain
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cyl(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cyl.Succeeded()) RainCurtain->SetStaticMesh(Cyl.Object);

	Lightning = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Lightning"));
	Lightning->SetupAttachment(Root);
	Lightning->SetMobility(EComponentMobility::Movable);
	Lightning->Intensity = 0.f;
	Lightning->bUseTemperature = true;
	Lightning->Temperature = 9500.f;
	Lightning->SetCastShadows(false);
	Lightning->bAffectsWorld = true;
	Lightning->SetVisibility(false);

	Post = CreateDefaultSubobject<UPostProcessComponent>(TEXT("Post"));
	Post->SetupAttachment(Root);
	Post->bUnbound = true;
	Post->Priority = -1.f; // lets any designer volume override
	FPostProcessSettings& P = Post->Settings;
	P.bOverride_AutoExposureMethod = true;           P.AutoExposureMethod = AEM_Histogram;
	P.bOverride_AutoExposureMinBrightness = true;    P.AutoExposureMinBrightness = -2.5f; // EV100 (extended range)
	P.bOverride_AutoExposureMaxBrightness = true;    P.AutoExposureMaxBrightness = 16.f;
	P.bOverride_AutoExposureBias = true;             P.AutoExposureBias = 0.25f;
	// Meter toward the bright half of the frame, as a photographer exposes for the sunlit
	// ground: a shadowed mountain filling the frame must not blow the meadow out to white.
	P.bOverride_AutoExposureLowPercent = true;       P.AutoExposureLowPercent = 40.f;
	P.bOverride_AutoExposureHighPercent = true;      P.AutoExposureHighPercent = 96.f;
	P.bOverride_AutoExposureSpeedUp = true;          P.AutoExposureSpeedUp = 2.5f;
	P.bOverride_AutoExposureSpeedDown = true;        P.AutoExposureSpeedDown = 1.2f;
	P.bOverride_BloomIntensity = true;               P.BloomIntensity = 0.35f;
	// A camera's look in cold northern light: white balance a touch cool, local exposure
	// holding detail in both the bright sky and the dark basalt.
	P.bOverride_WhiteTemp = true;                    P.WhiteTemp = 6900.f;
	P.bOverride_LocalExposureHighlightContrastScale = true; P.LocalExposureHighlightContrastScale = 0.75f;
	P.bOverride_LocalExposureShadowContrastScale = true;    P.LocalExposureShadowContrastScale = 0.7f;
	P.bOverride_ColorSaturation = true;              P.ColorSaturation = FVector4(1.04, 1.04, 1.04, 1.0);
	P.bOverride_VignetteIntensity = true;            P.VignetteIntensity = 0.25f;
	P.bOverride_FilmGrainIntensity = true;           P.FilmGrainIntensity = 0.05f;
	P.bOverride_MotionBlurAmount = true;             P.MotionBlurAmount = 0.35f;
	P.bOverride_MotionBlurMax = true;                P.MotionBlurMax = 3.f;
	// Lumen quality follows the scalability preset (the Max preset raises it in DefaultScalability.ini).
	P.bOverride_LumenSceneViewDistance = true;       P.LumenSceneViewDistance = 40000.f;
	P.bOverride_LumenMaxTraceDistance = true;        P.LumenMaxTraceDistance = 40000.f;
}

void ADragonSky::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Live = From = To = FDragonWeatherState::Preset(Weather);
	WeatherT = 1.f;
	Apply(0.f);
}

void ADragonSky::BeginPlay()
{
	Super::BeginPlay();
	Live = From = To = FDragonWeatherState::Preset(Weather);
	WeatherT = 1.f;
	WeatherTimer = FMath::FRandRange(240.f, 600.f);
	Apply(0.f);
	// Which knobs does the cloud material have? (Logged so the weather can be wired to them.)
	if (CloudMaterial)
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		CloudMaterial->GetAllScalarParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& I : Infos) UE_LOG(LogDragon, Log, TEXT("Cloud material scalar: %s"), *I.Name.ToString());
	}
}

ADragonSky* ADragonSky::Get(const UObject* WorldContext)
{
	if (!WorldContext) return nullptr;
	UWorld* World = WorldContext->GetWorld();
	if (!World) return nullptr;
	TActorIterator<ADragonSky> It(World);
	return It ? *It : nullptr;
}

void ADragonSky::SetWeather(EDragonWeather NewWeather, bool bInstant, float BlendSeconds)
{
	Weather = NewWeather;
	ActiveBlendSeconds = BlendSeconds > 0.f ? BlendSeconds : WeatherBlendSeconds;
	From = Live;
	To = FDragonWeatherState::Preset(NewWeather);
	WeatherT = bInstant ? 1.f : 0.f;
	if (bInstant) { Live = To; Wetness = To.Rain > 0.05f ? 1.f : 0.f; }
}

void ADragonSky::PickNextWeather()
{
	// Weather walks along clear-fair-overcast-rain-storm one step at a time, so a
	// storm always comes in through overcast and rain; fog is an occasional morning thing.
	static const EDragonWeather Order[] = { EDragonWeather::Clear, EDragonWeather::Fair, EDragonWeather::Overcast, EDragonWeather::Rain, EDragonWeather::Storm };
	int32 Idx = 1;
	for (int32 i = 0; i < 5; ++i) if (Order[i] == Weather) Idx = i;
	if (Weather == EDragonWeather::Fog) { SetWeather(EDragonWeather::Fair); return; }
	if (Hours > 4.5f && Hours < 8.f && FMath::FRand() < 0.15f) { SetWeather(EDragonWeather::Fog); return; }
	// Bias toward fair weather — it is a flying game.
	const float Up = Idx <= 1 ? 0.45f : 0.3f;
	const int32 Next = FMath::Clamp(Idx + (FMath::FRand() < Up ? 1 : -1), 0, 4);
	SetWeather(Order[Next]);
}

FVector ADragonSky::GetWind() const
{
	const FRotator R(0.f, WindYaw, 0.f);
	return R.Vector() * WindSpeed * Live.Wind;
}

FVector ADragonSky::SunDirection(float InHours) const
{
	// Standard solar position. Solar noon at 12:45 puts sunrise ~05:30 and sunset ~20:00 at 64N, dec 9.
	const double Lat = FMath::DegreesToRadians((double)Latitude);
	const double Dec = FMath::DegreesToRadians((double)Declination);
	const double H = FMath::DegreesToRadians((InHours - 12.75) * 15.0);
	const double SinE = FMath::Sin(Lat) * FMath::Sin(Dec) + FMath::Cos(Lat) * FMath::Cos(Dec) * FMath::Cos(H);
	const double E = FMath::Asin(FMath::Clamp(SinE, -1.0, 1.0));
	double CosA = (FMath::Sin(Dec) - SinE * FMath::Sin(Lat)) / FMath::Max(1e-6, FMath::Cos(E) * FMath::Cos(Lat));
	double A = FMath::Acos(FMath::Clamp(CosA, -1.0, 1.0)); // from north, toward east
	if (FMath::Sin(H) > 0.0) A = 2.0 * PI - A;              // afternoon: west of south
	// UE: +X east, -Y north (web -z), +Z up.
	return FVector(FMath::Sin(A) * FMath::Cos(E), -FMath::Cos(A) * FMath::Cos(E), FMath::Sin(E));
}

void ADragonSky::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (DayLengthMinutes > 0.f)
	{
		Hours = FMath::Fmod(Hours + DeltaSeconds * 24.f / (DayLengthMinutes * 60.f), 24.f);
		if (Hours < 0.f) Hours += 24.f;
	}

	if (WeatherT < 1.f)
	{
		WeatherT = FMath::Min(1.f, WeatherT + DeltaSeconds / FMath::Max(1.f, ActiveBlendSeconds));
		Live = FDragonWeatherState::Lerp(From, To, Smooth(WeatherT, 0.f, 1.f));
	}
	if (bAutoWeather && GetWorld() && GetWorld()->IsGameWorld())
	{
		WeatherTimer -= DeltaSeconds;
		if (WeatherTimer <= 0.f)
		{
			PickNextWeather();
			WeatherTimer = FMath::FRandRange(300.f, 900.f);
		}
	}

	Apply(DeltaSeconds);
	TickWeatherFX(DeltaSeconds);
}

void ADragonSky::BuildRainTubes()
{
	// Two open cylinders around the camera, 7 m and 20 m out, 60 m tall, centred on it. UVs
	// wrap around (u) and run up (v) so M_Rain's streaks fall along v. No caps: looking down
	// you see rain against the ground, not a disc.
	RainTubes = NewObject<UProceduralMeshComponent>(this, TEXT("RainTubes"));
	RainTubes->SetupAttachment(Root);
	RainTubes->SetAbsolute(true, true, true);
	RainTubes->RegisterComponent();
	RainTubes->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RainTubes->SetCastShadow(false);
	RainTubes->bAffectDistanceFieldLighting = false;
	TArray<FVector> V; TArray<int32> T; TArray<FVector> N; TArray<FVector2D> UV; TArray<FColor> C; TArray<FProcMeshTangent> Tg;
	// Four layers: 3.5, 8, 18 and 40 m out. Columns are spaced so near drops are close together and
	// far ones sparse on screen; each layer's v-tile is a few metres of fall (vertex G, metres/20),
	// so near drops make short streaks and far ones still read. Vertex R fades the far layers.
	struct FRing { float R, Half, ColM, TileM; uint8 Near; };
	const FRing Rings[] = { { 350.f, 1500.f, 0.22f, 2.5f, 255 }, { 800.f, 2500.f, 0.35f, 4.f, 190 }, { 1800.f, 4000.f, 0.6f, 7.f, 120 }, { 4000.f, 7000.f, 1.1f, 12.f, 70 } };
	for (const FRing& Rg : Rings)
	{
		const int32 Seg = 64;
		const int32 Base = V.Num();
		const float Cols = FMath::RoundToFloat(2.f * PI * Rg.R / 100.f / Rg.ColM);
		const float Tiles = 2.f * Rg.Half / 100.f / Rg.TileM;
		for (int32 i = 0; i <= Seg; ++i)
		{
			const float A = 2.f * PI * i / Seg;
			const FVector D(FMath::Cos(A), FMath::Sin(A), 0.f);
			for (int32 k = 0; k < 2; ++k)
			{
				V.Add(D * Rg.R + FVector(0.f, 0.f, k ? Rg.Half : -Rg.Half));
				N.Add(-D); Tg.Add(FProcMeshTangent(-D.Y, D.X, 0.f));
				UV.Add(FVector2D((float)i / Seg * Cols, k ? Tiles : 0.f));
				C.Add(FColor(Rg.Near, (uint8)FMath::Clamp(FMath::RoundToInt(Rg.TileM / 20.f * 255.f), 1, 255), 0, 255));
			}
		}
		for (int32 i = 0; i < Seg; ++i)
		{
			const int32 A = Base + i * 2, B = A + 1, Cc = A + 2, D2 = A + 3;
			T.Append({ A, Cc, B, B, Cc, D2 });
		}
	}
	RainTubes->CreateMeshSection(0, V, T, N, UV, C, Tg, false);
	RainTubes->SetBoundsScale(10.f);
	if (RainMID) RainTubes->SetMaterial(0, RainMID);
}

void ADragonSky::TickWeatherFX(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld()) return;

	// Rain curtain around the camera.
	if (!RainMID)
	{
		if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/World/Materials/M_Rain.M_Rain")))
		{
			RainMID = UMaterialInstanceDynamic::Create(M, this);
			RainCurtain->SetMaterial(0, RainMID);
		}
	}
	const bool bRain = Live.Rain > 0.02f && RainMID != nullptr;
	RainCurtain->SetVisibility(false);   // the old capped cylinder: seen from inside, its floor filled the view
	if (bRain && !RainTubes) BuildRainTubes();
	if (RainTubes) RainTubes->SetVisibility(bRain);
	if (bRain)
	{
		if (APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0))
		{
			// Follow the camera. The streaks lean along the drops' motion relative to us: they fall at
			// ~9 m/s, drift with the wind, and fly past as fast as we fly.
			const FVector CamPos = Cam->GetCameraLocation();
			FVector CamVel = DeltaSeconds > 0.f && !PrevCamPos.IsZero() ? (CamPos - PrevCamPos) / DeltaSeconds / 100.f : FVector::ZeroVector;
			if (CamVel.Size() > 120.f) CamVel = FVector::ZeroVector;     // a teleport, not flight
			PrevCamPos = CamPos;
			const FVector Wind = FRotator(0.f, WindYaw, 0.f).Vector() * (4.f * Live.Wind);
			FVector Rel = FVector(0.f, 0.f, -9.f) + Wind - CamVel;
			// lean at most ~60 degrees from vertical
			const FVector Hz(Rel.X, Rel.Y, 0.f);
			if (Hz.Size() > 15.6f) Rel = Hz.GetSafeNormal() * 15.6f + FVector(0.f, 0.f, -9.f);
			const FVector Axis = (-Rel).GetSafeNormal();
			RainAxis = FMath::Lerp(RainAxis, Axis, FMath::Clamp(DeltaSeconds * 4.f, 0.f, 1.f)).GetSafeNormal();
			if (RainTubes) RainTubes->SetWorldLocationAndRotation(CamPos, FRotationMatrix::MakeFromZ(RainAxis).Rotator());
		}
		RainMID->SetScalarParameterValue(TEXT("Intensity"), Live.Rain);
		RainMID->SetScalarParameterValue(TEXT("Night"), 1.f - Smooth(SunElevation, -8.f, 4.f));
	}

	// The ground wets through in about a minute of rain and dries over ten.
	Wetness = FMath::Clamp(Wetness + DeltaSeconds * (Live.Rain > 0.05f ? Live.Rain / 60.f : -1.f / 600.f), 0.f, 1.f);
	if (!WeatherMPC) WeatherMPC = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/World/Materials/MPC_Weather.MPC_Weather"));
	if (WeatherMPC) UKismetMaterialLibrary::SetScalarParameterValue(this, WeatherMPC, TEXT("Wetness"), FMath::Min(1.f, Wetness * 1.2f));

	// Lightning in a storm: a double flicker every 6-20 s.
	if (Live.Rain > 0.85f && Weather == EDragonWeather::Storm)
	{
		BoltTimer -= DeltaSeconds;
		if (BoltTimer <= 0.f)
		{
			BoltTimer = FMath::FRandRange(6.f, 20.f);
			BoltT = 0.f;
			// Where it strikes: mostly kilometres off, now and then close. The thunder follows at
			// the speed of sound, a crack when near, a long roll when far.
			const float Bearing = FMath::FRandRange(0.f, 360.f);
			const float Dist = FMath::Lerp(700.f, 14000.f, FMath::Pow(FMath::FRand(), 0.8f)) * 100.f;
			Lightning->SetWorldRotation(FRotator(-FMath::FRandRange(25.f, 70.f), Bearing + 180.f, 0.f));
			BoltFlash = FMath::Clamp(1.4f - Dist / 1200000.f, 0.25f, 1.f);
			const APlayerController* PC = GetWorld()->GetFirstPlayerController();
			const FVector Cam = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
			const FVector Dir = FRotator(0.f, Bearing, 0.f).Vector();
			FVector Ground = Cam + Dir * Dist; Ground.Z = 0.f;
			if (const UDragonWorldSubsystem* WS = GetGameInstance() ? GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr)
				Ground.Z = FMath::Max(0.0, WS->HeightAt(Ground.X, Ground.Y));
			if (Dist < 900000.f)
			{
				if (ADragonFX* FX = ADragonFX::Get(this))
					FX->LightningBolt(Ground + FVector(FMath::FRandRange(-40000.f, 40000.f), FMath::FRandRange(-40000.f, 40000.f), FMath::FRandRange(140000.f, 190000.f)), Ground, FMath::Clamp(Dist * 0.0012f, 60.f, 900.f));
			}
			PendingThunder.Add(FVector2D(Dist / 34300.f, Dist));
		}
	}
	if (BoltT >= 0.f)
	{
		BoltT += DeltaSeconds;
		// Two strokes: 0-0.08 s and 0.16-0.3 s, then out.
		const float F = BoltT < 0.08f ? 1.f : (BoltT > 0.16f && BoltT < 0.30f) ? 0.7f * (1.f - (BoltT - 0.16f) / 0.14f) : 0.f;
		Lightning->SetVisibility(F > 0.f);
		Lightning->SetIntensity(F * 40.f * BoltFlash * FMath::Max(1.f, MoonLux * 20.f));
		if (BoltT > 0.35f) { BoltT = -1.f; Lightning->SetVisibility(false); }
	}
	for (int32 i = PendingThunder.Num() - 1; i >= 0; --i)
	{
		PendingThunder[i].X -= DeltaSeconds;
		if (PendingThunder[i].X <= 0.f)
		{
			DragonSfx::Thunder(this, FVector::ZeroVector, PendingThunder[i].Y);
			PendingThunder.RemoveAtSwap(i);
		}
	}
}

void ADragonSky::Apply(float DeltaSeconds)
{
	const FVector ToSun = SunDirection(Hours);
	SunElevation = FMath::RadiansToDegrees(FMath::Asin(ToSun.Z));
	// Lights move in small steps, not every frame: any change of light direction throws away
	// the whole virtual shadow map cache (~10 ms to rebuild on an M4 Pro). 0.15 deg steps come
	// about once a second at the default day length, and nobody can see them.
	static const float StepCos = FMath::Cos(FMath::DegreesToRadians(0.15f));
	if (FVector::DotProduct(ToSun, AppliedSunDir) < StepCos)
	{
		AppliedSunDir = ToSun;
		Sun->SetWorldRotation((-ToSun).Rotation());

		// The moon rides roughly opposite the sun, a little higher so it clears the hills.
		FVector ToMoon = -ToSun;
		ToMoon.Z = FMath::Abs(ToMoon.Z) * 0.8f + 0.25f;
		ToMoon.Normalize();
		Moon->SetWorldRotation((-ToMoon).Rotation());
	}

	const float Day = Smooth(SunElevation, -3.f, 3.f);
	const float Night = 1.f - Smooth(SunElevation, -10.f, -1.f);
	Sun->SetIntensity(SunLux * Day * (1.f - Live.SunDim * 0.92f));
	Sun->SetVisibility(Day > 0.001f);
	Moon->SetIntensity(MoonLux * Night * (1.f - Live.SunDim * 0.6f));
	Moon->SetVisibility(Night > 0.001f);
	// Golden hour: warmer as the sun nears the horizon (the atmosphere's
	// transmittance does most of this; a little extra keeps the gold the web game had).
	// The atmosphere's own transmittance already reddens a low sun strongly; a 3600 K source on
	// top of that doubled it into a red-orange cast over everything.
	Sun->SetTemperature(FMath::Lerp(4900.f, 5900.f, Smooth(SunElevation, 0.f, 14.f)));

	// "Day for night": a generous blue fill at night so the land reads.
	SkyLight->SetIntensity(FMath::Lerp(1.5f, 3.5f, Night) * (1.f + Live.SunDim * 0.4f));

	// Under heavy cloud the eye does not adapt it all back to a bright day: a storm reads
	// dark, cold and drained of colour, a rainy day grey (exposure bias and grading follow
	// the weather; at night the night look already rules).
	if (Post)
	{
		const float Gloom = Live.SunDim * Day;
		// Dusk: the eye does not adapt a setting sun back up to daylight; the land goes dim and
		// the colour drains while the sky keeps it. The camera's white balance follows the light
		// toward warm a little (so the gold stays gold, not a cast over everything).
		const float Dusk = (1.f - Smooth(SunElevation, 1.f, 16.f)) * Smooth(SunElevation, -8.f, -1.f);
		Post->Settings.bOverride_AutoExposureBias = true;
		Post->Settings.AutoExposureBias = 0.25f - 1.6f * Gloom * Gloom - 0.25f * Live.Rain * Day - 0.7f * Dusk;
		Post->Settings.bOverride_WhiteTemp = true;
		Post->Settings.WhiteTemp = FMath::Lerp(6900.f, 5600.f, Dusk);
		Post->Settings.bOverride_ColorSaturation = true;
		Post->Settings.ColorSaturation = FVector4(1.f, 1.f, 1.f, (1.f - 0.35f * Gloom) * (1.f - 0.18f * Dusk));
		Post->Settings.bOverride_ColorGain = true;
		Post->Settings.ColorGain = FVector4(1.f - 0.06f * Gloom, 1.f - 0.02f * Gloom, 1.f + 0.05f * Gloom, 1.f);
	}

	// Rain hangs in the air as a grey veil: the far islands fade to shapes, the near ones lose
	// their contrast. It reaches up through the air, not just along the sea.
	Fog->SetFogDensity(0.0045f * Live.Fog + 0.022f * Live.Rain * Live.Rain);
	Fog->SetFogHeightFalloff(FMath::Lerp(0.12f, 0.03f, Live.Rain));
	Fog->SetVolumetricFogExtinctionScale(0.6f * FMath::Sqrt(Live.Fog));
	// The fog takes its colour from the sky; with the sun low that sky ambient is beige, and a
	// thick evening mist read as brown smog. Real mist at dusk goes blue-grey (lit by the blue
	// sky, the sun's gold only toward the sun), so cool the ambient part as the sun goes down.
	{
		const float DuskF = (1.f - Smooth(SunElevation, 2.f, 18.f)) * Smooth(SunElevation, -10.f, -2.f);
		const float Mist = DuskF * FMath::Clamp(Live.Fog / 3.f + Live.Rain * 0.5f, 0.3f, 1.f);
		Fog->SetSkyAtmosphereAmbientContributionColorScale(FMath::Lerp(FLinearColor::White, FLinearColor(0.62f, 0.74f, 1.0f), Mist));
		Fog->SetVolumetricFogAlbedo(FLinearColor::LerpUsingHSV(FLinearColor(0.886f, 0.91f, 0.94f), FLinearColor(0.78f, 0.86f, 0.98f), Mist).ToFColor(false));
	}

	CloudOffset += DeltaSeconds * WindSpeed * Live.Wind * 0.01f; // metres of drift
	if (!CloudMID && CloudMaterial)
	{
		CloudMID = UMaterialInstanceDynamic::Create(CloudMaterial, this);
		Clouds->SetMaterial(CloudMID);
	}
	if (CloudMID)
	{
		CloudMID->SetScalarParameterValue(TEXT("Coverage"), Live.Cover);
		CloudMID->SetScalarParameterValue(TEXT("Density"), Live.Density);
		CloudMID->SetScalarParameterValue(TEXT("Cumulus"), Live.Cumulus);
		CloudMID->SetScalarParameterValue(TEXT("WindOffset"), CloudOffset);
		CloudMID->SetScalarParameterValue(TEXT("WindYaw"), WindYaw);
	}
}

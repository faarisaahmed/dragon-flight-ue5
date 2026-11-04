#include "DragonSky.h"
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
	Sun->CloudShadowExtent = 35.f; // km (around the camera)
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

void ADragonSky::SetWeather(EDragonWeather NewWeather, bool bInstant)
{
	Weather = NewWeather;
	From = Live;
	To = FDragonWeatherState::Preset(NewWeather);
	WeatherT = bInstant ? 1.f : 0.f;
	if (bInstant) Live = To;
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
		WeatherT = FMath::Min(1.f, WeatherT + DeltaSeconds / FMath::Max(1.f, WeatherBlendSeconds));
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
	RainCurtain->SetVisibility(bRain);
	if (bRain)
	{
		if (APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0))
		{
			RainCurtain->SetWorldLocation(Cam->GetCameraLocation() - FVector(0, 0, 2000.f));
		}
		RainMID->SetScalarParameterValue(TEXT("Intensity"), Live.Rain);
		RainMID->SetScalarParameterValue(TEXT("Night"), 1.f - Smooth(SunElevation, -8.f, 4.f));
	}

	// Lightning in a storm: a double flicker every 6-20 s.
	if (Live.Rain > 0.85f && Weather == EDragonWeather::Storm)
	{
		BoltTimer -= DeltaSeconds;
		if (BoltTimer <= 0.f)
		{
			BoltTimer = FMath::FRandRange(6.f, 20.f);
			BoltT = 0.f;
			Lightning->SetWorldRotation(FRotator(-FMath::FRandRange(25.f, 70.f), FMath::FRandRange(0.f, 360.f), 0.f));
		}
	}
	if (BoltT >= 0.f)
	{
		BoltT += DeltaSeconds;
		// Two strokes: 0-0.08 s and 0.16-0.3 s, then out.
		const float F = BoltT < 0.08f ? 1.f : (BoltT > 0.16f && BoltT < 0.30f) ? 0.7f * (1.f - (BoltT - 0.16f) / 0.14f) : 0.f;
		Lightning->SetVisibility(F > 0.f);
		Lightning->SetIntensity(F * 40.f * FMath::Max(1.f, MoonLux * 20.f));
		if (BoltT > 0.35f) { BoltT = -1.f; Lightning->SetVisibility(false); }
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
	Sun->SetTemperature(FMath::Lerp(3600.f, 5900.f, Smooth(SunElevation, 0.f, 14.f)));

	// "Day for night": a generous blue fill at night so the land reads.
	SkyLight->SetIntensity(FMath::Lerp(1.5f, 3.5f, Night) * (1.f + Live.SunDim * 0.4f));

	Fog->SetFogDensity(0.0045f * Live.Fog);
	Fog->SetVolumetricFogExtinctionScale(0.6f * FMath::Sqrt(Live.Fog));

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

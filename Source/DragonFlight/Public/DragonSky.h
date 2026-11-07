#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DragonSky.generated.h"

class USkyAtmosphereComponent;
class UVolumetricCloudComponent;
class UExponentialHeightFogComponent;
class UDirectionalLightComponent;
class USkyLightComponent;
class UPostProcessComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;

UENUM(BlueprintType)
enum class EDragonWeather : uint8
{
	Clear,
	Fair,
	Overcast,
	Rain,
	Storm,
	Fog
};

/** What one weather preset does to the sky. Live state is always a blend of two of these. */
USTRUCT(BlueprintType)
struct FDragonWeatherState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite) float Cover = 0.35f;     // cloud coverage 0..1
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float Density = 1.0f;    // cloud extinction multiplier
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float Cumulus = 0.7f;    // 0 sheet .. 1 towers
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float SunDim = 0.0f;     // how much direct sun is lost 0..1
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float Fog = 1.0f;        // haze multiplier
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float Rain = 0.0f;       // 0..1
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float Wind = 1.0f;       // multiplier on base wind

	static FDragonWeatherState Preset(EDragonWeather W);
	static FDragonWeatherState Lerp(const FDragonWeatherState& A, const FDragonWeatherState& B, float T);
};

/**
 * The sky: one clock and one weather state, and everything that lights the
 * world reads them — sun, moon, sky atmosphere, volumetric clouds, height fog,
 * the real-time sky light and the exposure. Ported from the web game's sky.js:
 * a northern summer at Iceland's latitude, sunrise about 05:30, sunset about
 * 20:00, the sun low and white even at noon, gold only near the horizon, and a
 * short deep-blue night that is lit well enough to fly in.
 *
 * Everything is physically based: the sun is ~100 klux, the moon a fraction of
 * a lux, and auto exposure does what the web game's hand-keyed exposure did.
 */
UCLASS()
class DRAGONFLIGHT_API ADragonSky : public AActor
{
	GENERATED_BODY()

public:
	ADragonSky();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return bPreviewInEditor; }

	// --- Clock ---------------------------------------------------------------

	/** Hour of day, 0..24. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Clock", meta = (ClampMin = 0, ClampMax = 24))
	float Hours = 15.5f;

	/** Real minutes for one full game day. 0 freezes the clock. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Clock")
	float DayLengthMinutes = 48.f;

	/** Latitude in degrees (Iceland ~64). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Clock")
	float Latitude = 64.f;

	/** Solar declination in degrees: +18 is early summer — long days, short blue nights. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Clock")
	float Declination = 18.f;

	/** Tick in the editor viewport too, so the clock and weather can be previewed. */
	UPROPERTY(EditAnywhere, Category = "Sky|Clock")
	bool bPreviewInEditor = false;

	// --- Weather -------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Weather")
	EDragonWeather Weather = EDragonWeather::Fair;

	/** Let the weather wander on its own (clear -> fair -> overcast -> rain -> storm and back). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Weather")
	bool bAutoWeather = true;

	/** Seconds a change of weather takes to roll in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Weather")
	float WeatherBlendSeconds = 60.f;

	/** Prevailing wind direction (UE yaw, degrees) and base speed (cm/s). Read by clouds, water, the dragon. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Weather")
	float WindYaw = 123.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Weather")
	float WindSpeed = 600.f;

	UFUNCTION(BlueprintCallable, Category = "Sky")
	void SetWeather(EDragonWeather NewWeather, bool bInstant = false, float BlendSeconds = -1.f);

	UFUNCTION(BlueprintPure, Category = "Sky")
	const FDragonWeatherState& GetWeatherState() const { return Live; }

	/** Wind velocity in cm/s, weather included. */
	UFUNCTION(BlueprintPure, Category = "Sky")
	FVector GetWind() const;

	/** Sun elevation in degrees (negative below the horizon). */
	UFUNCTION(BlueprintPure, Category = "Sky")
	float GetSunElevation() const { return SunElevation; }

	UFUNCTION(BlueprintPure, Category = "Sky")
	bool IsNight() const { return SunElevation < -4.f; }

	static ADragonSky* Get(const UObject* WorldContext);

	// --- Components ----------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky") TObjectPtr<USceneComponent> Root;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky") TObjectPtr<USkyAtmosphereComponent> Atmosphere;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky") TObjectPtr<UVolumetricCloudComponent> Clouds;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky") TObjectPtr<UExponentialHeightFogComponent> Fog;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky") TObjectPtr<UDirectionalLightComponent> Sun;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky") TObjectPtr<UDirectionalLightComponent> Moon;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky") TObjectPtr<USkyLightComponent> SkyLight;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky") TObjectPtr<UPostProcessComponent> Post;
	/** A curtain of rain streaks that follows the camera (M_Rain, made by the builder). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky") TObjectPtr<class UStaticMeshComponent> RainCurtain;
	/** Storm lightning: a brief cold flash from a random bearing. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky") TObjectPtr<UDirectionalLightComponent> Lightning;

	/** Volumetric cloud material. If it exposes Coverage / Density / Cumulus / WindOffset scalars they are driven by the weather. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Clouds")
	TObjectPtr<UMaterialInterface> CloudMaterial;

	/** Sun illuminance at the zenith on a clear day, lux. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Light")
	float SunLux = 100000.f;

	/** Full-moon illuminance, lux. Real is ~0.25; a little more keeps night playable ("day for night"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky|Light")
	float MoonLux = 1.5f;

private:
	void Apply(float DeltaSeconds);
	FVector SunDirection(float InHours) const; // unit vector TOWARD the sun, UE space
	void PickNextWeather();

	FDragonWeatherState From, To, Live;
	float WeatherT = 1.f;
	float ActiveBlendSeconds = 60.f;
	float WeatherTimer = 0.f;
	float SunElevation = 30.f;
	float CloudOffset = 0.f;

	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> CloudMID;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> RainMID;
	/** Rain streaks: two open tubes around the camera (no caps to see from inside), near and far. */
	UPROPERTY(Transient) TObjectPtr<class UProceduralMeshComponent> RainTubes;
	void BuildRainTubes();
	FVector PrevCamPos = FVector::ZeroVector;
	FVector RainAxis = FVector::UpVector;      // smoothed: the way the streaks lean (wind and our own speed)
	float Wetness = 0.f;                        // how rained-on the ground is (MPC_Weather)
	UPROPERTY(Transient) TObjectPtr<class UMaterialParameterCollection> WeatherMPC;
	float BoltTimer = 8.f;
	float BoltFlash = 1.f;
	/** Thunder on its way: seconds to go, distance (cm). */
	TArray<FVector2D> PendingThunder;
	float BoltT = -1.f;
	void TickWeatherFX(float DeltaSeconds);

private:
	FVector AppliedSunDir = FVector::ZeroVector;
};

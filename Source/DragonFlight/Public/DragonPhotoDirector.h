#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DragonPawn.h"
#include "DragonPhotoDirector.generated.h"

class ACameraActor;

/**
 * Unattended photo mode, for judging the look without anyone at the screen.
 * Started by the game mode when the command line has -AutoShot. It waits for
 * shader compilation and streaming to settle, then visits a list of views
 * (the chase camera, then island vistas at a few times of day), saves a
 * screenshot at each into Saved/Screenshots/Auto/, and quits.
 *
 *   UnrealEditor DragonFlight.uproject -game -AutoShot [-AutoShotRes=1920x1080] [-RenderOffscreen]
 */
UCLASS()
class DRAGONFLIGHT_API ADragonPhotoDirector : public AActor
{
	GENERATED_BODY()

public:
	ADragonPhotoDirector();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Seconds to let a view settle (streaming, Lumen, exposure) before shooting it. */
	UPROPERTY(EditAnywhere, Category = "Photo") float SettleSeconds = 10.f;

private:
	struct FShot
	{
		FString Name;
		bool bChase = false;    // use the player's own camera
		bool bDragonClose = false; // a close portrait of the dragon
		FVector CloseOffset = FVector(1100.0, 850.0, 260.0);   // portrait camera: forward, right, up of the dragon (cm)
		FVector CloseAim = FVector(0.0, 0.0, 100.0);           // and the point it looks at
		float CloseFov = 40.f;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float Hours = 15.5f;
		uint8 Weather = 1;           // EDragonWeather (Fair)
		bool bPlasma = false;        // fire a bolt at Target, timed to burst just before the shot
		float PlasmaLead = 0.35f;    // seconds between the burst and the shot (negative: mid-flight)
		FVector Target = FVector::ZeroVector;
		bool bPanel = false;         // show the HUD
		int32 UiScreen = 0;          // with a screen over it: 1 menu, 2 chart
		int32 UiPage = 0;            // menu page
		bool bPose = false;          // chase shot: put the dragon at PoseM (m) facing PoseYaw first
		FVector PoseM = FVector::ZeroVector;
		float PoseYaw = 0.f;
		bool bDragonFires = false;   // chase shot: the dragon fires at Target (cm), bursting PlasmaLead s before the shutter
		bool bPoseGround = false;    // chase shot: standing on the ground at PoseM instead of flying
		float PoseSpeed = -1.f;      // m/s of flight at the pose (-1: leave it)
		ADragonPawn::FDragonScript Script;   // scripted keys for the shot (tests of the mechanics)
		float SettleS = 0.f;         // seconds from pose to shutter (0: the default)
	};

	void PlanShots();
	bool Busy() const;
	void Frame(const FShot& S);

	TArray<FShot> Shots;
	int32 Index = -1;
	float Timer = 0.f;
	float Warmup = 0.f;
	bool bShotRequested = false;
	bool bGrassBuilt = false;
	bool bFired = false;
	bool bReleased = false;
	float Wait = 0.f;       // mechanics shots: the dragon is held still until the world is ready
	void HoldPawn(bool bHold);
	void PosePawn(const FShot& S);
	FString OutDir;
	FString Tag;

	UPROPERTY(Transient) TObjectPtr<ACameraActor> Camera;
};

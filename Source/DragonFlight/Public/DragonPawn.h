#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "DragonFlightModel.h"
#include "DragonPawn.generated.h"

class UCameraComponent;
class UPoseableMeshComponent;
class UDragonStandInComponent;
class UDragonRigComponent;
class ADragonPlasma;
class UAudioComponent;

/**
 * The player dragon.
 *
 *  - Flight: FDragonFlightModel (controls.js, line for line).
 *  - The terrain "flight floor": a quarter-second look-ahead that lifts him over
 *    ridges, and real impacts only into cliff faces (main.js).
 *  - Landing, flare and camera-relative walking with the reverse cone.
 *  - The camera rig: the player owns yaw/pitch, position is smoothed relative to
 *    the focus, auto-trail after 0.7 s idle, occlusion, FOV with speed, lean.
 *  - Plasma fire on key-down with the 6-shot magazine.
 *
 * Everything is code — there are no input or animation assets to set up.
 * Keys are polled from the PlayerController with the web game's default
 * "wasd" scheme (Docs/Controls.md).
 */
UCLASS()
class DRAGONFLIGHT_API ADragonPawn : public APawn
{
	GENERATED_BODY()

public:
	ADragonPawn();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void CalcCamera(float DeltaTime, struct FMinimalViewInfo& OutResult) override;

	// --- Components ------------------------------------------------------------
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dragon") TObjectPtr<USceneComponent> Feet;
	/** Pitch and roll pivot, 1.45 m above the feet (BODY_CENTRE). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dragon") TObjectPtr<USceneComponent> Attitude;
	/** Hangs the mesh back down to the feet; carries the wingbeat heave. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dragon") TObjectPtr<USceneComponent> MeshPivot;
	/** The real dragon, once one is imported (see Docs/Dragon.md). Hidden while empty. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dragon") TObjectPtr<UPoseableMeshComponent> DragonMesh;
	/** Procedural stand-in made of primitives, used until a real dragon mesh exists. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dragon") TObjectPtr<UDragonStandInComponent> StandIn;
	/** Drives the real mesh's bones procedurally from the flight state (wings, tail, neck, legs). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dragon") TObjectPtr<UDragonRigComponent> Rig;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dragon") TObjectPtr<UCameraComponent> Camera;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dragon") TObjectPtr<UAudioComponent> WindAudio;

	// --- Tuning ----------------------------------------------------------------
	/** Mouse look sensitivity, radians per mouse count (web: 0.003 rad/px). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dragon|Controls") float MouseSensitivity = 0.0035f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dragon|Controls") bool bInvertX = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dragon|Controls") bool bInvertY = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dragon|Controls") float PadLookSpeed = 1.f;
	/** R2 = up (the web game's default). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dragon|Controls") bool bPadClimbInvert = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dragon|Gameplay") bool bDamage = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dragon|Gameplay") bool bCollide = true;
	/** Uniform scale on the dragon. 1 = life size (8.6 m nose to tail, 15.1 m span). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dragon") float DragonScale = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dragon|Fire") TSubclassOf<ADragonPlasma> PlasmaClass;

	// --- State for HUD / rig ---------------------------------------------------
	UFUNCTION(BlueprintPure, Category = "Dragon") bool IsGrounded() const { return bGrounded; }
	UFUNCTION(BlueprintPure, Category = "Dragon") float GetSpeedMS() const { return bGrounded ? WalkSpeed : Flight.ActiveSpeed; }
	UFUNCTION(BlueprintPure, Category = "Dragon") float GetAltitudeM() const { return PosM.Z; }
	UFUNCTION(BlueprintPure, Category = "Dragon") float GetAGL() const { return Agl; }
	UFUNCTION(BlueprintPure, Category = "Dragon") float GetHealth() const { return Health; }
	UFUNCTION(BlueprintPure, Category = "Dragon") int32 GetShots() const { return Shots; }
	UFUNCTION(BlueprintPure, Category = "Dragon") bool IsAiming() const { return AimBlend > 0.5f; }
	UFUNCTION(BlueprintPure, Category = "Dragon") FString GetPrompt() const { return Prompt; }
	UFUNCTION(BlueprintPure, Category = "Dragon") float GetCompassDegrees() const;

	const FDragonFlightModel& GetFlight() const { return Flight; }
	float GetWalkSpeed() const { return WalkSpeed; }
	float GetWalkYaw() const { return WalkYaw; }
	bool IsFlaring() const { return bFlaring; }
	float GetKnifeCharge01() const { return Flight.KnifeCharge / FDragonFlightModel::KNIFE_HOLD; }
	FString GetModeName() const;

	/** Teleport (metres, UE axes) — debug and respawn. */
	void TeleportToM(const FVector& NewPosM, bool bAirborne = true);

	/** Ground height (m) under a UE-axis point in metres, excluding water and the dragon. */
	float SolidAtM(float X, float Y) const;

private:
	// --- Input ---------------------------------------------------------------
	struct FFrameInput
	{
		float Move = 0, Turn = 0, Vertical = 0, Strafe = 0;
		float WalkFwd = 0, WalkSide = 0;
		bool bSprint = false, bBurst = false, bLand = false, bFire = false, bFirePressed = false, bAim = false;
		bool bAlignDragon = false, bRecenter = false, bUpPressed = false, bPadUpTakeoff = false, bPadRun = false;
		int32 Knife = 0;
		float LookYaw = 0, LookPitch = 0; // radians this frame
		bool bLooked = false;
		float BoomDelta = 0;
	};
	FFrameInput GatherInput(float Dt);
	bool AnyDown(std::initializer_list<FKey> Keys) const;
	bool AnyPressed(std::initializer_list<FKey> Keys) const;
	static FVector2D ShapeStick(FVector2D V);
	static float ShapeTrigger(float V);

	// --- Movement ------------------------------------------------------------
	void TickFlight(float Dt, const FFrameInput& In);
	void TickGround(float Dt, const FFrameInput& In);
	void FlightFloor(float Dt);
	void CheckLanding(float Dt, const FFrameInput& In);
	void Land();
	void TakeOff();
	void GroundAttitude(float X, float Y, float H, float& OutPitch, float& OutRoll, float& OutSteep) const;
	float Impact(const FVector& V, const FVector& N, bool bWater);
	void TickHealth(float Dt);
	void Respawn();

	// --- Camera --------------------------------------------------------------
	void TickCamera(float Dt, const FFrameInput& In);
	float ViewAspect() const;

	// --- Fire ----------------------------------------------------------------
	void TickFire(float Dt, const FFrameInput& In);
	FVector AimPointM() const;

	// --- Visuals -------------------------------------------------------------
	void ApplyTransform();

	FDragonFlightModel Flight;
	FVector PosM = FVector::ZeroVector; // feet, metres, UE axes
	FVector PrevPosM = FVector::ZeroVector;
	float Agl = 0.f;
	FString Prompt;

	// Ground
	bool bGrounded = false;
	bool bFlaring = false;
	float WalkYaw = 0.f;
	float WalkSpeed = 0.f;
	FVector GroundVel = FVector::ZeroVector;
	float GroundPitch = 0.f, GroundRoll = 0.f, GroundPitchVel = 0.f, GroundRollVel = 0.f;
	float Unsupported = 0.f;
	float LandHold = 0.f;
	bool bFloorContact = false;

	// Health
	float Health = 100.f;
	float HitGrace = 0.f;
	float SinceHit = 99.f;
	float ScrapeTimer = 0.f;

	// Camera (web conventions; metres)
	bool bCamInit = false;
	float CamYaw = 0.f;
	float CamPitch = 0.3f;
	float DistBase = 11.f;
	float CamPath = 0.f;
	FVector CamLead = FVector::ZeroVector;
	FVector CamVel = FVector::ZeroVector;
	FVector FocusLag = FVector::ZeroVector;
	FVector CamRel = FVector::ZeroVector;
	FVector CamPosM = FVector::ZeroVector;
	FVector FocusM = FVector::ZeroVector;
	float BoomScale = 1.f;
	float LookIdle = 99.f;
	bool bRecentering = false;
	float FovV = 70.f;
	float CamLean = 0.f;
	float PendingYaw = 0.f, PendingPitch = 0.f;
	float AimBlend = 0.f;
	FVector LookAtM = FVector::ZeroVector;

	// Fire
	int32 Shots = 6;
	float FireCooldown = 0.f;
	float RechargeT = 0.f;

	// Wing heave from the rig (m), kept off the collision/camera
	float Heave = 0.f;
};

#pragma once

#include "CoreMinimal.h"

/**
 * The player dragon's flight model, ported line for line from the web game's
 * controls.js (see Docs/FlightModel_FromWeb.md for every constant's origin).
 *
 * It is an arcade "flying mount" model, not aerodynamics: forward speed along
 * a heading chased up and down a speed ladder, an independent vertical speed,
 * and an aircraft-like single-flight-path model only for the committed zoom /
 * stall / dive / recover manoeuvres.
 *
 * Units are the web game's: METRES, seconds, radians. Heading h uses the web
 * convention — h = 0 faces web +z, increasing h turns LEFT — and the model
 * works on a UE-axis position in metres where
 *     forward(h) = (sin h, cos h, 0),  left(h) = (cos h, -sin h, 0),  up = +Z.
 * (UE X = web x, UE Y = web z.) ADragonPawn converts to centimetres.
 */

/** Double-tap-and-hold detector (makeTapGate in controls.js). */
struct FDragonTapGate
{
	float TapMax = 0.30f;
	float Gap = 0.34f;

	float Held = 0.f;
	int32 Dir = 0;
	int32 TapDir = 0;
	float Age = 99.f;
	bool bArmed = false;
	bool bFired = false;

	FDragonTapGate() = default;
	FDragonTapGate(float InTapMax, float InGap) : TapMax(InTapMax), Gap(InGap) {}

	void Update(float Dt, int32 Want);
	void Reset() { Held = 0.f; Dir = 0; TapDir = 0; Age = 99.f; bArmed = false; bFired = false; }
};

enum class EDragonFlightMode : uint8
{
	Level,
	Drop,
	Zoom,
	Stall,
	Dive,
	Recover
};

/** Already-combined input for one frame (keyboard + pad). */
struct FDragonFlightInput
{
	float Move = 0.f;        // -1..1, + forward (pad already squared)
	bool bSprint = false;
	bool bBurst = false;
	float Turn = 0.f;        // -1..1, + LEFT
	float Vertical = 0.f;    // -1..1, + up
	float Strafe = 0.f;      // -1..1, + right
	int32 KnifeWant = 0;     // +1 left wing down, -1 right
	bool bAlignRequest = false; // turn him to face where the camera looks
	float CamYaw = 0.f;      // camera bearing focus->camera, web heading convention
};

/** What the wing/body rig and camera read every frame (getFlightState). */
struct FDragonFlightState
{
	float SpeedT = 0.f;  // 0..1 of flat out (335 m/s)
	float Climb = 0.f;   // -1..1
	float Knife = 0.f;
	float Turn = 0.f;    // -1..1
	float Load = 1.f;    // g
	float Hang = 0.f;    // 0..1 stall hang
	float Roll = 0.f;    // barrel roll rate -1..1
};

class DRAGONFLIGHT_API FDragonFlightModel
{
public:
	// --- Constants (controls.js; names kept) ----------------------------------
	static constexpr float MPH = 0.44704f;
	static constexpr float SPEED_MIN = 0.f;
	static constexpr float SPEED_CRUISE = 55.f;
	static constexpr float PEDAL_MAX = 180.f;
	static constexpr float BURST_SPEED = 750.f * MPH;   // 335.28
	static constexpr float TURBO_SPEED = 1600.f * MPH;  // 715.26
	static constexpr float REVERSE_SPEED = 9.f;
	static constexpr float PEDAL_GAIN = 1.5f, PEDAL_BLEED = 0.9f, BURST_GAIN = 5.5f, BURST_BLEED = 1.7f, TURBO_GAIN = 0.75f;

	static constexpr float VERT_HOVER = 20.f, VERT_PER_SPEED = 0.62f, CLIMB_RATE_CAP = 140.f;
	static constexpr float VERT_LAMBDA = 7.5f, VERT_RELEASE = 5.5f, TRIM_RATE = 52.f;
	static constexpr float CLIMB_ENTRY_SPEED = 105.f, DIVE_ENTRY_SPEED = 105.f;
	static constexpr float MANOEUVRE_HOLD = 0.10f, AXIS_THRESHOLD = 0.55f;

	static constexpr float CLIMB_DRAG = 34.f, CLIMB_DRAG_V2 = 6.0e-4f, STALL_SPEED = 15.f, STALL_HANG = 0.5f;
	static constexpr float NOSE_OVER_RATE = 2.2f, DIVE_GRAVITY = 36.f, DIVE_MAX = 1.2f * BURST_SPEED;
	static constexpr float STALL_DIVE_COMMIT = 1.1f, RECOVER_RATE = 2.6f, RECOVER_HOLD = 1.1f;
	static constexpr float DROP_SINK = 75.f, DROP_LAMBDA = 3.0f, DROP_SPEED = 0.86f * 105.f, DROP_DRAG = 1.4f;

	static constexpr float YAW_RATE_MAX_DEFAULT = 1.45f, TURN_G_DEFAULT = 196.f;
	static constexpr float YAW_LAMBDA = 3.6f, YAW_DAMP = 6.0f, YAW_DEADZONE = 0.02f, ALIGN_GAIN = 1.6f, ALIGN_TOLERANCE = 0.02f;

	static constexpr float MAX_BANK = 1.00f, BANK_LAMBDA = 4.6f;
	static constexpr float STRAFE_MAX = 26.f, STRAFE_ACCEL = 90.f, STRAFE_DAMP = 4.2f;
	static constexpr float PITCH_MAX = 0.72f, PITCH_LAMBDA = 6.0f, AOA_SLOW = 0.22f, PATH_REF_SPEED = 34.f;

	static constexpr float KNIFE_ANGLE = 1.45f, KNIFE_IN = 3.2f, KNIFE_OUT = 5.2f, KNIFE_HOLD = 3.0f, KNIFE_RECOVER = 2.0f;
	static constexpr float KNIFE_SINK = 3.5f, KNIFE_YAW = 0.16f, KNIFE_TREMBLE = 0.022f;

	static constexpr float ROLL_EASE = 0.35f, ROLL_UNDER = 0.45f, ROLL_PITCH = 0.13f, ROLL_COST = 0.85f, ROLL_DRAG = 0.07f;

	static constexpr float EDGE_SOFT = 24300.f, EDGE_HARD = 28800.f, EDGE_TURN = 0.62f;

	static constexpr float SNARE_SINK = 26.f, SNARE_SPEED = 32.f, SNARE_DRAG = 2.2f, SNARE_STEER = 0.22f, SNARE_SHAKE = 0.62f;

	// --- State ---------------------------------------------------------------
	bool bHeadingSeeded = false;
	float Heading = 0.f;
	float YawRate = 0.f;
	float YawMaxNow = YAW_RATE_MAX_DEFAULT;
	float YawRateMax = YAW_RATE_MAX_DEFAULT;
	float TurnG = TURN_G_DEFAULT;
	float Airspeed = SPEED_CRUISE;
	float ActiveSpeed = SPEED_CRUISE;
	float ClimbVel = 0.f;
	float StrafeVel = 0.f;
	float PathAngle = 0.f;
	float CurrentPitch = 0.f;
	float CurrentRoll = 0.f;
	EDragonFlightMode Mode = EDragonFlightMode::Level;
	float ModeT = 0.f;
	float RecoverHold = 0.f;
	float DiveCommit = 0.f;
	float KnifeAmount = 0.f;
	float KnifeCharge = KNIFE_HOLD;
	bool bBursting = false;
	bool bTurbo = false;
	float Snared = 0.f;
	int32 SnareDir = 0;
	TOptional<float> AlignTarget;
	float CurrentForwardSpeed = SPEED_CRUISE;
	float VertAccel = 0.f;
	float EdgeT = 0.f;
	float Time = 0.f;
	bool bStalledThisFrame = false;

	// Barrel roll
	int32 RollDir = 0;
	float RollT = 0.f, RollTime = 0.58f, RollBulge = 2.5f, RollRise = 4.f, RollShift = 20.f;
	float RollEntry = 0.f, RollLat = 0.f, RollUp = 0.f, RollRate = 0.f;

	FDragonTapGate VertGate{ 0.30f, 0.34f };
	FDragonTapGate RollGate{ 0.26f, 0.32f };
	FDragonTapGate TurboGate{ 0.5f, 0.6f };
	int32 LastSnareCross = 0;

	// --- API -----------------------------------------------------------------

	/** Advance one frame. PosM is the actor origin (his feet) in metres, UE axes. */
	void Update(float Dt, const FDragonFlightInput& In, FVector& PosM);

	static FVector Forward(float H) { return FVector(FMath::Sin(H), FMath::Cos(H), 0.0); }
	static FVector Left(float H) { return FVector(FMath::Cos(H), -FMath::Sin(H), 0.0); }
	/** UE actor yaw (degrees) for a web heading. */
	static float HeadingToYaw(float H) { return 90.f - FMath::RadiansToDegrees(H); }
	static float YawToHeading(float YawDeg) { return FMath::DegreesToRadians(90.f - YawDeg); }
	static float Damp(float Lambda, float Dt) { return 1.f - FMath::Exp(-Lambda * Dt); }
	static float AngleDelta(float From, float To);

	float GetSpeedRatio() const { return FMath::Clamp(ActiveSpeed / BURST_SPEED, 0.f, 1.f); }
	float GetTurnT() const { return FMath::Clamp(YawRate / FMath::Max(YawMaxNow, 1e-4f), -1.f, 1.f); }
	float GetTurboT() const { return FMath::Clamp((ActiveSpeed - BURST_SPEED) / (TURBO_SPEED - BURST_SPEED), 0.f, 1.f); }
	float GetStallT() const { return 1.f - FMath::Clamp((Airspeed - STALL_SPEED) / (CLIMB_ENTRY_SPEED - STALL_SPEED), 0.f, 1.f); }
	bool IsManoeuvring() const { return Mode != EDragonFlightMode::Level && Mode != EDragonFlightMode::Drop; }
	bool IsTrimming() const { return Mode == EDragonFlightMode::Level && VertGate.Dir != 0 && !VertGate.bArmed; }
	float GetCommitT() const { return VertGate.bArmed ? FMath::Min(1.f, VertGate.Held / MANOEUVRE_HOLD) : 0.f; }
	FDragonFlightState GetFlightState() const;
	/** Velocity in m/s (UE axes), path speed along heading + climb + strafe. */
	FVector GetVelocity() const;

	void SetHeading(float H) { Heading = H; bHeadingSeeded = true; YawRate = 0.f; }
	void SetMode(EDragonFlightMode M) { if (M != Mode) { Mode = M; ModeT = 0.f; } }

	// Collision speed loss (controls.js scrapeSpeed / bleedSpeed)
	void ScrapeSpeed(float F);
	void BleedSpeed(float F);
	/** Struck the sea in a dive: punch through, keep most of the speed, swoop back up level. */
	void Plunge(float Keep);
	void Snare(float Seconds = 4.f);
	void ClearRoll();
	void ClearKeys();

private:
	void StartRoll(int32 Dir);
	EDragonFlightMode PrevVertMode = EDragonFlightMode::Level;
	float PrevClimbVel = 0.f;
};

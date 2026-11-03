#include "DragonFlightModel.h"

namespace
{
	float SmoothStep01(float X, float A, float B)
	{
		const float T = FMath::Clamp((X - A) / (B - A), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}
}

// ---------------------------------------------------------------------------
// Tap gate — one tap arms one press; "armed" lasts the whole second press.
// ---------------------------------------------------------------------------
void FDragonTapGate::Update(float Dt, int32 Want)
{
	Age += Dt;
	bFired = false;
	if (Want != 0 && Want == Dir)
	{
		Held += Dt;
		return;
	}
	// Released or reversed: classify the press that just ended.
	if (Dir != 0)
	{
		if (Held > 0.f && Held <= TapMax) { TapDir = Dir; Age = 0.f; }
		else TapDir = 0;
	}
	Held = 0.f;
	Dir = Want;
	bArmed = Want != 0 && Want == TapDir && Age <= Gap;
	if (bArmed) { TapDir = 0; bFired = true; }
}

float FDragonFlightModel::AngleDelta(float From, float To)
{
	float D = FMath::Fmod(To - From, 2.f * PI);
	if (D > PI) D -= 2.f * PI;
	if (D < -PI) D += 2.f * PI;
	return D;
}

// ---------------------------------------------------------------------------
// One frame of flight (controls.update)
// ---------------------------------------------------------------------------
void FDragonFlightModel::Update(float DtIn, const FDragonFlightInput& In, FVector& PosM)
{
	const float Dt = FMath::Min(DtIn, 0.05f);
	if (Dt <= 0.f) return;
	Time += Dt;
	bStalledThisFrame = false;

	if (!bHeadingSeeded) { Heading = In.CamYaw + PI; bHeadingSeeded = true; }

	// 2. Align request — face where the camera looks.
	if (In.bAlignRequest) AlignTarget = In.CamYaw + PI;

	// 3-4. Speed ladder ----------------------------------------------------------
	const float MoveInput = FMath::Clamp(In.Move, -1.f, 1.f);
	const bool bSprinting = In.bSprint;
	const float Top = bSprinting ? FMath::Max(PEDAL_MAX, CurrentForwardSpeed) : CurrentForwardSpeed;
	const float PedalTarget = MoveInput > 0.f ? MoveInput * Top : MoveInput < 0.f ? MoveInput * REVERSE_SPEED : 0.f;

	bBursting = In.bBurst;
	TurboGate.Update(Dt, bBursting ? 1 : 0);
	bTurbo = bBursting && (TurboGate.bArmed || bSprinting);
	const float SpeedTarget = bTurbo ? TURBO_SPEED : bBursting ? FMath::Max(BURST_SPEED, PedalTarget) : PedalTarget;
	const float Rate = bTurbo ? TURBO_GAIN
		: bBursting ? BURST_GAIN
		: SpeedTarget > Airspeed ? PEDAL_GAIN
		: Airspeed > PEDAL_MAX ? BURST_BLEED
		: PEDAL_BLEED;
	if (Mode == EDragonFlightMode::Level && RecoverHold <= 0.f)
	{
		Airspeed += (SpeedTarget - Airspeed) * Damp(Rate, Dt);
	}
	RecoverHold = FMath::Max(0.f, RecoverHold - Dt);

	// 5. Knife edge / barrel roll ---------------------------------------------
	int32 KnifeWant = In.KnifeWant;
	RollGate.Update(Dt, KnifeWant);
	if (RollGate.bFired) StartRoll(RollGate.Dir);
	if (RollDir != 0) KnifeWant = 0;
	if (KnifeWant != 0 || RollDir != 0) KnifeCharge = FMath::Max(0.f, KnifeCharge - Dt);
	else KnifeCharge = FMath::Min(KNIFE_HOLD, KnifeCharge + Dt * (KNIFE_HOLD / KNIFE_RECOVER));
	const float Stamina = SmoothStep01(KnifeCharge / KNIFE_HOLD, 0.f, 0.4f);
	const float KnifeTarget = KnifeWant * Stamina;
	KnifeAmount += (KnifeTarget - KnifeAmount) * Damp(FMath::Abs(KnifeTarget) > FMath::Abs(KnifeAmount) ? KNIFE_IN : KNIFE_OUT, Dt);

	// 6. Turning ---------------------------------------------------------------
	float TurnInput = FMath::Clamp(In.Turn, -1.f, 1.f);
	if (Snared > 0.f)
	{
		Snared = FMath::Max(0.f, Snared - Dt);
		// Wiggle out: each crossing of the stick the other way shakes some of it off.
		const int32 Cross = TurnInput > AXIS_THRESHOLD ? 1 : TurnInput < -AXIS_THRESHOLD ? -1 : 0;
		if (Cross != 0 && Cross != LastSnareCross)
		{
			if (LastSnareCross != 0) Snared = FMath::Max(0.f, Snared - SNARE_SHAKE);
			LastSnareCross = Cross;
		}
		TurnInput *= SNARE_STEER;
		AlignTarget.Reset();
	}
	if (TurnInput != 0.f) AlignTarget.Reset();
	YawMaxNow = FMath::Min(YawRateMax, TurnG / FMath::Max(Airspeed, 1.f));
	if (AlignTarget.IsSet())
	{
		const float D = AngleDelta(Heading, AlignTarget.GetValue());
		if (FMath::Abs(D) < ALIGN_TOLERANCE) AlignTarget.Reset();
		else YawRate += (FMath::Clamp(D * ALIGN_GAIN, -YawMaxNow, YawMaxNow) - YawRate) * Damp(YAW_LAMBDA, Dt);
	}
	else if (TurnInput != 0.f)
	{
		YawRate += (TurnInput * YawMaxNow - YawRate) * Damp(YAW_LAMBDA, Dt);
	}
	else
	{
		YawRate *= FMath::Exp(-YAW_DAMP * Dt);
		if (FMath::Abs(YawRate) < YAW_DEADZONE) YawRate = 0.f;
	}
	YawRate = FMath::Clamp(YawRate, -YawMaxNow, YawMaxNow);
	Heading += YawRate * Dt;

	// World edge: a current, not a wall. Chebyshev distance, only pulls when pointed outward.
	{
		const float Cheb = FMath::Max(FMath::Abs(PosM.X), FMath::Abs(PosM.Y));
		EdgeT = FMath::Clamp((Cheb - EDGE_SOFT) / (EDGE_HARD - EDGE_SOFT), 0.f, 1.f);
		if (EdgeT > 0.f)
		{
			const float Home = FMath::Atan2(-PosM.X, -PosM.Y);
			const float D = AngleDelta(Heading, Home);
			const float Outward = FMath::Clamp(-FMath::Cos(D), 0.f, 1.f);
			const float Pull = EDGE_TURN * EdgeT * EdgeT * Outward;
			Heading += FMath::Sign(D) * FMath::Min(Pull * Dt, FMath::Abs(D));
		}
	}
	// Knife edge slices toward the low wing, outside the yaw clamp.
	Heading += KnifeAmount * KNIFE_YAW * Dt;

	// 7. Strafe ----------------------------------------------------------------
	const float StrafeIn = FMath::Clamp(In.Strafe, -1.f, 1.f);
	StrafeVel += StrafeIn * STRAFE_ACCEL * Dt;
	if (StrafeIn == 0.f) StrafeVel *= FMath::Exp(-STRAFE_DAMP * Dt);
	StrafeVel = FMath::Clamp(StrafeVel, -STRAFE_MAX, STRAFE_MAX);

	// 8. Vertical axis + manoeuvre state machine --------------------------------
	const float VerticalInput = FMath::Clamp(In.Vertical, -1.f, 1.f);
	const bool bWantUp = VerticalInput > AXIS_THRESHOLD;
	const bool bWantDown = VerticalInput < -AXIS_THRESHOLD;
	VertGate.Update(Dt, bWantUp ? 1 : bWantDown ? -1 : 0);
	const bool bCommitted = Snared <= 0.f && VertGate.bArmed && VertGate.Held >= MANOEUVRE_HOLD;

	ModeT += Dt;
	if (Snared > 0.f && Mode != EDragonFlightMode::Level)
	{
		SetMode(EDragonFlightMode::Level);
		DiveCommit = 0.f;
		RecoverHold = 0.f;
	}

	switch (Mode)
	{
	case EDragonFlightMode::Level:
		if (bCommitted && bWantUp && Airspeed >= CLIMB_ENTRY_SPEED) SetMode(EDragonFlightMode::Zoom);
		else if (bCommitted && bWantDown) SetMode(Airspeed >= DIVE_ENTRY_SPEED ? EDragonFlightMode::Dive : EDragonFlightMode::Drop);
		break;
	case EDragonFlightMode::Drop:
		Airspeed += (DROP_SPEED - Airspeed) * Damp(DROP_DRAG, Dt);
		if (!bWantDown || bWantUp) SetMode(EDragonFlightMode::Level);
		break;
	case EDragonFlightMode::Zoom:
		Airspeed = FMath::Max(0.f, Airspeed - (CLIMB_DRAG + CLIMB_DRAG_V2 * Airspeed * Airspeed) * Dt);
		PathAngle += (HALF_PI - PathAngle) * Damp(NOSE_OVER_RATE, Dt);
		if (!bWantUp) SetMode(EDragonFlightMode::Recover);
		else if (Airspeed <= STALL_SPEED) { SetMode(EDragonFlightMode::Stall); bStalledThisFrame = true; }
		break;
	case EDragonFlightMode::Stall:
		Airspeed = FMath::Max(0.f, Airspeed - CLIMB_DRAG * 0.35f * Dt);
		if (ModeT > STALL_HANG) PathAngle += (-HALF_PI - PathAngle) * Damp(NOSE_OVER_RATE, Dt);
		if (PathAngle < -0.7f) { SetMode(EDragonFlightMode::Dive); DiveCommit = STALL_DIVE_COMMIT; }
		break;
	case EDragonFlightMode::Dive:
		PathAngle += (-HALF_PI - PathAngle) * Damp(NOSE_OVER_RATE, Dt);
		Airspeed = FMath::Min(DIVE_MAX, Airspeed + DIVE_GRAVITY * Dt * (-FMath::Sin(PathAngle)));
		DiveCommit -= Dt;
		if ((!bWantDown || bWantUp) && DiveCommit <= 0.f) SetMode(EDragonFlightMode::Recover);
		break;
	case EDragonFlightMode::Recover:
		PathAngle += (0.f - PathAngle) * Damp(RECOVER_RATE, Dt);
		RecoverHold = RECOVER_HOLD;
		if (FMath::Abs(PathAngle) < 0.10f) { SetMode(EDragonFlightMode::Level); ClimbVel = 0.f; }
		break;
	}

	const bool bManoeuvring = IsManoeuvring();
	const bool bTrimming = !bManoeuvring && Airspeed >= CLIMB_ENTRY_SPEED;
	const float VertRate = bTrimming ? TRIM_RATE : FMath::Min(VERT_HOVER + FMath::Abs(Airspeed) * VERT_PER_SPEED, CLIMB_RATE_CAP);
	const float VertTarget = VerticalInput * VertRate;
	const float VertK = (FMath::Abs(VertTarget) > 0.98f * FMath::Abs(ClimbVel) && VerticalInput != 0.f) ? VERT_LAMBDA : VERT_RELEASE;
	if (!bManoeuvring) ClimbVel += (VertTarget - ClimbVel) * Damp(VertK, Dt);
	if (Mode == EDragonFlightMode::Drop) ClimbVel += (-DROP_SINK - ClimbVel) * Damp(DROP_LAMBDA, Dt);
	if (Snared > 0.f)
	{
		ClimbVel += (-SNARE_SINK - ClimbVel) * Damp(6.f, Dt);
		Airspeed += (SNARE_SPEED - Airspeed) * Damp(SNARE_DRAG, Dt);
	}

	// 9. Translate -------------------------------------------------------------
	const FVector Fwd = Forward(Heading);
	if (!bManoeuvring)
	{
		ActiveSpeed = Airspeed;
		const float ClimbRate = ClimbVel - FMath::Abs(KnifeAmount) * KNIFE_SINK;
		const float PathTarget = FMath::Atan2(ClimbRate, FMath::Max(FMath::Abs(ActiveSpeed), PATH_REF_SPEED));
		PathAngle += (PathTarget - PathAngle) * Damp(PITCH_LAMBDA, Dt);
		PosM += Fwd * (ActiveSpeed * Dt);
		PosM.Z += ClimbRate * Dt;
	}
	else
	{
		const float Horizontal = Airspeed * FMath::Cos(PathAngle);
		const float ClimbRate = Airspeed * FMath::Sin(PathAngle);
		ClimbVel = ClimbRate;
		PosM += Fwd * (Horizontal * Dt);
		PosM.Z += ClimbRate * Dt;
		ActiveSpeed = Airspeed;
	}
	// Strafe, + = right; right(h) = -left(h).
	PosM -= Left(Heading) * (StrafeVel * Dt);

	// Smoothed vertical acceleration (animation only)
	VertAccel += ((ClimbVel - PrevClimbVel) / Dt - VertAccel) * Damp(6.f, Dt);
	PrevClimbVel = ClimbVel;

	// 10. Barrel roll helix ----------------------------------------------------
	if (RollDir != 0)
	{
		RollT = FMath::Min(1.f, RollT + Dt / RollTime);
		const float Theta = RollT * 2.f * PI;
		const float Swept = (Theta - ROLL_EASE * FMath::Sin(Theta)) / (2.f * PI);
		const float Lat = RollDir * (RollBulge * (1.f - FMath::Cos(Theta)) + RollShift * Swept);
		const float S = FMath::Sin(Theta);
		const float Up = RollRise * S * (S > 0.f ? 1.f : ROLL_UNDER);
		PosM += Left(Heading) * (Lat - RollLat);
		PosM.Z += Up - RollUp;
		RollLat = Lat;
		RollUp = Up;
		RollRate = RollDir * (1.f - ROLL_EASE * FMath::Cos(Theta)) / (1.f + ROLL_EASE);
		CurrentRoll = RollEntry + RollDir * (Theta - ROLL_EASE * FMath::Sin(Theta));
		if (RollT >= 1.f)
		{
			CurrentRoll -= RollDir * 2.f * PI;
			RollDir = 0; RollT = 0.f; RollLat = 0.f; RollUp = 0.f; RollRate = 0.f;
		}
	}

	// 11. Attitude ---------------------------------------------------------------
	const float BankScale = FMath::Clamp(ActiveSpeed / SPEED_CRUISE, 0.f, 1.f);
	const float Bank = ((YawRate / FMath::Max(YawMaxNow, 1e-4f)) * MAX_BANK - (StrafeVel / STRAFE_MAX) * 0.30f) * BankScale;
	const float KB = FMath::Abs(KnifeAmount);
	const float Tremble = FMath::Sin(Time * 18.6f) * FMath::Sin(Time * 7.8f) * KNIFE_TREMBLE * KB;
	const float TargetRoll = KnifeAmount * KNIFE_ANGLE + (1.f - KB) * Bank + Tremble;
	if (RollDir == 0) CurrentRoll += (TargetRoll - CurrentRoll) * Damp(BANK_LAMBDA, Dt);

	const float SlowT = 1.f - FMath::Clamp(ActiveSpeed / (SPEED_CRUISE * 2.f), 0.f, 1.f);
	const float PitchLimit = bManoeuvring ? HALF_PI + 0.08f : PITCH_MAX;
	const float TargetPitch = FMath::Clamp(PathAngle + (bManoeuvring ? 0.f : SlowT * AOA_SLOW) + KB * 0.14f
		+ (RollT > 0.f ? FMath::Sin(RollT * 2.f * PI) * ROLL_PITCH : 0.f), -PitchLimit, PitchLimit);
	CurrentPitch += (TargetPitch - CurrentPitch) * Damp(PITCH_LAMBDA, Dt);
}

void FDragonFlightModel::StartRoll(int32 Dir)
{
	if (RollDir != 0 || Snared > 0.f) return;
	if (Mode != EDragonFlightMode::Level && Mode != EDragonFlightMode::Drop) return;
	if (KnifeCharge < ROLL_COST) return;
	SetMode(EDragonFlightMode::Level);
	KnifeCharge -= ROLL_COST;
	Airspeed *= (1.f - ROLL_DRAG);
	RollDir = Dir;
	RollT = 0.f;
	RollEntry = CurrentRoll;
	RollLat = 0.f;
	RollUp = 0.f;
	const float T = GetSpeedRatio();
	RollTime = FMath::Lerp(0.58f, 0.46f, T);
	RollBulge = FMath::Lerp(2.5f, 5.f, T);
	RollRise = FMath::Lerp(4.f, 8.f, T);
	RollShift = FMath::Lerp(20.f, 34.f, T);
}

void FDragonFlightModel::ClearRoll()
{
	if (RollDir != 0) CurrentRoll -= RollDir * 2.f * PI * RollT; // unwind what was swept
	CurrentRoll = FMath::Fmod(CurrentRoll, 2.f * PI);
	RollDir = 0; RollT = 0.f; RollLat = 0.f; RollUp = 0.f; RollRate = 0.f;
}

void FDragonFlightModel::ScrapeSpeed(float F)
{
	Airspeed *= 1.f - F;
	ClimbVel *= 1.f - F;
	if (F > 0.2f) RecoverHold = FMath::Max(RecoverHold, FMath::Min(1.2f, 1.6f * F));
}

void FDragonFlightModel::BleedSpeed(float F)
{
	Airspeed *= 1.f - F;
	ClimbVel *= 0.2f;
	SetMode(EDragonFlightMode::Level);
	RecoverHold = 0.f;
	DiveCommit = 0.f;
	ClearRoll();
	if (F >= 1.f) RecoverHold = FMath::Max(RecoverHold, 1.0f);
}

void FDragonFlightModel::Snare(float Seconds)
{
	Snared = FMath::Max(Snared, Seconds);
	ClearRoll();
	SetMode(EDragonFlightMode::Level);
	Airspeed *= 0.55f;
}

void FDragonFlightModel::ClearKeys()
{
	StrafeVel = 0.f;
	PathAngle = 0.f;
	YawRate = 0.f;
	SetMode(EDragonFlightMode::Level);
	VertGate.Reset();
	RollGate.Reset();
	TurboGate.Reset();
}

FDragonFlightState FDragonFlightModel::GetFlightState() const
{
	FDragonFlightState S;
	S.SpeedT = GetSpeedRatio();
	S.Climb = FMath::Clamp(ClimbVel / CLIMB_RATE_CAP * 3.f, -1.f, 1.f);
	S.Knife = KnifeAmount;
	S.Turn = GetTurnT();
	S.Load = FMath::Sqrt(FMath::Square(YawRate * Airspeed) + FMath::Square(VertAccel)) / 9.81f;
	S.Hang = Mode == EDragonFlightMode::Stall ? 1.f : Mode == EDragonFlightMode::Zoom ? FMath::Max(0.f, (GetStallT() - 0.45f) / 0.55f) : 0.f;
	S.Roll = RollRate;
	return S;
}

FVector FDragonFlightModel::GetVelocity() const
{
	const FVector Fwd = Forward(Heading);
	if (IsManoeuvring())
	{
		return Fwd * (Airspeed * FMath::Cos(PathAngle)) + FVector(0, 0, Airspeed * FMath::Sin(PathAngle)) - Left(Heading) * StrafeVel;
	}
	return Fwd * ActiveSpeed + FVector(0, 0, ClimbVel - FMath::Abs(KnifeAmount) * KNIFE_SINK) - Left(Heading) * StrafeVel;
}

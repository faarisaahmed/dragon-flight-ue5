#include "DragonPawn.h"
#include "DragonFlight.h"
#include "DragonWorldSubsystem.h"
#include "DragonStandInComponent.h"
#include "DragonRigComponent.h"
#include "DragonPlasma.h"
#include "Camera/CameraComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "DragonWindAudio.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/GameInstance.h"
#include "WaterBodyActor.h"
#include "InputCoreTypes.h"

using F = FDragonFlightModel;

namespace
{
	constexpr float M = 100.f;              // cm per m
	constexpr float BODY_CENTRE = 1.45f;
	constexpr float SEA_LEVEL = 0.f;

	// Landing / ground (main.js, groundbody.js)
	constexpr float LAND_SPEED_T = 0.18f, LAND_AGL = 22.f, LAND_SLOPE_MAX = 0.95f, LAND_HOLD = 0.4f;
	constexpr float FOOT_R = 3.2f;
	constexpr float WALK_SPEED = 5.5f, RUN_SPEED = 15.f, WALK_TURN = 7.f, RUN_TURN = 3.6f, WALK_ACCEL = 7.f, RUN_ACCEL = 3.6f;
	constexpr float REVERSE_CONE = 120.f * PI / 180.f, REVERSE_FRAC = 0.5f;
	constexpr float FLARE_BRAKE = 2.2f, FLARE_SINK = 9.f, FLARE_TOUCH = 1.8f;
	constexpr float STEP_MAX = 1.1f, ATT_HZ = 3.f, ATT_ZETA = 0.8f, ATT_MAX = 0.7f;
	constexpr float G = 9.81f, GROUND_MU = 0.9f;

	// Health (health.js)
	constexpr float SAFE_CLOSING = 26.f, LETHAL_CLOSING = 130.f;

	// Camera (main.js)
	constexpr float PITCH_LIMIT = 1.5f, LOOK_SPIKE = 160.f, TRAIL_DELAY = 0.7f;
	constexpr float LEAD_MAX = 10.f, LEAD_CURVE = 1.5f, LOOK_HEIGHT = 1.5f;
	constexpr float DIST_SPEED = 10.f, OCCLUDE_CLEAR = 5.f, BOOM_HARD_MIN = 9.f, BOOM_MAX_LAG = 12.f;
	constexpr float AIM_BOOM = 0.55f, AIM_SHOULDER = 4.2f, AIM_RISE = 1.6f, AIM_FOV = 12.f;
	constexpr float FOV_BASE = 70.f, FOV_SPEED_GAIN = 24.f, FOV_TURBO = 14.f, FOV_LAMBDA = 3.f;
	constexpr int32 OCCLUDE_STEPS = 12;
	constexpr float PAD_LOOK_YAW = 2.9f, PAD_LOOK_PITCH = 2.0f;

	// Fire (plasma.js)
	constexpr float FIRE_COOLDOWN = 0.22f, RECHARGE_TIME = 2.6f, MUZZLE_SPEED = 600.f;
	constexpr int32 MAX_SHOTS = 6;

	float Smooth(float X, float A, float B)
	{
		const float T = FMath::Clamp((X - A) / (B - A), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}

	/** Boom direction from (camYaw, camPitch), web convention -> UE axes. */
	FVector BoomDir(float Yaw, float Pitch)
	{
		return FVector(FMath::Cos(Pitch) * FMath::Sin(Yaw), FMath::Cos(Pitch) * FMath::Cos(Yaw), FMath::Sin(Pitch));
	}
}

ADragonPawn::ADragonPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	AutoPossessPlayer = EAutoReceiveInput::Player0;

	Feet = CreateDefaultSubobject<USceneComponent>(TEXT("Feet"));
	RootComponent = Feet;

	Attitude = CreateDefaultSubobject<USceneComponent>(TEXT("Attitude"));
	Attitude->SetupAttachment(Feet);
	Attitude->SetRelativeLocation(FVector(0, 0, BODY_CENTRE * M));

	MeshPivot = CreateDefaultSubobject<USceneComponent>(TEXT("MeshPivot"));
	MeshPivot->SetupAttachment(Attitude);
	MeshPivot->SetRelativeLocation(FVector(0, 0, -BODY_CENTRE * M));

	DragonMesh = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("DragonMesh"));
	DragonMesh->SetupAttachment(MeshPivot);
	DragonMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DragonMesh->bCastDynamicShadow = true;
	DragonMesh->SetGenerateOverlapEvents(false);

	StandIn = CreateDefaultSubobject<UDragonStandInComponent>(TEXT("StandIn"));
	StandIn->SetupAttachment(MeshPivot);

	Rig = CreateDefaultSubobject<UDragonRigComponent>(TEXT("Rig"));

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Feet);
	Camera->SetUsingAbsoluteLocation(true);
	Camera->SetUsingAbsoluteRotation(true);
	Camera->bConstrainAspectRatio = false;
	Camera->PostProcessBlendWeight = 0.f;

	WindAudio = CreateDefaultSubobject<UDragonWindAudio>(TEXT("WindAudio"));
	WindAudio->SetupAttachment(Feet);

	PlasmaClass = ADragonPlasma::StaticClass();
}

void ADragonPawn::BeginPlay()
{
	Super::BeginPlay();
	PosM = GetActorLocation() / M;
	PrevPosM = PosM;
	SetActorScale3D(FVector(DragonScale));
	if (!DragonMesh->GetSkinnedAsset())
	{
		USkeletalMesh* Asset = DragonAsset.IsNull() ? nullptr : DragonAsset.LoadSynchronous();
		if (!Asset) Asset = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Dragon/SK_Dragon.SK_Dragon"));
		if (Asset) DragonMesh->SetSkinnedAssetAndUpdate(Asset);
	}
	DragonMesh->SetRelativeRotation(FRotator(0.f, DragonMeshYaw, 0.f));
	const bool bHasMesh = DragonMesh->GetSkinnedAsset() != nullptr;
	DragonMesh->SetVisibility(bHasMesh);
	StandIn->SetStandInVisible(!bHasMesh);
	if (bHasMesh) Rig->Bind(DragonMesh);

	// Face away from the camera, as the web game does on its first frame.
	CamYaw = F::YawToHeading(GetActorRotation().Yaw) + PI;
	Flight.SetHeading(CamYaw + PI);
}

void ADragonPawn::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	Camera->GetCameraView(DeltaTime, OutResult);
}

// ===========================================================================
// Input
// ===========================================================================
bool ADragonPawn::AnyDown(std::initializer_list<FKey> Keys) const
{
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC) return false;
	for (const FKey& K : Keys) if (PC->IsInputKeyDown(K)) return true;
	return false;
}

bool ADragonPawn::AnyPressed(std::initializer_list<FKey> Keys) const
{
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC) return false;
	for (const FKey& K : Keys) if (PC->WasInputKeyJustPressed(K)) return true;
	return false;
}

FVector2D ADragonPawn::ShapeStick(FVector2D V)
{
	// gamepad.js: radial deadzone 0.10, curve 0.78 t^2 + 0.22 t up to 0.98.
	const float Mag = V.Size();
	if (Mag < 0.10f) return FVector2D::ZeroVector;
	const float T = FMath::Min(1.f, (Mag - 0.10f) / (0.98f - 0.10f));
	const float Shaped = 0.78f * T * T + 0.22f * T;
	return V * (Shaped / Mag);
}

float ADragonPawn::ShapeTrigger(float V)
{
	return V < 0.045f ? 0.f : (V - 0.045f) / (1.f - 0.045f);
}

ADragonPawn::FFrameInput ADragonPawn::GatherInput(float Dt)
{
	FFrameInput In;
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC) return In;

	// Keyboard — the web game's default "wasd" scheme (keymap.js).
	const bool W = AnyDown({ EKeys::W }), S = AnyDown({ EKeys::S }), A = AnyDown({ EKeys::A }), D = AnyDown({ EKeys::D });
	In.Move = (W ? 1.f : 0.f) - (S ? 1.f : 0.f);
	In.Turn = (A ? 1.f : 0.f) - (D ? 1.f : 0.f); // + left
	In.WalkFwd = In.Move;
	In.WalkSide = (D ? 1.f : 0.f) - (A ? 1.f : 0.f);
	In.Vertical = (AnyDown({ EKeys::SpaceBar }) ? 1.f : 0.f) - (AnyDown({ EKeys::K, EKeys::C }) ? 1.f : 0.f);
	In.bUpPressed = AnyPressed({ EKeys::SpaceBar });
	In.bSprint = AnyDown({ EKeys::LeftShift, EKeys::RightShift });
	In.bBurst = AnyDown({ EKeys::J, EKeys::B, EKeys::Gamepad_FaceButton_Bottom });
	In.Strafe = (AnyDown({ EKeys::P, EKeys::Gamepad_FaceButton_Right }) ? 1.f : 0.f) - (AnyDown({ EKeys::O, EKeys::Gamepad_FaceButton_Left }) ? 1.f : 0.f);
	const bool KL = AnyDown({ EKeys::L, EKeys::Gamepad_LeftShoulder });
	const bool KR = AnyDown({ EKeys::Semicolon, EKeys::Gamepad_RightShoulder });
	In.Knife = KR ? -1 : KL ? 1 : 0;
	In.bLand = AnyDown({ EKeys::I, EKeys::R });
	In.bFire = AnyDown({ EKeys::U, EKeys::LeftMouseButton, EKeys::Gamepad_RightTrigger });
	In.bFirePressed = AnyPressed({ EKeys::U, EKeys::LeftMouseButton });
	In.bAim = AnyDown({ EKeys::E, EKeys::RightMouseButton });
	In.bAlignDragon = AnyPressed({ EKeys::H, EKeys::Gamepad_DPad_Up });
	In.bRecenter = AnyPressed({ EKeys::Q, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_RightThumbstick });

	// Gamepad sticks and triggers.
	const FVector2D LS = ShapeStick(FVector2D(PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftX), PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftY)));
	const FVector2D RS = ShapeStick(FVector2D(PC->GetInputAnalogKeyState(EKeys::Gamepad_RightX), PC->GetInputAnalogKeyState(EKeys::Gamepad_RightY)));
	const float R2 = ShapeTrigger(PC->GetInputAnalogKeyState(EKeys::Gamepad_RightTriggerAxis));
	const float L2 = ShapeTrigger(PC->GetInputAnalogKeyState(EKeys::Gamepad_LeftTriggerAxis));
	if (FMath::Abs(LS.Y) > 0.02f) In.Move = FMath::Clamp(In.Move + (float)(FMath::Sign(LS.Y) * LS.Y * LS.Y), -1.f, 1.f);
	In.Turn = FMath::Clamp(In.Turn - (float)LS.X, -1.f, 1.f);
	const float PadV = bPadClimbInvert ? (R2 - L2) : (L2 - R2);
	In.Vertical = FMath::Clamp(In.Vertical + PadV, -1.f, 1.f);
	In.bPadUpTakeoff = (bPadClimbInvert ? R2 : L2) > 0.6f;
	// The fire trigger is R2 on pad only when climb is not on R2; by default fire = mouse/U.
	if (bPadClimbInvert) In.bFire = AnyDown({ EKeys::U, EKeys::LeftMouseButton });
	{
		// On foot the pad gets an extra 0.12 per-axis deadzone and runs past 0.92.
		auto DZ = [](float V) { return FMath::Abs(V) < 0.12f ? 0.f : V; };
		In.WalkFwd += DZ(LS.Y);
		In.WalkSide += DZ(LS.X);
		In.bPadRun = LS.Size() > 0.92f;
	}

	// Look: mouse (buffered, drained at lambda 34) and right stick.
	float MX = 0.f, MY = 0.f;
	PC->GetInputMouseDelta(MX, MY);
	if (FMath::Abs(MX) > LOOK_SPIKE || FMath::Abs(MY) > LOOK_SPIKE) { MX = 0.f; MY = 0.f; }
	PendingYaw -= MX * MouseSensitivity * (bInvertX ? -1.f : 1.f);
	// UE mouse Y is up-positive. Web: mouse up raises the camera over him (camPitch increases).
	PendingPitch += MY * MouseSensitivity * (bInvertY ? -1.f : 1.f);
	const float K = F::Damp(34.f, Dt);
	In.LookYaw = PendingYaw * K;
	In.LookPitch = PendingPitch * K;
	PendingYaw -= In.LookYaw;
	PendingPitch -= In.LookPitch;
	In.LookYaw -= RS.X * PAD_LOOK_YAW * PadLookSpeed * Dt;
	In.LookPitch += RS.Y * PAD_LOOK_PITCH * PadLookSpeed * Dt;
	In.bLooked = FMath::Abs(MX) + FMath::Abs(MY) > 0.f || RS.SizeSquared() > 0.f;

	In.BoomDelta = (AnyDown({ EKeys::Gamepad_DPad_Right }) ? 1.f : 0.f) - (AnyDown({ EKeys::Gamepad_DPad_Left }) ? 1.f : 0.f);
	if (AnyPressed({ EKeys::MouseScrollDown })) In.BoomDelta += 6.f / FMath::Max(Dt, 1e-3f) / 22.f;
	if (AnyPressed({ EKeys::MouseScrollUp })) In.BoomDelta -= 6.f / FMath::Max(Dt, 1e-3f) / 22.f;
	if (AnyPressed({ EKeys::Gamepad_LeftThumbstick })) Flight.CurrentForwardSpeed = F::SPEED_CRUISE;
	return In;
}

// ===========================================================================
// Tick
// ===========================================================================
void ADragonPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(DeltaSeconds, 0.1f);
	if (Dt <= 0.f) return;

	// Pick up anything that moved us (teleport, editor).
	PrevPosM = PosM;
	const FFrameInput In = GatherInput(Dt);

	Prompt.Reset();
	if (bGrounded) TickGround(Dt, In);
	else TickFlight(Dt, In);

	TickHealth(Dt);
	TickFire(Dt, In);
	ApplyTransform();

	// Rig / stand-in animation read the flight state.
	const FDragonFlightState FS = Flight.GetFlightState();
	Heave = 0.f;
	if (StandIn && StandIn->IsStandInVisible()) Heave = StandIn->Animate(Dt, FS, bGrounded, WalkSpeed, bFlaring);
	if (Rig && Rig->IsBound()) Heave = Rig->Animate(Dt, FS, bGrounded, WalkSpeed, bFlaring);
	MeshPivot->SetRelativeLocation(FVector(0, 0, (-BODY_CENTRE + Heave) * M));

	TickCamera(Dt, In);
	WindAudio->SetFlight(bGrounded ? 0.f : Flight.GetSpeedRatio(), bGrounded ? 0.f : Flight.GetTurboT(), bGrounded);
}

void ADragonPawn::TickFlight(float Dt, const FFrameInput& In)
{
	FDragonFlightInput FI;
	FI.Move = In.Move;
	FI.bSprint = In.bSprint;
	FI.bBurst = In.bBurst;
	FI.Turn = In.Turn;
	FI.Vertical = In.Vertical;
	FI.Strafe = In.Strafe;
	FI.KnifeWant = In.Knife;
	FI.bAlignRequest = In.bAlignDragon;
	FI.CamYaw = CamYaw;
	Flight.Update(Dt, FI, PosM);

	if (bCollide) FlightFloor(Dt);
	else bFloorContact = false;

	const float Ground = FMath::Max(SolidAtM(PosM.X, PosM.Y), SEA_LEVEL);
	Agl = PosM.Z - Ground;
	CheckLanding(Dt, In);
}

// --- The flight floor (main.js 2476-2612) -----------------------------------
void ADragonPawn::FlightFloor(float Dt)
{
	const FVector Fwd = F::Forward(Flight.Heading);
	float Rock = SolidAtM(PosM.X, PosM.Y);
	const float Reach = FMath::Abs(Flight.ActiveSpeed) * 0.25f;
	for (int32 i = 1; i <= 3; ++i)
	{
		const FVector P = PosM + Fwd * (Reach * i / 3.f);
		Rock = FMath::Max(Rock, SolidAtM(P.X, P.Y));
	}
	const float Floor = FMath::Max(Rock + 6.f, SEA_LEVEL + 10.f);
	if (PosM.Z >= Floor) { bFloorContact = false; return; }

	const bool bFirst = !bFloorContact;
	bFloorContact = true;
	const float Under = SolidAtM(PosM.X, PosM.Y);
	const float B = 25.f; // IMPACT_BASELINE
	const FVector2D Af(Fwd.X, Fwd.Y);
	const float GF = (SolidAtM(PosM.X + Af.X * B, PosM.Y + Af.Y * B) - Under) / B;
	const float GS = (SolidAtM(PosM.X - Af.Y * B, PosM.Y + Af.X * B) - Under) / B;
	const float Gx = Af.X * GF - Af.Y * GS;
	const float Gy = Af.Y * GF + Af.X * GS;
	const float Inv = 1.f / FMath::Sqrt(Gx * Gx + Gy * Gy + 1.f);
	const FVector N(-Gx * Inv, -Gy * Inv, Inv);
	const float Sp = Flight.ActiveSpeed;
	const FVector V(Af.X * Sp, Af.Y * Sp, Flight.ClimbVel);

	const float Hit = Impact(V, N, Under <= SEA_LEVEL);
	if (Hit > 0.f)
	{
		Flight.BleedSpeed(1.f);
	}
	else
	{
		const float Len = FMath::Max((float)V.Size(), 1e-3f);
		const float Into = FMath::Max(0.f, (float)(-FVector::DotProduct(V, N) / Len));
		if (bFirst && Into > 0.3f) Flight.BleedSpeed(1.f);
		else
		{
			const float Lose = bFirst ? FMath::Min(0.85f, 1.4f * Into) : 1.f - FMath::Exp(-Dt * (0.9f + 4.f * Into));
			Flight.ScrapeSpeed(Lose);
			if (!bFirst && bDamage && Sp > 45.f)
			{
				ScrapeTimer += Dt;
				if (ScrapeTimer >= 0.3f) { ScrapeTimer = 0.f; Health -= (Sp - 45.f) * 0.02f; SinceHit = 0.f; }
			}
		}
	}
	PosM.Z = Floor;
}

float ADragonPawn::Impact(const FVector& V, const FVector& N, bool bWater)
{
	if (!bDamage || HitGrace > 0.f) return 0.f;
	const float Closing = -FVector::DotProduct(V, N);
	if (Closing <= SAFE_CLOSING) return 0.f;
	const float T = FMath::Min(1.f, (Closing - SAFE_CLOSING) / (LETHAL_CLOSING - SAFE_CLOSING));
	float Dmg = 100.f * FMath::Pow(T, 1.5f);
	if (bWater) Dmg *= 0.5f;
	if (N.Z > 0.75f && V.Z < 0.f) Dmg *= 1.25f;
	Health -= Dmg;
	HitGrace = 0.9f;
	SinceHit = 0.f;
	return Dmg;
}

void ADragonPawn::TickHealth(float Dt)
{
	HitGrace = FMath::Max(0.f, HitGrace - Dt);
	SinceHit += Dt;
	if (SinceHit > 6.f) Health = FMath::Min(100.f, Health + 7.f * Dt);
	if (Health <= 0.f) Respawn();
}

void ADragonPawn::Respawn()
{
	Health = 100.f;
	// 28 m above the nearest gentle dry ground: walk outward in a spiral until we find some.
	FVector Best = PosM;
	for (int32 Ring = 0; Ring < 24; ++Ring)
	{
		bool bFound = false;
		for (int32 k = 0; k < 12 && !bFound; ++k)
		{
			const float A = k * PI / 6.f;
			const float R = Ring * 40.f;
			const float X = PosM.X + FMath::Cos(A) * R, Y = PosM.Y + FMath::Sin(A) * R;
			const float H = SolidAtM(X, Y);
			float P, Ro, Steep;
			GroundAttitude(X, Y, H, P, Ro, Steep);
			if (H > SEA_LEVEL + 1.f && Steep < 0.5f) { Best = FVector(X, Y, H + 28.f); bFound = true; }
		}
		if (bFound) break;
	}
	TeleportToM(Best, true);
	Flight.BleedSpeed(1.f);
}

void ADragonPawn::TeleportToM(const FVector& NewPosM, bool bAirborne)
{
	PosM = NewPosM;
	PrevPosM = NewPosM;
	if (bAirborne && bGrounded) { bGrounded = false; bFlaring = false; Flight.SetHeading(WalkYaw); }
	CamVel = FVector::ZeroVector;
	FocusLag = FVector::ZeroVector;
	bCamInit = false;
	ApplyTransform();
}

// --- Landing (main.js 1183-1236, 2769-2825) --------------------------------
void ADragonPawn::GroundAttitude(float X, float Y, float H, float& OutPitch, float& OutRoll, float& OutSteep) const
{
	const FVector Fw = F::Forward(bGrounded ? WalkYaw : Flight.Heading);
	const FVector Ri = -F::Left(bGrounded ? WalkYaw : Flight.Heading);
	const float HF = SolidAtM(X + Fw.X * FOOT_R, Y + Fw.Y * FOOT_R);
	const float HB = SolidAtM(X - Fw.X * FOOT_R, Y - Fw.Y * FOOT_R);
	const float HR = SolidAtM(X + Ri.X * FOOT_R, Y + Ri.Y * FOOT_R);
	const float HL = SolidAtM(X - Ri.X * FOOT_R, Y - Ri.Y * FOOT_R);
	OutPitch = FMath::Atan2(HF - HB, 2.f * FOOT_R);
	OutRoll = FMath::Atan2(HR - HL, 2.f * FOOT_R);
	OutSteep = FMath::Sqrt(OutPitch * OutPitch + OutRoll * OutRoll);
}

void ADragonPawn::CheckLanding(float Dt, const FFrameInput& In)
{
	const float GroundY = SolidAtM(PosM.X, PosM.Y);
	const bool bOverLand = GroundY > SEA_LEVEL + 0.5f;
	const float AglLand = PosM.Z - (GroundY + 6.f);
	const bool bSlow = Flight.GetSpeedRatio() < LAND_SPEED_T;
	bool bTooSteep = false;
	if (bOverLand && AglLand < LAND_AGL)
	{
		float P, R, Steep;
		GroundAttitude(PosM.X, PosM.Y, GroundY, P, R, Steep);
		bTooSteep = Steep > LAND_SLOPE_MAX;
	}
	const bool bCanLand = bOverLand && AglLand < LAND_AGL && bSlow && !bTooSteep;
	const bool bSinking = Flight.ClimbVel < -3.f;

	if (bCanLand && (In.bLand || bSinking))
	{
		LandHold += In.bLand ? Dt : 0.6f * Dt;
		if (LandHold > LAND_HOLD) { Land(); return; }
	}
	else if (!In.bLand)
	{
		LandHold = FMath::Max(0.f, LandHold - 2.f * Dt);
	}

	// Prompts name the failing condition and the key that fixes it.
	if (bCanLand) Prompt = TEXT("Hold I or K to land");
	else if (In.bLand || In.Vertical < -0.5f)
	{
		if (!bOverLand && AglLand < 140.f) Prompt = TEXT("No land below — find an island");
		else if (bOverLand && !bSlow && AglLand < LAND_AGL) Prompt = TEXT("Too fast to land — let go of W");
		else if (bTooSteep) Prompt = TEXT("Too steep to stand");
		else if (bOverLand && AglLand < 260.f) Prompt = TEXT("Too high to land — hold K to drop");
	}
	if (Flight.EdgeT > 0.45f) Prompt = TEXT("He will not go further. The chart ends here.");
	else if (Flight.EdgeT > 0.02f) Prompt = TEXT("The last of the chart. He is drifting back.");
}

void ADragonPawn::Land()
{
	bGrounded = true;
	bFlaring = true;
	WalkYaw = Flight.Heading;
	Flight.ClearRoll();
	Flight.Snared = 0.f;
	LandHold = 0.f;
	// The incoming velocity is the camera's smoothed measured velocity.
	GroundVel = CamVel;
	GroundPitch = Flight.CurrentPitch * 0.f;
	GroundRoll = 0.f;
	Unsupported = 0.f;
}

void ADragonPawn::TakeOff()
{
	bGrounded = false;
	bFlaring = false;
	Flight.SetHeading(WalkYaw);
	PosM.Z += 3.f;
	Flight.ClimbVel = FMath::Max(Flight.ClimbVel, 0.f);
}

// --- On foot (main.js 2667-2768, groundbody.js simplified) ------------------
void ADragonPawn::TickGround(float Dt, const FFrameInput& In)
{
	if (In.bUpPressed || In.bPadUpTakeoff) { TakeOff(); return; }

	const bool bRunning = In.bSprint || In.bPadRun;
	const float FwdIn = FMath::Clamp(In.WalkFwd, -1.f, 1.f);
	const float SideIn = FMath::Clamp(In.WalkSide, -1.f, 1.f);
	const float Mag = FMath::Min(1.f, FMath::Sqrt(FwdIn * FwdIn + SideIn * SideIn));
	const float CamFwd = CamYaw + PI;
	const FVector Wish = F::Forward(CamFwd) * FwdIn + F::Forward(CamFwd - HALF_PI) * SideIn;
	const float GearT = FMath::Clamp((FMath::Abs(WalkSpeed) - WALK_SPEED) / (RUN_SPEED - WALK_SPEED), 0.f, 1.f);
	float Align = 1.f;
	bool bReversing = false;
	if (Mag > 0.01f && !bFlaring)
	{
		const float WishYaw = FMath::Atan2(Wish.X, Wish.Y);
		const float Off = F::AngleDelta(WalkYaw, WishYaw);
		if (FMath::Abs(Off) > REVERSE_CONE && WalkSpeed < 1.5f)
		{
			// Asked to go behind him from a walk or stand: he backs up.
			bReversing = true;
			const float Back = F::AngleDelta(WalkYaw, FMath::Atan2(-Wish.X, -Wish.Y));
			WalkYaw += FMath::Clamp(Back, -WALK_TURN * 0.35f * Dt, WALK_TURN * 0.35f * Dt);
			Align = -FMath::Max(0.f, FMath::Cos(Back));
		}
		else
		{
			const float Turn = FMath::Lerp(WALK_TURN, RUN_TURN, GearT) * Dt;
			WalkYaw += FMath::Clamp(Off, -Turn, Turn);
			Align = FMath::Max(0.f, FMath::Cos(F::AngleDelta(WalkYaw, WishYaw)));
		}
	}
	const float Gear = bReversing ? WALK_SPEED * REVERSE_FRAC : (bRunning ? RUN_SPEED : WALK_SPEED);
	const FVector Fw = F::Forward(WalkYaw);
	const FVector Want = Fw * (Mag * Align * Gear);
	const float Accel = bRunning ? RUN_ACCEL : WALK_ACCEL;

	const float GroundZ = SolidAtM(PosM.X, PosM.Y);
	const float Hag = PosM.Z - GroundZ;

	if (bFlaring)
	{
		// Gravity off; brake, and sink toward the ground gently.
		GroundVel.X *= FMath::Exp(-FLARE_BRAKE * Dt);
		GroundVel.Y *= FMath::Exp(-FLARE_BRAKE * Dt);
		const float Sink = -FMath::Lerp(FLARE_TOUCH, FLARE_SINK, FMath::Clamp(Hag / 8.f, 0.f, 1.f));
		GroundVel.Z += (Sink - GroundVel.Z) * F::Damp(4.f, Dt);
		PosM += GroundVel * Dt;
		if (PosM.Z <= GroundZ + 0.02f) { PosM.Z = GroundZ; bFlaring = false; GroundVel.Z = 0.f; }
	}
	else
	{
		// Drive toward the wanted velocity, limited by friction on the slope.
		FVector Hor(GroundVel.X, GroundVel.Y, 0.f);
		FVector Drive = (Want - Hor) * Accel;
		float P, R, Steep;
		GroundAttitude(PosM.X, PosM.Y, GroundZ, P, R, Steep);
		const float Cap = GROUND_MU * G * FMath::Cos(FMath::Min(Steep, 1.4f));
		if (Drive.Size() > Cap * 1.6f) Drive = Drive.GetSafeNormal() * Cap * 1.6f;
		Hor += Drive * Dt;
		// Steep slopes beat friction: he slides downhill.
		if (Steep > 0.75f)
		{
			const FVector Downhill = (F::Forward(WalkYaw) * -FMath::Sin(P) + (-F::Left(WalkYaw)) * -FMath::Sin(R)).GetSafeNormal();
			Hor += Downhill * G * (FMath::Sin(Steep) - GROUND_MU * 0.6f * FMath::Cos(Steep)) * Dt;
		}
		// Walls: something ahead taller than a step removes the velocity into it.
		const FVector Dir = Hor.GetSafeNormal();
		if (!Dir.IsNearlyZero())
		{
			const float Ahead = SolidAtM(PosM.X + Dir.X * 1.75f, PosM.Y + Dir.Y * 1.75f);
			if (Ahead - GroundZ > STEP_MAX) Hor -= Dir * FMath::Max(0.0, FVector::DotProduct(Hor, Dir));
		}
		GroundVel = FVector(Hor.X, Hor.Y, 0.f);
		PosM.X += Hor.X * Dt;
		PosM.Y += Hor.Y * Dt;
		const float NewGround = SolidAtM(PosM.X, PosM.Y);
		// Walked off an edge / onto water -> he takes to the air.
		if (NewGround <= SEA_LEVEL + 0.3f) { PosM.Z = FMath::Max(NewGround, SEA_LEVEL); TakeOff(); return; }
		if (PosM.Z - NewGround > 2.5f)
		{
			Unsupported += Dt;
			PosM.Z -= FMath::Min(PosM.Z - NewGround, (double)(9.f * Dt + Unsupported * G * Dt));
			if (Unsupported > 0.55f) { TakeOff(); return; }
		}
		else
		{
			Unsupported = 0.f;
			// Legs absorb the terrain: follow it with a little spring.
			PosM.Z += (NewGround - PosM.Z) * F::Damp(18.f, Dt);
			if (PosM.Z < NewGround) PosM.Z = NewGround;
		}
	}
	WalkSpeed = FVector::DotProduct(FVector(GroundVel.X, GroundVel.Y, 0.f), Fw);

	// Body attitude: damped spring toward the plane through his feet.
	{
		float P, R, Steep;
		GroundAttitude(PosM.X, PosM.Y, SolidAtM(PosM.X, PosM.Y), P, R, Steep);
		P = FMath::Clamp(P, -ATT_MAX, ATT_MAX);
		R = FMath::Clamp(R, -ATT_MAX, ATT_MAX);
		const float W = 2.f * PI * ATT_HZ;
		GroundPitchVel += ((P - GroundPitch) * W * W - 2.f * ATT_ZETA * W * GroundPitchVel) * Dt;
		GroundRollVel += ((R - GroundRoll) * W * W - 2.f * ATT_ZETA * W * GroundRollVel) * Dt;
		GroundPitch += GroundPitchVel * Dt;
		GroundRoll += GroundRollVel * Dt;
	}
	Agl = 0.f;
	Prompt = TEXT("");
}

// ===========================================================================
// Terrain queries
// ===========================================================================
float ADragonPawn::SolidAtM(float X, float Y) const
{
	UWorld* World = GetWorld();
	if (!World) return 0.f;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DragonGround), false, this);
	FCollisionObjectQueryParams O;
	O.AddObjectTypesToQuery(ECC_WorldStatic);
	O.AddObjectTypesToQuery(ECC_WorldDynamic);
	const FVector From(X * M, Y * M, 4000.f * M);
	const FVector To(X * M, Y * M, -800.f * M);
	TArray<FHitResult> Hits;
	if (World->LineTraceMultiByObjectType(Hits, From, To, O, Q))
	{
		for (const FHitResult& H : Hits)
		{
			const AActor* A = H.GetActor();
			if (A && (A->IsA<AWaterBody>() || A == this)) continue;
			return H.ImpactPoint.Z / M;
		}
	}
	// No collision there (or no landscape yet): fall back to the baked heightmap.
	if (const UGameInstance* GI = World->GetGameInstance())
	{
		if (const UDragonWorldSubsystem* WS = GI->GetSubsystem<UDragonWorldSubsystem>())
		{
			return WS->HeightAt(X * M, Y * M) / M;
		}
	}
	return -200.f;
}

// ===========================================================================
// Camera (main.js 213-305, 2945-3226)
// ===========================================================================
float ADragonPawn::ViewAspect() const
{
	if (GEngine && GEngine->GameViewport)
	{
		FVector2D Size;
		GEngine->GameViewport->GetViewportSize(Size);
		if (Size.Y > 0) return Size.X / Size.Y;
	}
	return 16.f / 9.f;
}

void ADragonPawn::TickCamera(float Dt, const FFrameInput& In)
{
	const float SpeedT = bGrounded ? 0.f : Flight.GetSpeedRatio();
	const float Speed = bGrounded ? 0.f : FMath::Abs(Flight.ActiveSpeed);
	const float Heading = bGrounded ? WalkYaw : Flight.Heading;

	// Look input belongs to the player.
	if (In.bLooked) { LookIdle = 0.f; bRecentering = false; }
	else LookIdle += Dt;
	CamYaw += In.LookYaw;
	CamPitch = FMath::Clamp(CamPitch + In.LookPitch, -PITCH_LIMIT, PITCH_LIMIT);
	DistBase = FMath::Clamp(DistBase + In.BoomDelta * 22.f * Dt, 6.f, 46.f);
	if (In.bRecenter) bRecentering = true;

	if (bRecentering)
	{
		const float D = F::AngleDelta(CamYaw, Heading + PI);
		CamYaw += D * F::Damp(7.f, Dt);
		if (FMath::Abs(D) < 0.01f) bRecentering = false;
	}
	else if (!bGrounded)
	{
		const float Ramp = FMath::Clamp(LookIdle - TRAIL_DELAY, 0.f, 1.f);
		const float Lam = FMath::Lerp(0.8f, 3.6f, SpeedT);
		CamYaw += F::AngleDelta(CamYaw, Heading + PI) * F::Damp(Lam * Ramp, Dt);
		CamPitch += ((0.26f - CamPath * 0.55f) - CamPitch) * F::Damp(Lam * Ramp * 0.5f, Dt);
	}
	else
	{
		const float Ramp = FMath::Clamp(LookIdle - 1.5f, 0.f, 1.f);
		const float Away = FMath::Max(0.f, FMath::Cos(F::AngleDelta(CamYaw + PI, WalkYaw)));
		const float Moving = FMath::Clamp(FMath::Abs(WalkSpeed) / WALK_SPEED, 0.f, 1.f);
		CamYaw += F::AngleDelta(CamYaw, WalkYaw + PI) * F::Damp(0.9f * Ramp * Away * Away * Moving, Dt);
		CamPitch += (0.06f - CamPitch) * F::Damp(1.2f * Ramp, Dt);
	}

	// Aim blend (air only)
	const bool bAimWant = In.bAim && !bGrounded;
	AimBlend += ((bAimWant ? 1.f : 0.f) - AimBlend) * F::Damp(9.f, Dt);

	// Focus point
	CamPath += ((bGrounded ? 0.f : Flight.PathAngle) - CamPath) * F::Damp(2.4f, Dt);
	const float Lead = LEAD_MAX * FMath::Pow(SpeedT, LEAD_CURVE);
	const FVector Fwd = F::Forward(Heading);
	const FVector LeadXY = Fwd * (FMath::Cos(CamPath) * Lead);
	CamLead.X += (LeadXY.X - CamLead.X) * F::Damp(5.f, Dt);
	CamLead.Y += (LeadXY.Y - CamLead.Y) * F::Damp(5.f, Dt);
	CamLead.Z += (FMath::Sin(CamPath) * Lead - CamLead.Z) * F::Damp(3.f, Dt);
	const FVector Target = PosM + CamLead + FVector(0, 0, LOOK_HEIGHT * DragonScale);

	const FVector Delta = PosM - PrevPosM;
	if (!bCamInit || Delta.Size() > FMath::Max(60.f, 0.6f * Speed + 20.f))
	{
		CamVel = FVector::ZeroVector;
		FocusLag = FVector::ZeroVector;
	}
	else
	{
		CamVel = FMath::Lerp(CamVel, Delta / Dt, F::Damp(8.f, Dt));
	}
	const float TrackXY = FMath::Max(18.f, Speed / 2.f), TrackZ = FMath::Max(7.f, Speed / 6.f);
	FocusLag.X += (-CamVel.X / TrackXY - FocusLag.X) * F::Damp(10.f, Dt);
	FocusLag.Y += (-CamVel.Y / TrackXY - FocusLag.Y) * F::Damp(10.f, Dt);
	FocusLag.Z += (-CamVel.Z / TrackZ - FocusLag.Z) * F::Damp(6.f, Dt);
	FocusLag = FocusLag.GetClampedToMaxSize(2.f);
	FocusM = Target + FocusLag;

	// Boom with occlusion
	const FVector Boom = BoomDir(CamYaw, CamPitch);
	const float Dist = (bGrounded ? 10.5f : DistBase + DIST_SPEED * SpeedT) * DragonScale;
	const float Lift = bGrounded ? 1.8f : 2.f;
	float Clear = 1.f;
	float PrevMargin = 1e9f;
	for (int32 i = 1; i <= OCCLUDE_STEPS; ++i)
	{
		const float T = (float)i / OCCLUDE_STEPS;
		const FVector P = FocusM + Boom * (Dist * T) + FVector(0, 0, Lift * T);
		const float Margin = P.Z - (FMath::Max(SolidAtM(P.X, P.Y), SEA_LEVEL) + OCCLUDE_CLEAR);
		if (Margin < 0.f)
		{
			const float Frac = PrevMargin < 1e8f ? PrevMargin / (PrevMargin - Margin) : 0.f;
			Clear = (i - 1 + Frac) / OCCLUDE_STEPS;
			break;
		}
		PrevMargin = Margin;
	}
	BoomScale += (Clear - BoomScale) * F::Damp(Clear < BoomScale ? 9.f : 2.2f, Dt);
	BoomScale = FMath::Clamp(BoomScale, 0.1f, 1.f);
	const float Armed = FMath::Max(Dist * BoomScale * (1.f - (1.f - AIM_BOOM) * AimBlend), BOOM_HARD_MIN * FMath::Min(1.f, DragonScale));
	const FVector Right(Boom.Y, -Boom.X, 0.f);
	const FVector RightN = Right.GetSafeNormal();
	const FVector Desired = FocusM + Boom * Armed + FVector(0, 0, Lift) + RightN * (AIM_SHOULDER * AimBlend) + FVector(0, 0, AIM_RISE * AimBlend);
	const FVector LagV = (-CamVel / FMath::Max(9.f, Speed / 12.f)).GetClampedToMaxSize(BOOM_MAX_LAG);
	if (!bCamInit) CamRel = Desired - FocusM;
	CamRel = FMath::Lerp(CamRel, Desired - FocusM + LagV, F::Damp(9.f, Dt));
	CamPosM = FocusM + CamRel;

	// Keep the camera out of the ground and the sea.
	const float MinZ = FMath::Max(SolidAtM(CamPosM.X, CamPosM.Y) + 3.f, SEA_LEVEL + 4.f);
	if (CamPosM.Z < MinZ)
	{
		CamPosM.Z += (MinZ - CamPosM.Z) * F::Damp(18.f, Dt);
		CamPosM.Z = FMath::Max(CamPosM.Z, (double)(MinZ - 2.5f));
	}
	bCamInit = true;

	// Look-at, with the aim shoulder offset
	const FVector FlatFwd = FVector(-Boom.X, -Boom.Y, 0.f).GetSafeNormal();
	LookAtM = FocusM + RightN * (AIM_SHOULDER * AimBlend) + FlatFwd * (30.f * AimBlend) + FVector(0, 0, AIM_RISE * AimBlend);
	FRotator Rot = (LookAtM - CamPosM).Rotation();

	// Horizon lean in a carve (subtle).
	const float TurnT = bGrounded ? 0.f : Flight.GetTurnT();
	CamLean += ((AimBlend > 0.5f ? 0.f : -TurnT * 0.20f) - CamLean) * F::Damp(3.4f, Dt);
	// Web: positive lean rolls the camera counter-clockwise; in a left turn (TurnT>0) it rolls clockwise.
	Rot.Roll = -FMath::RadiansToDegrees(CamLean);

	// FOV: the web values are vertical; UE wants horizontal.
	const float VTarget = FOV_BASE + FOV_SPEED_GAIN * SpeedT * SpeedT + FOV_TURBO * Flight.GetTurboT() * (bGrounded ? 0.f : 1.f) - AIM_FOV * AimBlend;
	FovV += (VTarget - FovV) * F::Damp(FOV_LAMBDA, Dt);
	const float HFov = 2.f * FMath::RadiansToDegrees(FMath::Atan(FMath::Tan(FMath::DegreesToRadians(FovV) * 0.5f) * ViewAspect()));

	Camera->SetWorldLocationAndRotation(CamPosM * M, Rot);
	Camera->SetFieldOfView(FMath::Clamp(HFov, 40.f, 150.f));
}

float ADragonPawn::GetCompassDegrees() const
{
	// Bearing of where the camera looks; north is UE -Y (web -z).
	const float H = CamYaw + PI;
	float Deg = FMath::RadiansToDegrees(FMath::Atan2(FMath::Sin(H), -FMath::Cos(H)));
	if (Deg < 0.f) Deg += 360.f;
	return Deg;
}

// ===========================================================================
// Fire (plasma.js)
// ===========================================================================
FVector ADragonPawn::AimPointM() const
{
	// March the screen-centre ray over terrain and sea, 20 m to 1500 m.
	const FVector Dir = (LookAtM - CamPosM).GetSafeNormal();
	float T = 20.f;
	while (T < 1500.f)
	{
		const FVector P = CamPosM + Dir * T;
		if (P.Z <= FMath::Max(SolidAtM(P.X, P.Y), SEA_LEVEL)) return P;
		T += T < 200.f ? 4.f : 12.f;
	}
	return CamPosM + Dir * 1500.f;
}

void ADragonPawn::TickFire(float Dt, const FFrameInput& In)
{
	FireCooldown = FMath::Max(0.f, FireCooldown - Dt);
	if (Shots < MAX_SHOTS)
	{
		RechargeT += Dt;
		if (RechargeT >= RECHARGE_TIME) { RechargeT = 0.f; ++Shots; }
	}
	else RechargeT = 0.f;

	if (!(In.bFirePressed || (In.bFire && FireCooldown <= 0.f))) return;
	if (FireCooldown > 0.f || Shots <= 0) return;

	const float H = bGrounded ? WalkYaw : Flight.Heading;
	const float Path = bGrounded ? 0.f : Flight.PathAngle;
	const FVector Fwd = F::Forward(H);
	const FVector Muzzle = PosM + FVector(0, 0, 1.6f * DragonScale) + Fwd * (5.2f * DragonScale);
	FVector Dir;
	if (AimBlend > 0.5f) Dir = (AimPointM() - Muzzle).GetSafeNormal();
	else Dir = (Fwd * FMath::Cos(Path) + FVector(0, 0, FMath::Sin(Path))).GetSafeNormal();
	const FVector Carry = bGrounded ? FVector::ZeroVector : Flight.GetVelocity();
	const float Speed = MUZZLE_SPEED + FMath::Max(0.f, (float)FVector::DotProduct(Carry, Dir));

	if (UWorld* World = GetWorld())
	{
		FActorSpawnParameters SP;
		SP.Owner = this;
		SP.Instigator = this;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		UClass* Cls = PlasmaClass ? PlasmaClass.Get() : ADragonPlasma::StaticClass();
		if (ADragonPlasma* P = World->SpawnActor<ADragonPlasma>(Cls, Muzzle * M, Dir.Rotation(), SP))
		{
			P->Launch(Dir * Speed * M);
		}
	}
	--Shots;
	FireCooldown = FIRE_COOLDOWN;
}

// ===========================================================================
// Transform
// ===========================================================================
void ADragonPawn::ApplyTransform()
{
	const float H = bGrounded ? WalkYaw : Flight.Heading;
	SetActorLocationAndRotation(PosM * M, FRotator(0.f, F::HeadingToYaw(H), 0.f));
	float PitchDeg, RollDeg;
	if (bGrounded)
	{
		PitchDeg = FMath::RadiansToDegrees(GroundPitch);
		RollDeg = FMath::RadiansToDegrees(GroundRoll);
	}
	else
	{
		PitchDeg = FMath::RadiansToDegrees(Flight.CurrentPitch);
		// Web +roll = LEFT wing down; UE +roll = right wing down.
		RollDeg = -FMath::RadiansToDegrees(Flight.CurrentRoll);
	}
	Attitude->SetRelativeRotation(FRotator(PitchDeg, 0.f, RollDeg));
}

FString ADragonPawn::GetModeName() const
{
	if (bGrounded) return bFlaring ? TEXT("Landing") : (FMath::Abs(WalkSpeed) > 9.f ? TEXT("Galloping") : TEXT("Walking"));
	switch (Flight.Mode)
	{
	case EDragonFlightMode::Drop: return TEXT("Drop");
	case EDragonFlightMode::Zoom: return TEXT("Zoom climb");
	case EDragonFlightMode::Stall: return TEXT("Stall");
	case EDragonFlightMode::Dive: return TEXT("Dive");
	case EDragonFlightMode::Recover: return TEXT("Pull-up");
	default: break;
	}
	if (Flight.bTurbo) return TEXT("Turbo");
	if (Flight.bBursting) return TEXT("Flat out");
	if (Flight.ActiveSpeed < 3.f) return TEXT("Hover");
	return TEXT("Flying");
}

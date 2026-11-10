#include "DragonPawn.h"
#include "DragonFlight.h"
#include "DragonWorldSubsystem.h"
#include "DragonStandInComponent.h"
#include "DragonRigComponent.h"
#include "DragonWebRigComponent.h"
#include "DragonPlasma.h"
#include "DragonGameMode.h"
#include "Camera/CameraComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "DragonWindAudio.h"
#include "DragonSfx.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/GameInstance.h"
#include "WaterBodyActor.h"
#include "DragonFX.h"
#include "DragonWake.h"
#include "DragonSea.h"
#include "LandscapeProxy.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "InputCoreTypes.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"

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
	constexpr float G = 9.81f, GROUND_MU = 0.95f;
	// The body on its legs (metres at DragonScale 1): hips 1.25 m up, legs that reach 1.33 m
	// so his weight squats them 8 cm; a full crouch is 85 cm of give. Damping ratio ~0.55: a
	// landing settles in one dip and a half, as a big animal's does.
	constexpr float HIP_H = 1.25f, REST_COMP = 0.08f, LEG_L0 = HIP_H + REST_COMP, COMP_MAX = 0.85f, LEG_ZETA = 0.55f;
	constexpr float BUOYANCY = 2.5f;      // floats high: back, neck and head out of the water
	constexpr float HALF_SPAN = 7.4f;     // wingtip from the body's centre line

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
	WebRig = CreateDefaultSubobject<UDragonWebRigComponent>(TEXT("WebRig"));

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Feet);
	Camera->SetUsingAbsoluteLocation(true);
	Camera->SetUsingAbsoluteRotation(true);
	Camera->bConstrainAspectRatio = false;
	Camera->PostProcessBlendWeight = 0.f;

	WindAudio = CreateDefaultSubobject<UDragonWindAudio>(TEXT("WindAudio"));
	WindAudio->SetupAttachment(Feet);
	Ambience = CreateDefaultSubobject<UDragonAmbience>(TEXT("Ambience"));
	Ambience->SetupAttachment(Feet);

	PlasmaClass = ADragonPlasma::StaticClass();
}

void ADragonPawn::BeginPlay()
{
	Super::BeginPlay();
	PosM = GetActorLocation() / M;
	PrevPosM = PosM;
	SetActorScale3D(FVector(DragonScale));
	// A skeletal mesh with no skeleton asserts in UPoseableMeshComponent::AllocateTransformData,
	// so anything without one is refused and the stand-in is used instead.
	auto Usable = [](const USkeletalMesh* A) { return A && A->GetSkeleton() != nullptr; };
	if (DragonMesh->GetSkinnedAsset() && !DragonMesh->GetSkinnedAsset()->GetSkeleton())
	{
		UE_LOG(LogDragon, Warning, TEXT("Dragon mesh %s has no skeleton; ignoring it."), *DragonMesh->GetSkinnedAsset()->GetName());
		DragonMesh->SetSkinnedAssetAndUpdate(nullptr);
	}
	if (!DragonMesh->GetSkinnedAsset())
	{
		USkeletalMesh* Asset = nullptr;
		auto Try = [&](USkeletalMesh* A, const TCHAR* What)
		{
			if (Asset || !A) return;
			if (Usable(A)) Asset = A;
			else UE_LOG(LogDragon, Warning, TEXT("Dragon mesh %s (%s) has no skeleton; skipping it."), *A->GetPathName(), What);
		};
		Try(DragonAsset.IsNull() ? nullptr : DragonAsset.LoadSynchronous(), TEXT("DragonAsset"));
		// The Night Fury by Fulonimation Studios (https://ko-fi.com/fulonimationstudios) first; -OldDragon for the web model.
		if (!FParse::Param(FCommandLine::Get(), TEXT("OldDragon")))
			Try(LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/DragonFulo/SK_DragonFulo/SkeletalMeshes/SK_DragonFulo.SK_DragonFulo")), TEXT("Fulonimation Studios Night Fury"));
		Try(LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Dragon/SK_Dragon/SkeletalMeshes/SK_Dragon.SK_Dragon")), TEXT("Interchange import"));
		Try(LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Dragon/SK_Dragon.SK_Dragon")), TEXT("legacy path"));
		if (Asset) DragonMesh->SetSkinnedAssetAndUpdate(Asset);
		else UE_LOG(LogDragon, Log, TEXT("No usable dragon skeletal mesh; using the stand-in."));
	}
	DragonMesh->SetRelativeRotation(FRotator(0.f, DragonMeshYaw, 0.f));
	const bool bHasMesh = DragonMesh->GetSkinnedAsset() != nullptr;
	DragonMesh->SetVisibility(bHasMesh);
	// The hero is always on screen: keep full detail (and LOD0's materials) at chase distance.
	DragonMesh->SetForcedLOD(1);
	StandIn->SetStandInVisible(!bHasMesh);
	if (bHasMesh)
	{
		// The web game's own dragon gets the web game's own animation; anything
		// else gets the generic, data-driven rig.
		if (UDragonWebRigComponent::IsWebSkeleton(DragonMesh->GetSkinnedAsset()) && WebRig->Bind(DragonMesh))
		{
			DragonMesh->SetRelativeTransform(WebRig->GetMeshRelativeTransform() * FTransform(FRotator(0.f, DragonMeshYaw, 0.f)));
			// Wing tips beat well outside the bind-pose bounds.
			DragonMesh->SetBoundsScale(2.f);
			if (WebRig->IsFulo()) WebRig->SetFinStyle(UDragonWebRigComponent::SavedFinStyle());
		}
		else
		{
			Rig->Bind(DragonMesh);
		}
	}

	// The four legs: hips 1.6 m fore and aft of the centre of mass, 0.75 m either side.
	Legs[0].Hx = 1.6f; Legs[0].Hy = -0.75f;
	Legs[1].Hx = 1.6f; Legs[1].Hy = 0.75f;
	Legs[2].Hx = -1.6f; Legs[2].Hy = -0.75f;
	Legs[3].Hx = -1.6f; Legs[3].Hy = 0.75f;
	{ TActorIterator<ALandscapeProxy> It(GetWorld()); if (It) Landscape = *It; }
	bTelemetry = FParse::Param(FCommandLine::Get(), TEXT("DragonTelemetry"));

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

	// Keyboard: everything under the left hand around WASD, so the right hand stays on the mouse
	// (look, LMB fire, RMB aim, wheel zoom, middle button recentre). The web game's right-hand
	// keys (J K L ; O P I U) still work as alternates.
	const bool W = AnyDown({ EKeys::W }), S = AnyDown({ EKeys::S }), A = AnyDown({ EKeys::A }), D = AnyDown({ EKeys::D });
	In.Move = (W ? 1.f : 0.f) - (S ? 1.f : 0.f);
	In.Turn = (A ? 1.f : 0.f) - (D ? 1.f : 0.f); // + left
	In.WalkFwd = In.Move;
	In.WalkSide = (D ? 1.f : 0.f) - (A ? 1.f : 0.f);
	In.Vertical = (AnyDown({ EKeys::SpaceBar }) ? 1.f : 0.f) - (AnyDown({ EKeys::LeftControl, EKeys::C, EKeys::K }) ? 1.f : 0.f);
	In.bUpPressed = AnyPressed({ EKeys::SpaceBar });
	In.bSprint = AnyDown({ EKeys::LeftShift, EKeys::RightShift });
	In.bBurst = AnyDown({ EKeys::F, EKeys::J, EKeys::B, EKeys::Gamepad_FaceButton_Bottom });
	In.Strafe = (AnyDown({ EKeys::X, EKeys::P, EKeys::Gamepad_FaceButton_Right }) ? 1.f : 0.f) - (AnyDown({ EKeys::Z, EKeys::O, EKeys::Gamepad_FaceButton_Left }) ? 1.f : 0.f);
	const bool KL = AnyDown({ EKeys::Q, EKeys::L, EKeys::Gamepad_LeftShoulder });
	const bool KR = AnyDown({ EKeys::E, EKeys::Semicolon, EKeys::Gamepad_RightShoulder });
	In.Knife = KR ? -1 : KL ? 1 : 0;
	In.bLand = AnyDown({ EKeys::I, EKeys::R });
	In.bFire = AnyDown({ EKeys::One, EKeys::U, EKeys::LeftMouseButton, EKeys::Gamepad_RightTrigger });
	In.bFirePressed = AnyPressed({ EKeys::One, EKeys::U, EKeys::LeftMouseButton });
	In.bAim = AnyDown({ EKeys::RightMouseButton, EKeys::Two });
	In.bAlignDragon = AnyPressed({ EKeys::G, EKeys::H, EKeys::Gamepad_DPad_Up });
	In.bRecenter = AnyPressed({ EKeys::V, EKeys::MiddleMouseButton, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_RightThumbstick });

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
	if (bPadClimbInvert) In.bFire = AnyDown({ EKeys::One, EKeys::U, EKeys::LeftMouseButton });
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
	// The weather/time panel has the mouse: no looking, no firing with the buttons.
	const ADragonPlayerController* DPC = Cast<ADragonPlayerController>(PC);
	if (DPC && DPC->bPanelOpen)
	{
		MX = MY = 0.f;
		In.bFire = AnyDown({ EKeys::One, EKeys::U, EKeys::Gamepad_RightTrigger });
		In.bFirePressed = AnyPressed({ EKeys::One, EKeys::U });
		In.bAim = AnyDown({ EKeys::Two });
	}
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
	if (Script.bOn)
	{
		// Scripted tests (the photo director): these stand in for the keys.
		In.Move = Script.Move; In.Vertical = Script.Vertical; In.Turn = Script.Turn;
		In.WalkFwd = Script.WalkFwd; In.WalkSide = Script.WalkSide; In.Knife = Script.Knife;
		In.bLand = Script.bLand; In.bSprint = Script.bSprint; In.bUpPressed = Script.bUpPressed;
		Script.bUpPressed = false;
	}
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
	if (Script.bOn) ScriptTime += Dt;
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
	if (WebRig && WebRig->IsBound())
	{
		FDragonWebRigInput WI;
		WI.FS = FS;
		WI.bBoost = Flight.bBursting;
		WI.bGrounded = bGrounded && !bApproach;   // flying in, the wings are still working
		WI.bFlaring = bFlaring;
		WI.GroundSpeed = FMath::Abs(WalkSpeed) / FMath::Max(DragonScale, 0.01f);
		WI.MaxGroundSpeed = RUN_SPEED / FMath::Max(DragonScale, 0.01f);
		// aim.js setTarget: the head turns toward the crosshair's point, off his heading and flight path.
		WI.bAimActive = In.bAim && !bGrounded;
		if (WI.bAimActive)
		{
			const FVector D = (AimPointM() - (PosM + FVector(0, 0, 1.6f * DragonScale))).GetSafeNormal();
			WI.AimYaw = FMath::Clamp(F::AngleDelta(Flight.Heading, FMath::Atan2(D.X, D.Y)), -1.20f, 1.20f);
			WI.AimPitch = FMath::Clamp(FMath::Asin(FMath::Clamp(D.Z, -1.0, 1.0)) - Flight.PathAngle, -0.78f, 0.78f);
		}
		WI.GroundAtM = [this](float X, float Y) { return SolidAtM(X, Y); };
		Heave = WebRig->Animate(Dt, WI);
	}
	else if (Rig && Rig->IsBound()) Heave = Rig->Animate(Dt, FS, bGrounded, WalkSpeed, bFlaring);
	MeshPivot->SetRelativeLocation(FVector(0, 0, (-BODY_CENTRE + Heave) * M));

	TickCamera(Dt, In);
	WindAudio->SetFlight(bGrounded ? 0.f : Flight.GetSpeedRatio(), bGrounded ? 0.f : Flight.GetTurboT(), bGrounded);
	if (bGrounded && Ambience && !bSwimming) Ambience->SetSkim(0.f);
	// Wingbeats: a push of air at the start of each downstroke, loud in a hover or a hard
	// climb, gone in a glide.
	if (WebRig && (!bGrounded || bApproach))
	{
		const float Ph = WebRig->GetWingPhase01();
		if (Ph < LastWingPhase - 0.5f)
		{
			const float Strength = FMath::Clamp((WebRig->GetWingAmp() - 0.12f) / 0.5f, 0.f, 1.f) * (1.f - WebRig->GetGlide());
			const float Beat = FMath::Max(0.2f, WebRig->GetDownstroke() * 0.9f);
			DragonSfx::Wingbeat(this, GetActorLocation(), Strength, Beat);
		}
		LastWingPhase = Ph;
	}
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
	TickWaterFlight(Dt);
	if (Script.bOn || bTelemetry)
	{
		TelemetryT += Dt;
		if (TelemetryT >= 0.1f)
		{
			TelemetryT = 0.f;
			UE_LOG(LogDragon, Display, TEXT("[Fly] z=%.2f climb=%.2f speed=%.1f roll=%.1f knife=%.2f charge=%.2f rolldir=%d floor=%d mode=%d"),
				PosM.Z, Flight.ClimbVel, Flight.ActiveSpeed, FMath::RadiansToDegrees(Flight.CurrentRoll), Flight.KnifeAmount, Flight.KnifeCharge, Flight.RollDir, bFloorContact ? 1 : 0, (int32)Flight.Mode);
		}
	}

	const float Ground = FMath::Max(SolidAtM(PosM.X, PosM.Y), SEA_LEVEL);
	Agl = PosM.Z - Ground;
	CheckLanding(Dt, In);
}

// --- The flight floor (main.js 2476-2612) -----------------------------------
void ADragonPawn::FlightFloor(float Dt)
{
	const float S = DragonScale;
	const FVector Fwd = F::Forward(Flight.Heading);
	float Rock = SolidAtM(PosM.X, PosM.Y);
	const float Reach = FMath::Abs(Flight.ActiveSpeed) * 0.25f;
	for (int32 i = 1; i <= 3; ++i)
	{
		const FVector P = PosM + Fwd * (Reach * i / 3.f);
		Rock = FMath::Max(Rock, SolidAtM(P.X, P.Y));
	}
	// Over land the wings keep clear of the ground: more room the faster he goes.
	const float Clear = FMath::Lerp(2.2f, 6.f, Smooth(FMath::Abs(Flight.ActiveSpeed), 12.f, 40.f)) * S;
	// Over the sea he may come right down: feet a hand above the water, or in a knife edge low
	// enough for the lower wingtip to cut the surface about half a metre deep (never so deep it
	// would catch and cartwheel him).
	const float TipDrop = FMath::Sin(FMath::Min(FMath::Abs(Flight.CurrentRoll), HALF_PI)) * HALF_SPAN * S - BODY_CENTRE * S;
	// (the surface where he is, waves and all)
	// A plunge carries him under for a moment: the floor dips and comes back up.
	float Dip = 0.f;
	if (PlungeT > 0.f)
	{
		PlungeT += Dt;
		const float U = PlungeT / 0.75f;
		Dip = U < 1.f ? PlungeDepth * FMath::Sin(U * PI) : 0.f;
		if (U >= 1.f) PlungeT = 0.f;
	}
	const float WaterFloor = SeaAtM(PosM.X, PosM.Y) + FMath::Max(-0.1f * S, TipDrop - 0.55f * S) - Dip;
	const float Floor = FMath::Max(Rock + Clear, WaterFloor);
	if (PosM.Z >= Floor) { bFloorContact = false; return; }
	if (WaterFloor >= Rock + Clear)
	{
		// The sea is a surface, not a wall: struck hard it hurts and throws up a wall of water;
		// brushed, it holds him up and drags.
		const FVector V = Flight.GetVelocity();
		if (!bFloorContact && V.Z < -7.f && PlungeT <= 0.f)
		{
			// A dive into the sea is a plunge, like a gannet's: a burst of water, a moment
			// under, and out again still flying. Water slows him a little; it never stops him.
			const float Str = FMath::Clamp(-V.Z / 25.f, 0.3f, 1.f);
			DragonSfx::Splash(this, PosM * M, Str);
			if (ADragonWake* Wk = ADragonWake::Get(this)) Wk->Disturb(PosM * M, 700.f * S, -1.1f * Str, 8.f * Str);
			WaterSpray(PosM, FVector(V.X, V.Y, 0.f) * 0.25f, 40, 1.4f + 2.0f * Str);
			Shake = FMath::Max(Shake, 0.35f * Str);
			PlungeT = 1e-3f;
			PlungeDepth = FMath::Lerp(0.8f, 3.2f, Str) * S;
			Flight.Plunge(FMath::Lerp(0.9f, 0.75f, Str));
		}
		bFloorContact = true;
		PosM.Z = FMath::Max(PosM.Z + (Floor - PosM.Z) * F::Damp(14.f, Dt), (double)(Floor - 0.3f * S));
		Flight.ClimbVel = FMath::Max(Flight.ClimbVel, 0.f);
		return;
	}
	// Slow and just under the floor (lifting off, hovering low): ease up to it, no jolt.
	if (FMath::Abs(Flight.ActiveSpeed) < 15.f && Floor - PosM.Z < 2.5f * S)
	{
		bFloorContact = true;
		PosM.Z += (Floor - PosM.Z) * F::Damp(6.f, Dt);
		Flight.ClimbVel = FMath::Max(Flight.ClimbVel, 0.f);
		return;
	}

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
	if (bAirborne && bGrounded) { bGrounded = false; bFlaring = false; bApproach = false; bSwimming = false; Flight.SetHeading(WalkYaw); }
	CamVel = FVector::ZeroVector;
	FocusLag = FVector::ZeroVector;
	bCamInit = false;
	ApplyTransform();
}

void ADragonPawn::PhotoPose(const FVector& NewPosM, float YawDeg)
{
	TeleportToM(NewPosM, true);
	bGrounded = false;
	bFlaring = false;
	bApproach = false;
	bSwimming = false;
	Flight.SetHeading(F::YawToHeading(YawDeg));
	CamYaw = Flight.Heading + PI;
	bCamInit = false;
	ApplyTransform();
}

void ADragonPawn::PhotoFire(const FVector& TargetM)
{
	const FVector Fwd = F::Forward(Flight.Heading);
	const FVector Muzzle = PosM + FVector(0, 0, 1.6f * DragonScale) + Fwd * (5.2f * DragonScale);
	const FVector Dir = (TargetM - Muzzle).GetSafeNormal();
	const float Speed = MUZZLE_SPEED + FMath::Max(0.f, (float)FVector::DotProduct(Flight.GetVelocity(), Dir));
	if (UWorld* World = GetWorld())
	{
		FActorSpawnParameters SP;
		SP.Owner = this;
		SP.Instigator = this;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		UClass* Cls = PlasmaClass ? PlasmaClass.Get() : ADragonPlasma::StaticClass();
		if (ADragonPlasma* P = World->SpawnActor<ADragonPlasma>(Cls, Muzzle * M, Dir.Rotation(), SP)) P->Launch(Dir * Speed * M);
	}
}

void ADragonPawn::PhotoGround(const FVector& NewPosM, float YawDeg)
{
	TeleportToM(NewPosM, false);
	bGrounded = true; bApproach = false; bFlaring = false; bSwimming = false;
	WalkYaw = F::YawToHeading(YawDeg);
	CamYaw = WalkYaw + PI;
	GroundVel = FVector::ZeroVector;
	GroundPitch = GroundRoll = GroundPitchVel = GroundRollVel = 0.f;
	PosM.Z = SolidAtM(PosM.X, PosM.Y);
	if (WebRig && WebRig->IsBound()) WebRig->SnapFold(1.f);
	bCamInit = false;
	ApplyTransform();
}

void ADragonPawn::PhotoSpeed(float MS)
{
	// a fresh start for the manoeuvre meters too
	Flight.KnifeCharge = F::KNIFE_HOLD; Flight.KnifeAmount = 0.f; Flight.ClimbVel = 0.f; Flight.SetMode(EDragonFlightMode::Level);
	Flight.Airspeed = Flight.ActiveSpeed = MS;
	Flight.CurrentForwardSpeed = FMath::Max(MS, 1.f);
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
	const float AglLand = PosM.Z - GroundY;
	const float AglSea = PosM.Z - SEA_LEVEL;
	const bool bSlow = Flight.GetSpeedRatio() < LAND_SPEED_T;
	bool bTooSteep = false;
	if (bOverLand && AglLand < LAND_AGL)
	{
		float P, R, Steep;
		GroundAttitude(PosM.X, PosM.Y, GroundY, P, R, Steep);
		bTooSteep = Steep > LAND_SLOPE_MAX;
	}
	// Over water he can come down to swim, as long as he is slow and low.
	const bool bCanLand = bSlow && ((bOverLand && AglLand < LAND_AGL && !bTooSteep) || (!bOverLand && AglSea < 14.f));
	const bool bSinking = Flight.ClimbVel < -3.f && bOverLand;

	if (bCanLand && (In.bLand || bSinking))
	{
		LandHold += In.bLand ? Dt : 0.6f * Dt;
		if (LandHold > LAND_HOLD) { BeginApproach(); return; }
	}
	else if (!In.bLand)
	{
		LandHold = FMath::Max(0.f, LandHold - 2.f * Dt);
	}

	// Prompts name the failing condition and the key that fixes it.
	if (bCanLand) Prompt = bOverLand ? TEXT("Hold R or Ctrl to land") : TEXT("Hold R to come down on the water");
	else if (In.bLand || In.Vertical < -0.5f)
	{
		if (bOverLand && !bSlow && AglLand < LAND_AGL) Prompt = TEXT("Too fast to land — let go of W");
		else if (bTooSteep) Prompt = TEXT("Too steep to stand");
		else if (bOverLand && AglLand < 260.f) Prompt = TEXT("Too high to land — hold Ctrl to drop");
		else if (!bOverLand && AglSea < 140.f && !bSlow) Prompt = TEXT("Too fast to come down — let go of W");
	}
	if (Flight.EdgeT > 0.45f) Prompt = TEXT("He will not go further. The chart ends here.");
	else if (Flight.EdgeT > 0.02f) Prompt = TEXT("The last of the chart. He is drifting back.");
}

// The flight model hands over to the body: wings still working, legs reaching for the ground.
// From here on the dragon is a mass on four spring-damper legs; the wings hold him up and
// brake him until his feet take the weight.
void ADragonPawn::BeginApproach()
{
	bGrounded = true;
	bApproach = true;
	bFlaring = false;
	bSwimming = false;
	WalkYaw = Flight.Heading;
	Flight.ClearRoll();
	Flight.Snared = 0.f;
	LandHold = 0.f;
	GroundVel = Flight.GetVelocity();          // momentum carries over: speed, sink, slip
	GroundPitch = Flight.CurrentPitch;
	GroundRoll = -Flight.CurrentRoll;
	GroundPitchVel = GroundRollVel = 0.f;
	YawRateG = 0.f;
	AirTime = 0.f;
	for (FLeg& L : Legs) { L.bContact = false; L.Comp = 0.f; }
}

void ADragonPawn::TouchDown(float Vz)
{
	bApproach = false;
	const float Hard = FMath::Clamp((-Vz - 1.f) / 9.f, 0.f, 1.f);
	if (WebRig && WebRig->IsBound()) WebRig->BeginFold();
	DragonSfx::Thud(this, PosM * M, 0.25f + 0.85f * Hard);
	Shake = FMath::Max(Shake, 0.25f + Hard * 1.2f);
	// A crash still hurts (closing speed past the safe limit), however it is made.
	Impact(FVector(GroundVel.X, GroundVel.Y, Vz), FVector::UpVector, false);
	if (ADragonFX* FX = ADragonFX::Get(this))
	{
		// Dust (or powder snow) kicked out from under the feet.
		const bool bSnow = PosM.Z > 350.f;
		const int32 N = 6 + FMath::RoundToInt(10.f * Hard);
		for (int32 i = 0; i < N; ++i)
		{
			FDragonPuff P;
			P.Kind = EDragonPuff::Smoke;
			const float A = FMath::FRand() * 2.f * PI;
			const FVector D(FMath::Cos(A), FMath::Sin(A), 0.f);
			P.Pos = PosM * M + D * (150.f + 200.f * FMath::FRand()) * DragonScale + FVector(0, 0, 40.f);
			P.Vel = D * (300.f + 700.f * Hard) * (0.5f + FMath::FRand()) + FVector(0, 0, 60.f + 120.f * FMath::FRand());
			P.Drag = 2.2f; P.Buoyancy = 25.f;
			P.Life = 1.6f + 1.4f * FMath::FRand();
			P.Size0 = 120.f; P.Size1 = (350.f + 500.f * Hard) * (0.7f + 0.6f * FMath::FRand());
			P.Colour0 = bSnow ? FLinearColor(0.9f, 0.92f, 0.95f) : FLinearColor(0.42f, 0.38f, 0.30f);
			P.Colour1 = bSnow ? FLinearColor(0.86f, 0.88f, 0.92f) : FLinearColor(0.48f, 0.45f, 0.38f);
			P.Intensity = (0.10f + 0.25f * Hard) * (bSnow ? 1.6f : 1.f); P.FadeIn = 0.05f; P.Spin = FMath::FRand() - 0.5f;
			FX->Spawn(P);
		}
	}
}

void ADragonPawn::TakeOff(bool bLeap)
{
	const FVector Fw = F::Forward(WalkYaw);
	const float Ahead = FMath::Max(0.f, (float)FVector::DotProduct(FVector(GroundVel.X, GroundVel.Y, 0.f), Fw));
	const bool bFromWater = bSwimming;
	bGrounded = false;
	bApproach = false;
	bFlaring = false;
	bSwimming = false;
	Flight.SetHeading(WalkYaw);
	Flight.Airspeed = Flight.ActiveSpeed = Ahead;
	// A leap: legs drive him up (or wings haul him out of the water); falling off an edge he
	// just opens his wings with the speed he has.
	Flight.ClimbVel = bLeap ? FMath::Max((float)GroundVel.Z, 0.f) + (bFromWater ? 5.f : 7.5f) : (float)GroundVel.Z;
	PosM.Z += 0.05f;
	if (WebRig && WebRig->IsBound()) WebRig->BeginUnfold();
	if (bLeap)
	{
		if (bFromWater)
		{
			DragonSfx::Splash(this, PosM * M, 0.8f);
			if (ADragonWake* Wk = ADragonWake::Get(this)) Wk->Disturb(PosM * M, 450.f * DragonScale, -0.3f, 3.f);
			WaterSpray(PosM + FVector(0, 0, 0.5f), Fw * 2.f, 22, 1.2f);
		}
		else DragonSfx::Thud(this, PosM * M, 0.35f);
	}
}

void ADragonPawn::ProbeLegs()
{
	// One short trace per foot, from above the hip down past the reach of the leg: the ground
	// (landscape, rocks, roofs, the sea bed) and which way it faces.
	UWorld* World = GetWorld();
	const FVector Fw = F::Forward(WalkYaw), Ri = -F::Left(WalkYaw);
	const float S = DragonScale;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DragonLeg), true, this);
	FCollisionObjectQueryParams O;
	O.AddObjectTypesToQuery(ECC_WorldStatic);
	O.AddObjectTypesToQuery(ECC_WorldDynamic);
	const UDragonWorldSubsystem* WS = World && World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;
	for (FLeg& L : Legs)
	{
		const FVector Foot = PosM + Fw * (L.Hx * S) + Ri * (L.Hy * S);
		bool bHit = false;
		if (World)
		{
			TArray<FHitResult> Hits;
			const FVector From = FVector(Foot.X, Foot.Y, PosM.Z + 3.5f * S) * M, To = FVector(Foot.X, Foot.Y, PosM.Z - 9.f * S) * M;
			if (World->LineTraceMultiByObjectType(Hits, From, To, O, Q))
				for (const FHitResult& H : Hits)
				{
					const AActor* A = H.GetActor();
					if (A && (A->IsA<AWaterBody>() || A == this)) continue;
					L.GroundZ = H.ImpactPoint.Z / M;
					L.Normal = H.ImpactNormal;
					bHit = true;
					break;
				}
		}
		if (!bHit)
		{
			const double H0 = WS ? WS->HeightAt(Foot.X * M, Foot.Y * M) / M : -200.0;
			const double Hx = WS ? WS->HeightAt((Foot.X + 1.0) * M, Foot.Y * M) / M : H0;
			const double Hy = WS ? WS->HeightAt(Foot.X * M, (Foot.Y + 1.0) * M) / M : H0;
			L.GroundZ = (float)H0;
			L.Normal = FVector(H0 - Hx, H0 - Hy, 1.0).GetSafeNormal();
		}
		// A foot can't find ground far above the hip: anything that high is a wall, handled
		// by the body's collision, not something to stand on.
		L.GroundZ = FMath::Min(L.GroundZ, (float)PosM.Z + 1.6f * S);
		if (L.Normal.Z < 0.2f) L.Normal = FVector::UpVector;
	}
	// He levels himself as an animal does: on a slope the downhill legs reach further and the
	// uphill ones bend, so his back is tilted only half as much as the ground.
	const float Zf = 0.5f * (Legs[0].GroundZ + Legs[1].GroundZ), Zr = 0.5f * (Legs[2].GroundZ + Legs[3].GroundZ);
	const float Zl = 0.5f * (Legs[0].GroundZ + Legs[2].GroundZ), Zrt = 0.5f * (Legs[1].GroundZ + Legs[3].GroundZ);
	const float Pg = FMath::Atan2(Zf - Zr, 3.2f * S), Rg = -FMath::Atan2(Zrt - Zl, 1.5f * S);
	const float Pd = 0.5f * Pg, Rd = 0.5f * Rg;
	for (FLeg& L : Legs)
	{
		const float Want = -L.Hx * S * (FMath::Sin(Pg) - FMath::Sin(Pd)) + L.Hy * S * (FMath::Sin(Rg) - FMath::Sin(Rd));
		L.Reach = FMath::Clamp(Want, -0.45f * S, 0.45f * S);
	}
}

void ADragonPawn::StepBody(float h, const FVector& WantV, float Kacc, float AccMax)
{
	const float S = FMath::Max(DragonScale, 0.05f);
	const float K = G / (4.f * REST_COMP * S);           // leg spring, per unit mass: 4 legs hold him up at REST_COMP
	const float C = LEG_ZETA * 2.f * FMath::Sqrt(4.f * K) / 4.f;
	const float Ip = 2.6f * S * S, Ir = 0.5f * S * S;   // pitch and roll inertia per unit mass (m^2)
	const float SinP = FMath::Sin(GroundPitch), SinR = FMath::Sin(GroundRoll);

	FVector Fn = FVector::ZeroVector;   // sum of leg forces (along each foot's ground normal)
	float Ntot = 0.f, TauP = 0.f, TauR = 0.f;
	int32 Contacts = 0;
	for (FLeg& L : Legs)
	{
		const float HipZ = PosM.Z + HIP_H * S + L.Hx * S * SinP - L.Hy * S * SinR;
		const float Comp = (LEG_L0 * S + L.Reach) - (HipZ - L.GroundZ);
		L.Comp = Comp;
		L.bContact = Comp > 0.f;
		if (!L.bContact) continue;
		// how fast the hip is closing on the ground (rigid body, ground at rest)
		const float CompRate = -(float)(GroundVel.Z + L.Hx * S * GroundPitchVel * FMath::Cos(GroundPitch) - L.Hy * S * GroundRollVel * FMath::Cos(GroundRoll));
		float Fl = K * Comp + C * CompRate;
		if (Comp > COMP_MAX * S) Fl += K * 12.f * (Comp - COMP_MAX * S) + C * 3.f * FMath::Max(0.f, CompRate);   // fully crouched: bone on bone
		Fl = FMath::Max(0.f, Fl);
		Fn += L.Normal * Fl;
		Ntot += Fl * L.Normal.Z;
		TauP += Fl * L.Hx * S;
		TauR -= Fl * L.Hy * S;
		++Contacts;
	}

	FVector A(0.f, 0.f, -G);
	A += Fn;
	const FVector Vh(GroundVel.X, GroundVel.Y, 0.f);

	// Traction: the feet push him toward the speed he wants, and hold him on a slope, as far as
	// friction (mu x the weight on them) allows. Past that he slides.
	if (Ntot > 0.f)
	{
		FVector Des = (WantV - Vh) * Kacc;
		if (Des.Size() > AccMax) Des = Des.GetSafeNormal() * AccMax;
		const FVector FnH(Fn.X, Fn.Y, 0.f);
		FVector Fric = Des - FnH;
		const float Cap = GROUND_MU * Ntot;
		if (Fric.Size() > Cap) Fric = Fric.GetSafeNormal() * Cap;
		A += Fric;
	}

	// The sea: buoyancy on the part of him that is under, drag, and paddling.
	const float Belly = PosM.Z + 0.9f * S, Back = PosM.Z + 2.0f * S;
	Submerged = FMath::Clamp((SeaZ - Belly) / (Back - Belly), 0.f, 1.f);
	if (Submerged > 0.f)
	{
		// he rides the swell: buoyancy against the surface where it is, damping relative to its motion
		A.Z += G * BUOYANCY * Submerged;
		A -= FVector(GroundVel.X * 0.45f, GroundVel.Y * 0.45f, (GroundVel.Z - SeaVz) * 2.6f) * Submerged;
		A -= GroundVel * (0.06f * (float)GroundVel.Size() * Submerged);
		if (Ntot <= 0.f || Submerged > 0.4f)
		{
			FVector Paddle = (WantV - Vh) * 3.f;
			if (Paddle.Size() > 3.f) Paddle = Paddle.GetSafeNormal() * 3.f;
			A += Paddle * Submerged;
		}
	}

	// Flying in: the wings carry most of him and brake hard (backwinging), easing him down at a
	// sink rate that slows as the ground comes up. K or C comes down faster; W keeps a little way on.
	if (bApproach)
	{
		const float Hag = (float)PosM.Z - FMath::Max(Legs[0].GroundZ, FMath::Max(Legs[2].GroundZ, SeaZ));
		// drop in briskly, flare hard in the last few metres
		float SinkT = -FMath::Lerp(1.6f, 7.f, FMath::Clamp((Hag - 1.5f) / 9.f, 0.f, 1.f));
		if (ApproachDrop) SinkT = -8.f;
		const float Lift = FMath::Clamp(G + (SinkT - (float)GroundVel.Z) * 4.5f, 0.f, 1.9f * G);
		A.Z += Lift;
		A += (WantV * 0.25f - Vh) * 1.4f;
		// the body pitches up into the flare
		GroundPitchVel += ((0.32f - GroundPitch) * 26.f - GroundPitchVel * 7.f) * h;
		GroundRollVel += ((0.f - GroundRoll) * 26.f - GroundRollVel * 7.f) * h;
	}

	GroundVel += A * h;
	PosM += GroundVel * h;

	// Rigid-body attitude: the legs' torques, a little damping, and with no feet down he rights himself.
	GroundPitchVel += (TauP / Ip - GroundPitchVel * 1.5f) * h;
	GroundRollVel += (TauR / Ir - GroundRollVel * 1.5f) * h;
	if (Contacts == 0 && !bApproach)
	{
		GroundPitchVel += (-GroundPitch * 8.f - GroundPitchVel * 3.f) * h;
		GroundRollVel += (-GroundRoll * 8.f - GroundRollVel * 3.f) * h;
	}
	if (Submerged > 0.f)
	{
		GroundPitchVel += (-GroundPitch * 10.f - GroundPitchVel * 4.f) * Submerged * h;
		GroundRollVel += (-GroundRoll * 14.f - GroundRollVel * 5.f) * Submerged * h;
	}
	// An animal on a steep roof or bank keeps its body far flatter than the surface and takes the
	// rest up in its legs: at 51 degrees he stood on a longhouse roof like a lizard on a wall.
	GroundPitch = FMath::Clamp(GroundPitch + GroundPitchVel * h, -0.6f, 0.6f);
	GroundRoll = FMath::Clamp(GroundRoll + GroundRollVel * h, -0.6f, 0.6f);
	if (FMath::Abs(GroundPitch) >= 0.6f) GroundPitchVel = 0.f;
	if (FMath::Abs(GroundRoll) >= 0.6f) GroundRollVel = 0.f;
	LegContacts = Contacts;
	LegLoad = Ntot / G;
}

// --- On foot (and in the water): a mass on four spring-damper legs ------------
void ADragonPawn::TickGround(float Dt, const FFrameInput& In)
{
	const float S = FMath::Max(DragonScale, 0.05f);
	if (In.bUpPressed || In.bPadUpTakeoff)
	{
		// Mid-approach: go round. On the ground or in the water: leap (or haul) into the air.
		TakeOff(!bApproach);
		return;
	}
	ApproachDrop = bApproach && In.Vertical < -0.5f;

	const bool bRunning = In.bSprint || In.bPadRun;
	const float FwdIn = FMath::Clamp(In.WalkFwd, -1.f, 1.f);
	const float SideIn = FMath::Clamp(In.WalkSide, -1.f, 1.f);
	const float Mag = FMath::Min(1.f, FMath::Sqrt(FwdIn * FwdIn + SideIn * SideIn));
	const float CamFwd = CamYaw + PI;
	const FVector Wish = F::Forward(CamFwd) * FwdIn + F::Forward(CamFwd - HALF_PI) * SideIn;
	const float GearT = FMath::Clamp((FMath::Abs(WalkSpeed) - WALK_SPEED) / (RUN_SPEED - WALK_SPEED), 0.f, 1.f);
	const bool bInWater = Submerged > 0.35f && LegLoad < 0.5f;
	// He can only turn with feet on the ground, paddling, or with the wings in the air.
	const float Grip = bApproach ? 0.35f : bInWater ? 0.6f : FMath::Clamp(LegLoad, 0.f, 1.f);
	float Align = 1.f;
	bool bReversing = false;
	float WantYawRate = 0.f;
	if (Mag > 0.01f)
	{
		const float WishYaw = FMath::Atan2(Wish.X, Wish.Y);
		const float Off = F::AngleDelta(WalkYaw, WishYaw);
		if (FMath::Abs(Off) > REVERSE_CONE && WalkSpeed < 1.5f && !bInWater && !bApproach)
		{
			bReversing = true;
			const float Back = F::AngleDelta(WalkYaw, FMath::Atan2(-Wish.X, -Wish.Y));
			WantYawRate = FMath::Clamp(Back * 4.f, -WALK_TURN * 0.35f, WALK_TURN * 0.35f);
			Align = -FMath::Max(0.f, FMath::Cos(Back));
		}
		else
		{
			const float MaxRate = bInWater ? 1.2f : FMath::Lerp(WALK_TURN, RUN_TURN, GearT);
			WantYawRate = FMath::Clamp(Off * 5.f, -MaxRate, MaxRate);
			Align = FMath::Max(0.f, FMath::Cos(F::AngleDelta(WalkYaw, WishYaw)));
		}
	}
	// yaw has inertia too: it spins up and down
	YawRateG += (WantYawRate * Grip - YawRateG) * F::Damp(9.f, Dt);
	WalkYaw += YawRateG * Dt;

	const float Gear = bInWater ? (bRunning ? 4.2f : 2.4f) : bReversing ? WALK_SPEED * REVERSE_FRAC : (bRunning ? RUN_SPEED : WALK_SPEED);
	const FVector Fw = F::Forward(WalkYaw);
	const FVector WantV = Fw * (Mag * Align * Gear * S);
	// Walking pace picks up quickly; a gallop takes a few strides to build and to stop.
	const float Kacc = bRunning ? 2.2f : 3.6f;
	const float AccMax = (bRunning ? 6.5f : 4.5f) * S;

	// Walls and big rocks: the body (a sphere at the chest) can't pass through them. Low
	// things are the legs' business: they step up onto anything under ~1 m.
	const FVector Start = PosM;
	ProbeLegs();
	{
		const float NewSea = SeaAtM(PosM.X, PosM.Y);
		SeaVz = FMath::Lerp(SeaVz, (NewSea - SeaZ) / FMath::Max(Dt, 1e-3f), 0.5f);
		SeaZ = NewSea;
	}
	const float VzBefore = GroundVel.Z;
	const bool bWasApproach = bApproach;
	const bool bWasSwimming = bSwimming;
	const int32 NSub = FMath::Clamp(FMath::CeilToInt(Dt / (1.f / 240.f)), 1, 32);
	const float H = Dt / NSub;
	for (int32 i = 0; i < NSub; ++i)
	{
		StepBody(H, WantV, Kacc, AccMax);
		if (bApproach && LegContacts > 0) { TouchDown(VzBefore); }
	}

	// The body's collision, against everything but the ground itself.
	if (UWorld* World = GetWorld())
	{
		const FVector Move(PosM.X - Start.X, PosM.Y - Start.Y, 0.f);
		if (Move.SizeSquared() > 1e-6)
		{
			const float Rad = 0.95f * S;
			const FVector Chest = FVector(0, 0, 1.75f * S) + Fw * (1.2f * S);
			FCollisionQueryParams Q(SCENE_QUERY_STAT(DragonBody), false, this);
			if (Landscape.IsValid()) Q.AddIgnoredActor(Landscape.Get());
			FCollisionObjectQueryParams O;
			O.AddObjectTypesToQuery(ECC_WorldStatic);
			O.AddObjectTypesToQuery(ECC_WorldDynamic);
			FHitResult Hit;
			const FVector A0 = (Start + Chest) * M, A1 = (FVector(PosM.X, PosM.Y, Start.Z) + Chest) * M;
			if (World->SweepSingleByObjectType(Hit, A0, A1, FQuat::Identity, O, FCollisionShape::MakeSphere(Rad * M), Q)
				&& !(Hit.GetActor() && Hit.GetActor()->IsA<AWaterBody>()) && Hit.ImpactNormal.Z < 0.75f)
			{
				// Stop at the contact and slide along it; what was going into it is lost.
				const FVector Nh = FVector(Hit.ImpactNormal.X, Hit.ImpactNormal.Y, 0.f).GetSafeNormal();
				const FVector Allowed = Move * Hit.Time + (Move * (1.f - Hit.Time) - Nh * FVector::DotProduct(Move * (1.f - Hit.Time), Nh));
				PosM.X = Start.X + Allowed.X; PosM.Y = Start.Y + Allowed.Y;
				const float Into = FVector::DotProduct(FVector(GroundVel.X, GroundVel.Y, 0.f), Nh);
				if (Into < 0.f) { GroundVel.X -= Nh.X * Into; GroundVel.Y -= Nh.Y * Into; }
				if (Into < -6.f) { DragonSfx::Thud(this, Hit.ImpactPoint, FMath::Clamp(-Into / 15.f, 0.f, 1.f)); Shake = FMath::Max(Shake, 0.4f); }
			}
		}
		// The ground itself as a wall: a rise ahead too tall to step (a crag, a cut bank).
		const FVector Hv(GroundVel.X, GroundVel.Y, 0.f);
		const FVector Dir = Hv.GetSafeNormal();
		if (!Dir.IsNearlyZero() && LegContacts > 0)
		{
			const float Here = FMath::Max(Legs[0].GroundZ, Legs[1].GroundZ);
			const float Ahead = SolidAtM(PosM.X + Dir.X * 2.9f * S, PosM.Y + Dir.Y * 2.9f * S);
			if (Ahead - Here > STEP_MAX * S && Ahead > PosM.Z + STEP_MAX * S)
			{
				const float Into = FVector::DotProduct(Hv, Dir);
				GroundVel.X -= Dir.X * Into; GroundVel.Y -= Dir.Y * Into;
				PosM.X = Start.X; PosM.Y = Start.Y;
			}
		}
	}

	// In and out of the water.
	bSwimming = Submerged > 0.25f && LegLoad < 0.6f;
	if (bApproach && Submerged > 0.1f)
	{
		// Settled onto the water: wings fold, he swims.
		bApproach = false;
		if (WebRig && WebRig->IsBound()) WebRig->BeginFold();
		DragonSfx::Splash(this, PosM * M, 0.3f);
	}
	if (Submerged > 0.08f && !bWasSwimming && VzBefore < -1.5f && SinceSplash > 0.6f)
	{
		const float Str = FMath::Clamp(-VzBefore / 8.f, 0.15f, 1.f);
		DragonSfx::Splash(this, PosM * M, Str);
		if (ADragonWake* Wk = ADragonWake::Get(this)) Wk->Disturb((PosM + FVector(0, 0, 0.5f)) * M, 400.f * S, -0.35f * Str, 4.f * Str);
		WaterSpray(PosM + FVector(0, 0, 0.6f), FVector(GroundVel.X, GroundVel.Y, 0.f) * 0.3f, 10 + FMath::RoundToInt(20 * Str), 0.6f + Str);
		SinceSplash = 0.f;
		if (bWasApproach && WebRig && WebRig->IsBound()) WebRig->BeginFold();
		bApproach = false;
	}
	SinceSplash += Dt;
	if (bSwimming || (Submerged > 0.05f && FVector(GroundVel.X, GroundVel.Y, 0.f).Size() > 0.6f))
	{
		// He pushes a bow wave and leaves a wake; each stroke of the legs a ripple.
		WakeTimer -= Dt;
		const float Sp = FVector(GroundVel.X, GroundVel.Y, 0.f).Size();
		if (ADragonWake* Wk = ADragonWake::Get(this))
			Wk->Disturb((PosM + Fw * 2.2f * S) * M, 140.f * S, -(0.03f + 0.04f * Sp), 0.08f * Sp);
		if (WakeTimer <= 0.f && Sp > 0.5f)
		{
			WakeTimer = 0.55f;
			if (ADragonWake* Wk = ADragonWake::Get(this)) Wk->Disturb(PosM * M, 260.f * S, -0.12f, 0.3f);
		}
	}

	// Off an edge with nothing under the feet: he falls a moment, then opens his wings.
	if (LegContacts == 0 && !bApproach && Submerged < 0.05f)
	{
		AirTime += Dt;
		if (AirTime > 0.3f && GroundVel.Z < -2.5f) { TakeOff(false); return; }
	}
	else AirTime = 0.f;

	WalkSpeed = FVector::DotProduct(FVector(GroundVel.X, GroundVel.Y, 0.f), Fw);
	// While still flying in, the rig keeps flying: feed it the approach as a slow, sinking flight.
	if (bApproach)
	{
		Flight.Airspeed = Flight.ActiveSpeed = FVector(GroundVel.X, GroundVel.Y, 0.f).Size();
		Flight.ClimbVel = GroundVel.Z;
	}
	Agl = 0.f;
	Prompt = bApproach ? TEXT("Landing — Space to go round") : bSwimming ? TEXT("Swimming — Space to take off") : TEXT("");

	// Telemetry for the scripted tests.
	if (Script.bOn || bTelemetry)
	{
		TelemetryT += Dt;
		if (TelemetryT >= 0.1f)
		{
			TelemetryT = 0.f;
			UE_LOG(LogDragon, Display, TEXT("[Body] %s z=%.2f vz=%.2f vh=%.2f pitch=%.1f roll=%.1f legs=%d load=%.2f comp=%.2f/%.2f/%.2f/%.2f sub=%.2f"),
				bApproach ? TEXT("approach") : bSwimming ? TEXT("swim") : TEXT("ground"), PosM.Z, GroundVel.Z, FVector(GroundVel.X, GroundVel.Y, 0.f).Size(),
				FMath::RadiansToDegrees(GroundPitch), FMath::RadiansToDegrees(GroundRoll), LegContacts, LegLoad,
				Legs[0].Comp, Legs[1].Comp, Legs[2].Comp, Legs[3].Comp, Submerged);
		}
	}
}

// --- Over the water in flight: wingtips, feet and downwash ----------------------
void ADragonPawn::WaterSpray(const FVector& AtM, const FVector& VelM, int32 N, float Size)
{
	ADragonFX* FX = ADragonFX::Get(this);
	if (!FX) return;
	for (int32 i = 0; i < N; ++i)
	{
		FDragonPuff P;
		P.Kind = EDragonPuff::Spray;
		const float A = FMath::FRand() * 2.f * PI;
		const FVector Out(FMath::Cos(A), FMath::Sin(A), 0.f);
		P.Pos = AtM * M + Out * 40.f * Size;
		P.Vel = (VelM + Out * (1.5f + 2.5f * FMath::FRand()) * Size + FVector(0, 0, (2.5f + 4.5f * FMath::FRand()) * FMath::Sqrt(Size))) * M;
		P.Drag = 0.9f; P.Gravity = 980.f;
		P.Life = 0.5f + 0.6f * FMath::FRand();
		P.Size0 = 12.f * Size; P.Size1 = (45.f + 50.f * FMath::FRand()) * Size; P.Stretch = 0.010f;
		P.Colour0 = FLinearColor(0.95f, 0.97f, 1.f); P.Colour1 = FLinearColor(0.9f, 0.92f, 0.95f);
		P.Intensity = 0.42f; P.FadeIn = 0.02f;
		FX->Spawn(P);
		if ((i & 3) == 0)
		{
			// and a drift of fine mist that hangs where the spray was
			FDragonPuff Mi;
			Mi.Kind = EDragonPuff::Smoke;
			Mi.Pos = P.Pos + FVector(0, 0, 30.f);
			Mi.Vel = P.Vel * 0.25f;
			Mi.Drag = 2.f; Mi.Buoyancy = 10.f;
			Mi.Life = 1.2f + FMath::FRand();
			Mi.Size0 = 60.f * Size; Mi.Size1 = (220.f + 160.f * FMath::FRand()) * Size;
			Mi.Colour0 = FLinearColor(0.93f, 0.95f, 0.98f); Mi.Colour1 = FLinearColor(0.9f, 0.92f, 0.95f);
			Mi.Intensity = 0.14f; Mi.FadeIn = 0.1f; Mi.Spin = FMath::FRand() - 0.5f;
			FX->Spawn(Mi);
		}
	}
}

void ADragonPawn::TickWaterFlight(float Dt)
{
	const float S = DragonScale;
	float Skim = 0.f;
	SprayTimer -= Dt;
	ADragonWake* Wk = nullptr;
	const FVector Fw = F::Forward(Flight.Heading), Ri = -F::Left(Flight.Heading);
	const FVector V = Flight.GetVelocity();
	const float Speed = (float)V.Size();
	const FVector Centre = PosM + FVector(0, 0, BODY_CENTRE * S);
	if (Centre.Z - SEA_LEVEL > 40.f * S) { if (Ambience) Ambience->SetSkim(0.f); return; }
	if (SolidAtM(PosM.X, PosM.Y) > SEA_LEVEL - 0.3f) { if (Ambience) Ambience->SetSkim(0.f); return; }
	Wk = ADragonWake::Get(this);

	// The wingtips, from the body's roll (UE roll + = right wing down), a little behind the shoulders.
	const float R = -Flight.CurrentRoll;
	for (int32 Side = -1; Side <= 1; Side += 2)
	{
		const FVector Tip = Centre - Fw * (1.2f * S) + Ri * (Side * FMath::Cos(R) * HALF_SPAN * S) + FVector(0, 0, -Side * FMath::Sin(R) * HALF_SPAN * S);
		const float Depth = SeaAtM(Tip.X, Tip.Y) - (float)Tip.Z;
		if (Depth < -0.25f) continue;
		// It cuts the surface: a moving point pushing water aside (the V of a wake falls out of
		// the wave equation), a sheet of spray thrown up and back, and a drag on that wing.
		const float D = FMath::Clamp(Depth + 0.25f, 0.f, 0.9f);
		const float SpT = FMath::Clamp(Speed / 40.f, 0.2f, 1.6f);
		if (Wk) Wk->Disturb(Tip * M, (50.f + 70.f * D) * S, -(0.08f + 0.25f * D) * FMath::Min(SpT, 1.f), 3.f + 6.f * D);
		Skim = FMath::Max(Skim, FMath::Clamp(0.35f + D * SpT, 0.f, 1.f));
		if (SprayTimer <= 0.f)
		{
			const FVector Throw = -V * 0.12f + Ri * (Side * 2.5f) + FVector(0, 0, 3.5f * SpT);
			WaterSpray(FVector(Tip.X, Tip.Y, SeaAtM(Tip.X, Tip.Y) + 0.1f), Throw, 2 + FMath::RoundToInt(3 * D * SpT), 0.5f + 0.8f * D * SpT);
		}
		if (!bTipWet[(Side + 1) / 2]) { DragonSfx::Splash(this, Tip * M, 0.25f + 0.4f * D * SpT); bTipWet[(Side + 1) / 2] = true; }
		// Water grabs the wing: speed bleeds and he is pulled round toward it, a little.
		Flight.ScrapeSpeed(FMath::Clamp(Dt * 0.12f * D, 0.f, 0.05f));
		Flight.Heading += Side * 0.10f * D * Dt;
	}
	{
		// feet and tail trailing in the water
		const float SeaHere = SeaAtM(PosM.X, PosM.Y);
		const float FeetDepth = SeaHere - (float)PosM.Z;
		if (FeetDepth > -0.3f)
		{
			const float D = FMath::Clamp(FeetDepth + 0.3f, 0.f, 1.f);
			const float SpT = FMath::Clamp(Speed / 30.f, 0.2f, 1.6f);
			if (Wk) { Wk->Disturb((PosM - Fw * 0.8f * S + Ri * 0.6f * S) * M, 60.f * S, -(0.08f + 0.2f * D), 3.f); Wk->Disturb((PosM - Fw * 0.8f * S - Ri * 0.6f * S) * M, 60.f * S, -(0.08f + 0.2f * D), 3.f); }
			if (SprayTimer <= 0.f) WaterSpray(FVector(PosM.X, PosM.Y, SeaHere + 0.1f) - Fw * S, -V * 0.15f + FVector(0, 0, 2.5f), 3, 0.6f + 0.6f * D);
			Skim = FMath::Max(Skim, FMath::Clamp(0.3f + D, 0.f, 1.f));
			Flight.ScrapeSpeed(FMath::Clamp(Dt * 0.25f * D, 0.f, 0.06f));
			if (!bFeetWet) { DragonSfx::Splash(this, PosM * M, 0.3f + 0.4f * D); bFeetWet = true; }
		}
		else bFeetWet = false;
	}
	for (int32 i = 0; i < 2; ++i)
	{
		const FVector Tip = Centre - Fw * (1.2f * S) + Ri * ((i * 2 - 1) * FMath::Cos(R) * HALF_SPAN * S) + FVector(0, 0, -(i * 2 - 1) * FMath::Sin(R) * HALF_SPAN * S);
		if (SeaAtM(Tip.X, Tip.Y) - Tip.Z < -0.6f) bTipWet[i] = false;
	}

	// Downwash: hovering or slow and low, the wings blow a ring of ripples and a skirt of spray
	// off the sea, pulsing with each downstroke.
	const float Alt = (float)(Centre.Z - SEA_LEVEL);
	const float Wash = FMath::Square(FMath::Clamp(1.f - Alt / (18.f * S), 0.f, 1.f)) * (1.f - Smooth(Speed, 8.f, 30.f));
	if (Wash > 0.02f && Wk)
	{
		const float Amp = WebRig && WebRig->IsBound() ? FMath::Clamp(WebRig->GetWingAmp() / 0.6f, 0.3f, 1.3f) : 1.f;
		const FVector Under(PosM.X, PosM.Y, SEA_LEVEL);
		Wk->Disturb(Under * M, 550.f * S, -0.05f * Wash * Amp, 0.15f * Wash);
		const float Ph = WebRig && WebRig->IsBound() ? WebRig->GetWingPhase01() : 0.f;
		if (Ph < LastWashPhase - 0.5f)
		{
			Wk->Disturb(Under * M, 650.f * S, -0.22f * Wash * Amp, 0.8f * Wash);
			if (ADragonFX* FX = ADragonFX::Get(this))
				for (int32 k = 0; k < 8; ++k)
				{
					FDragonPuff P;
					P.Kind = EDragonPuff::Smoke;
					const float A = (k + FMath::FRand()) * PI / 4.f;
					const FVector D(FMath::Cos(A), FMath::Sin(A), 0.f);
					P.Pos = Under * M + D * 500.f * S + FVector(0, 0, 40.f);
					P.Vel = D * (700.f + 500.f * FMath::FRand()) * Wash + FVector(0, 0, 60.f);
					P.Drag = 1.6f; P.Buoyancy = 15.f;
					P.Life = 1.5f + FMath::FRand();
					P.Size0 = 150.f; P.Size1 = 500.f + 300.f * FMath::FRand();
					P.Colour0 = FLinearColor(0.92f, 0.94f, 0.97f); P.Colour1 = FLinearColor(0.88f, 0.9f, 0.94f);
					P.Intensity = 0.18f * Wash * Amp; P.FadeIn = 0.1f; P.Spin = FMath::FRand() - 0.5f;
					FX->Spawn(P);
				}
		}
		LastWashPhase = Ph;
		Skim = FMath::Max(Skim, Wash * 0.35f);
	}
	if (SprayTimer <= 0.f) SprayTimer = 0.035f;
	if (Ambience) Ambience->SetSkim(Skim);
}

float ADragonPawn::SeaAtM(float X, float Y) const
{
	if (!SeaActor.IsValid()) SeaActor = ADragonSea::Get(this);
	return SEA_LEVEL + (SeaActor.IsValid() ? SeaActor->SurfaceM(X, Y) : 0.f);
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
	// (on the ground the horizon levels quickly: a bank left over from the last turn tipped the view)
	CamLean += ((AimBlend > 0.5f ? 0.f : -TurnT * 0.20f) - CamLean) * F::Damp(bGrounded ? 9.f : 3.4f, Dt);
	// Web: positive lean rolls the camera counter-clockwise; in a left turn (TurnT>0) it rolls clockwise.
	Rot.Roll = -FMath::RadiansToDegrees(CamLean);

	// FOV: the web values are vertical; UE wants horizontal.
	const float VTarget = FOV_BASE + FOV_SPEED_GAIN * SpeedT * SpeedT + FOV_TURBO * Flight.GetTurboT() * (bGrounded ? 0.f : 1.f) - AIM_FOV * AimBlend;
	FovV += (VTarget - FovV) * F::Damp(FOV_LAMBDA, Dt);
	const float HFov = 2.f * FMath::RadiansToDegrees(FMath::Atan(FMath::Tan(FMath::DegreesToRadians(FovV) * 0.5f) * ViewAspect()));

	FVector CamOut = CamPosM;
	if (Shake > 0.f)
	{
		// A hard landing or a knock: a short, heavy jolt.
		ShakeT += Dt;
		CamOut += FVector(FMath::Sin(ShakeT * 37.f), FMath::Sin(ShakeT * 29.f + 1.f), FMath::Sin(ShakeT * 43.f + 2.f) * 1.6f) * (0.11f * Shake * DragonScale);
		Shake = FMath::Max(0.f, Shake - Dt * 2.4f);
	}
	Camera->SetWorldLocationAndRotation(CamOut * M, Rot);
	Camera->SetFieldOfView(FMath::Clamp(HFov, 40.f, 150.f));

	// Trees between the camera and the dragon fade out (M_Foliage / M_SurfaceFade read this).
	if (!ViewMPC) ViewMPC = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/World/Materials/MPC_Weather.MPC_Weather"));
	if (ViewMPC)
	{
		const FVector F = FocusM * M;
		UKismetMaterialLibrary::SetVectorParameterValue(this, ViewMPC, TEXT("Focus"), FLinearColor(F.X, F.Y, F.Z, 450.f * DragonScale));
	}
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
	if (bGrounded) return bApproach ? TEXT("Landing") : bSwimming ? TEXT("Swimming") : FMath::Abs(WalkSpeed) > 9.f ? TEXT("Galloping") : FMath::Abs(WalkSpeed) > 0.4f ? TEXT("Walking") : TEXT("Standing");
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

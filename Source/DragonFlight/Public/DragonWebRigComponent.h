#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DragonFlightModel.h"
#include "DragonFuloRig.h"
#include "DragonWebRigComponent.generated.h"

class UPoseableMeshComponent;
class USkinnedAsset;

/** What the web rig reads each frame. Everything is in the web game's units and sign conventions. */
struct FDragonWebRigInput
{
	FDragonFlightState FS;
	/** controls.isBursting(): cancels flap-glide (wings.js state.boost). */
	bool bBoost = false;
	bool bGrounded = false;
	/** Landing run-out: only the fold runs (main.js: groundBody.flaring). */
	bool bFlaring = false;
	/** Ground speed, m/s, and what counts as a full run (main.js passes groundBody.speed and RUN_SPEED). */
	float GroundSpeed = 0.f;
	float MaxGroundSpeed = 15.f;
	/** aim.js: held and allowed (in the air). AimYaw/AimPitch are the WANTED head turn, radians, already clamped. */
	bool bAimActive = false;
	float AimYaw = 0.f;
	float AimPitch = 0.f;
	/** Ground height in metres under a UE-axis point in metres (web groundAt(x, z) with x = UE X, z = UE Y). Unset = no gait.js, sine legs. */
	TFunction<float(float, float)> GroundAtM;
};

/**
 * The web game's procedural animation for its own 152-bone dragon
 * (dragon_rigged_hd.glb), ported from js/wings.js, js/flightrig.js,
 * js/dragonrig.js (+ js/foldpose.js) and js/gait.js, plus js/aim.js's head turn.
 *
 * HOW IT STAYS FAITHFUL WHATEVER THE IMPORTER DID
 *
 * The web code poses bones as three.js local rotations (XYZ eulers and
 * quaternions) in the glTF's own frame. Rather than guess how Interchange
 * re-oriented the skeleton, the glTF rest pose is baked into C++
 * (DragonWebRigData.inl, generated from the glb), and every frame the whole
 * pose is computed in glTF space exactly as three.js would. Each bone's
 * glTF component-space DELTA from rest is then carried into UE through one
 * linear map A (glTF model space -> UE component space), fitted at bind by
 * least squares from the 152 bone positions of the two rest poses:
 *
 *     C_ue(anim) = (A * dC_gl * A^-1) o C_ue(rest),   dC_gl = C_gl(anim) o C_gl(rest)^-1
 *
 * That is exactly the condition for the skinned vertices to move the same way
 * (v_ue = A v_gl), so it holds however Interchange rolled the bone-local axes,
 * swapped handedness or scaled units. A = s * R with R orthogonal (det may be -1,
 * a handedness flip); a rotation conjugated by R is a rotation about
 * det(R) * R * axis by the same angle, which is how the quaternions are mapped.
 *
 * The mesh's facing is derived the same way: forward = Tail.001 -> Head and
 * up = A * glTF +Y in component space, giving the relative transform that puts
 * him facing +X, left wing on -Y, glTF origin (his feet) on the pivot, metres
 * scaled to centimetres.
 */
UCLASS(ClassGroup = (Dragon), meta = (BlueprintSpawnableComponent))
class DRAGONFLIGHT_API UDragonWebRigComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	/** Wingbeat phase 0..1 (the downstroke is [0, GetDownstroke())), amplitude (0.6 = hover) and glide blend. */
	float GetWingPhase01() const { return WP; }
	float GetDownstroke() const { return WDown; }
	float GetWingAmp() const { return WAmp; }
	float GetGlide() const { return GlideK; }
	UDragonWebRigComponent();

	/** True when the asset is the web game's 152-bone dragon (has Wing_Finger.001.L, under whatever spelling the importer used). */
	static bool IsWebSkeleton(const USkinnedAsset* Asset);

	bool Bind(UPoseableMeshComponent* InMesh);
	bool IsBound() const { return bBound; }
	/** Relative transform for the mesh component under the pivot: faces +X, left wing -Y, feet on the pivot, cm. */
	const FTransform& GetMeshRelativeTransform() const { return MeshRelative; }
	/** The Fulonimation Studios Night Fury: this rig's motion retargeted onto its own rig (DragonWebRigFulo.cpp). */
	bool IsFulo() const { return bFulo; }
	/** Tail fin: 0 Black, 1 Red (first film), 2 Brown, 3 Blue, 4 Purple, 5 Green flames, 6 Strike class, 7 bare frame. */
	void SetFinStyle(int32 Style);
	static int32 NumFinStyles() { return 8; }
	static const TCHAR* FinStyleName(int32 Style);
	/** The player's fin, saved with the settings (GameUserSettings, [/Script/DragonFlight.DragonControls] FinStyle). */
	static int32 SavedFinStyle();
	static void SaveFinStyle(int32 Style);
	int32 GetFinStyle() const { return FinStyle; }

	/** Poses the mesh. Returns the wingbeat heave in metres (wings.js getHeave), 0 on the ground. */
	float Animate(float Dt, const FDragonWebRigInput& In);

	/** Touchdown (main.js land()): fold from wherever the wings are. */
	void BeginFold();
	/** Take-off (main.js takeOff()): unfold to the bind pose; the fold keeps running over the beat until open. */
	void BeginUnfold();
	/** Snap the fold, 1 = folded, 0 = spread. */
	void SnapFold(float V);
	float GetFold() const { return Fold; }

	// --- Wing stowing (BONES_HD.md) ----------------------------------------------
	/** Past StowFrom of the fold, hide the wing_membrane section and collapse Wing_Shoulder (scale inherits down the arm and digits). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRig|Fold") bool bStowWingsPastHalfFold = false; // current web game keeps the membrane visible at every fold
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRig|Fold") float StowFrom = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRig|Fold") float StowShoulderScale = 0.02f;

	// --- main.js `tuning` -----------------------------------------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRig|Wings") float FlapAmplitude = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRig|Wings") float FlapSpeed = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRig|Wings") float SweepAmount = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WebRig|Wings") float TuckSign = 1.f;

private:
	struct FBone
	{
		int32 Parent = INDEX_NONE;
		int32 UE = INDEX_NONE;
		FQuat RestQ = FQuat::Identity;
		FVector RestT = FVector::ZeroVector;
		float RestS = 1.f;
		FVector RestE = FVector::ZeroVector; // three.js XYZ euler of RestQ
		FQuat Q = FQuat::Identity;
		FVector T = FVector::ZeroVector;
		float S = 1.f;
		FVector E = FVector::ZeroVector;
		bool bE = true; // E is in sync with Q
	};
	struct FFoldEntry
	{
		int32 Bone = INDEX_NONE;
		bool bSolved = false;     // slerp FromQ -> FoldQ
		FQuat FoldQ = FQuat::Identity;
		FQuat FromQ = FQuat::Identity;
		int32 Axis = 0;           // else: RestQ * axis(Axis, Amount * fold)
		float Amount = 0.f;
	};
	struct FGaitLeg
	{
		int32 A = INDEX_NONE, B = INDEX_NONE, C = INDEX_NONE, D = INDEX_NONE, E = INDEX_NONE;
		bool bFore = false;
		float L1 = 0, L2 = 0, L3 = 0, ToeLen = 0.12f, ToeH = 0, Meta0 = 0, ToePitch0 = 0, TipH = 0;
		FVector Home = FVector::ZeroVector, Pole = FVector::ZeroVector;
		bool bPlant = false;
		FVector Plant = FVector::ZeroVector, LiftFrom = FVector::ZeroVector, Foot = FVector::ZeroVector;
		bool bDown = true, bWasDown = true;
		float P = 0, S = 0;
	};
	struct FBeat { float Phase = 0, Amp = 0.6f, Glide = 0, Outer = 0, Tip = 0, Fold = 0, Cup = 0, Size = 0; };

	int32 Find(const FString& GltfName) const;
	int32 FindF(const TCHAR* Base, int32 N, const TCHAR* Side) const;

	// three.js bone ops, glTF local space
	void ResetPose();
	void SetE(int32 I, int32 Axis, float V);
	void AddE(int32 I, int32 Axis, float V);
	void SetQ(int32 I, const FQuat& Q);
	void RotateLocal(int32 I, int32 Axis, float V);
	const FTransform& Model(int32 I);
	void RecomputeModel();

	// model (glTF) space <-> web world (three.js world, metres)
	FVector ModelToWeb(const FVector& P) const;
	FVector ModelDirToWeb(const FVector& D) const;
	FVector WebToModel(const FVector& W) const;
	FVector ApplyAn(const FVector& V) const;
	FVector ApplyAnT(const FVector& V) const;

	// ported systems
	void UpdateWings(float Dt, const FDragonWebRigInput& In);
	FBeat GetBeat() const;
	void UpdateFlightRig(float Dt, const FDragonFlightState& S, const FBeat& Beat);
	float ApplyFold(float Dt);
	void UpdateGround(float Dt, const FDragonWebRigInput& In);
	void UpdateSineLegs(float Dt);
	bool CreateGait();
	void UpdateGait(float RawDt, const FDragonWebRigInput& In);
	void GaitAim(int32 Bone, const FVector& ChildW, const FVector& TargetW);
	/** Same, with positions in the model's own space. */
	void AimModel(int32 Bone, const FVector& ChildM, const FVector& TargetM);
	/** Legs in the air, by IK: hind legs trailing back along the belly, forelegs folded under the
	 *  chest; Ready (0..1) lowers them toward a landing reach. K blends over the current pose. */
	void FlightLegs(float K, float Ready, float Dt);
	void TailCarriage(TFunctionRef<double(double, double)> GroundAt);
	float SFlightReady = 0.f;
	/** The folded wing, built by aiming each bone (anatomy, not a baked pose): humerus back and up,
	 *  forearm forward to a wrist high over the shoulder, the fingers stacked back along the flank. */
	void FoldIK(float K);
	void ApplyAim(float Dt, const FDragonWebRigInput& In);
	void ApplyStow();
	void WriteToMesh();
	void SetMembraneShown(bool bShow);

	UPROPERTY(Transient) TObjectPtr<UPoseableMeshComponent> Mesh;
	bool bBound = false;

	TArray<FBone> B;
	TMap<FString, int32> ByKey;
	TArray<FTransform> RestModel;
	TArray<FTransform> ModelCache;
	bool bModelDirty = true;

	// UE skeleton
	TArray<int32> UEParent;
	TArray<int32> UEToGl;
	TArray<FTransform> UERefLocal, UERefCS, UEAnimCS;

	// glTF model -> UE component: c = AScale * An * p + AOffset
	double An[3][3] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1} };
	double AScale = 100.0;
	double ADet = 1.0;
	FVector AOffset = FVector::ZeroVector;
	FTransform MeshRelative = FTransform::Identity;
	FTransform CompToWorld = FTransform::Identity;

	int32 MembraneMaterial = INDEX_NONE;
	bool bMembraneShown = true;

	// --- the Fulonimation Studios rig (DragonWebRigFulo.cpp) ---
	bool bFulo = false;
	TSharedPtr<FDragonFuloRig> Fulo;
	bool BindFulo(const struct FReferenceSkeleton& Ref);
	FString WebBoneName(int32 G) const;
	void WriteFulo();
	TArray<int32> FuloToUE, UEToFulo;
	TArray<FFuloM4> FuloK;             // per Fulo bone: (C * Rest)^-1 * UE ref (component space)
	FFuloM4 FuloC;                     // Fulo armature -> UE component
	double FuloSW = 0.8;               // web metres per Fulo armature unit
	FVector FuloOffW = FVector::ZeroVector;
	struct FFuloDir { int32 Bone = INDEX_NONE; int32 Web = INDEX_NONE; int32 Side = 0; bool bWing = false; float Arc = -1.f; };
	TArray<FFuloDir> FuloDirs;         // bones aimed along a web bone (wing segments, fingers) or the web tail at an arc
	struct FFuloRib { int32 Bone = INDEX_NONE; int32 Side = 0; int32 A = INDEX_NONE, B = INDEX_NONE; float W = 0.f; FVector RestDir = FVector::ZeroVector; };
	TArray<FFuloRib> FuloRibs;         // membrane ribs between two fingers (indices into FuloDirs)
	struct FFuloFollow { int32 Bone = INDEX_NONE; int32 WebPos = INDEX_NONE; int32 WebRot = INDEX_NONE; };
	TArray<FFuloFollow> FuloFollows;   // controls that follow a web bone rigidly
	struct FFuloRel { int32 Bone = INDEX_NONE; int32 Web = INDEX_NONE; int32 WebParent = INDEX_NONE; };
	TArray<FFuloRel> FuloRels;         // bones turned as a web bone turns against its parent (tail fins, ears)
	TArray<float> FuloTailArc; TArray<int32> FuloTailWeb;   // web tail bones by arc
	int32 FuloHead = INDEX_NONE, FuloNeckStart = INDEX_NONE, FuloWingFold[2] = { -1, -1 };
	int32 FinMaterial = INDEX_NONE;
	int32 FinStyle = 1;

	// --- bone indices (glTF table), [0] = .L, [1] = .R ---
	int32 Clav[2] = { -1, -1 }, WShoulder[2] = { -1, -1 }, WUpper[2] = { -1, -1 }, WFore[2] = { -1, -1 };
	int32 Finger[2][18];
	int32 FinStrut[2][6], FinTip[2][6];
	int32 Thigh[2], Shin[2], Ankle[2], Toe[2], UpperArm[2], Forearm[2], Wrist[2], FrontToe[2];
	int32 FrontDigit[2][3], HindDigit[2][3];
	int32 EarA1[2], EarA2[2], EarB1[2], EarB2[2], EarC[2], Eye[2];
	int32 TailB[11];
	int32 NeckB[3];
	int32 Dorsal[8];
	int32 SpineB[3];
	int32 Hips = -1, HeadB = -1, Jaw = -1;
	TArray<FFoldEntry> FoldList;

	// --- wings.js ---
	float WP = 0.f, WAmp = 0.4f, WBias = 0.06f, WSweep = 0.f, WDown = 0.5f, WHeave = 0.f;
	bool bGliding = false;
	float GlideLeft = 0.f, GlideK = 0.f, LastP = 0.f;
	int32 BeatsLeft = 4;

	// --- flightrig.js ---
	float STurn = 0, SClimb = 0, SSpeed = 0, SHang = 0, SLoad = 0, SRoll = 0;

	// --- dragonrig.js ---
	float Fold = 0.f, FoldTarget = 0.f;
	bool bOpeningWings = false;
	float GPhase = 0.f, MoveAmt = 0.f, TailCurl = 0.f;

	// --- gait.js ---
	bool bGaitTried = false, bGaitOK = false, bGaitFirst = true, bStill = true;
	FGaitLeg Legs[4];
	float TailDownSign = 0.f;
	float NeckUpSign = 0.f, NeckLenM = 1.f; // which way a local X turn on the neck raises the head, and the neck length (m)
	float HipH = 1.f, GPhase01 = 0.f, Lean = 0.f, HeadYawLag = 0.f, Carry = 0.f, YawRate = 0.f, PrevYaw = 0.f;
	FVector GVel = FVector::ZeroVector, PrevPos = FVector::ZeroVector, HipsRestPos = FVector::ZeroVector;

	// --- aim.js ---
	float AimYaw = 0.f, AimPitch = 0.f;

	float TimeS = 0.f;
};

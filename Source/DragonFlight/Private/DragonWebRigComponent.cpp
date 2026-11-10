#include "DragonWebRigComponent.h"
#include "DragonFlight.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Components/PoseableMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

// The glTF rest pose of dragon_rigged_hd.glb and js/foldpose.js's solved fold.
// Regenerate with Tools/gen_web_rig_data.py if the web model is re-rigged.
#include "DragonWebRigData.inl"

// ---------------------------------------------------------------------------
// A line-for-line port of the web game's dragon animation:
//
//   js/wings.js       the wingbeat (clavicles), flap-glide, heave
//   js/flightrig.js   everything else in the air: legs tucked, camber, washout,
//                     upstroke fold, load bow, roll twist, tail fins as rudder
//                     and elevator, tail carve, head/neck look, crest, ears, eyes
//   js/dragonrig.js   the ground rig: the solved wing fold (js/foldpose.js), the
//                     tail sail and fin furl, the fold/unfold handover, and the
//                     sine-wave legs used when there is no ground to plant on
//   js/gait.js        the real gait: planted feet, two-bone leg IK, walk/trot/
//                     gallop by Froude number, body bob/lean, head stabilised
//   js/aim.js         the head turning toward the crosshair (applyToRig)
//
// Every pose is computed in the glTF's own frame on a copy of the skeleton, with
// three.js semantics (XYZ eulers: `set` = rest + delta on one axis, `add` = on
// top of the current rotation; quaternions Hamilton, q * axis = about the bone's
// own axis). Only the final component-space deltas cross into UE — see the
// header for the basis change.
// ---------------------------------------------------------------------------

namespace
{
	constexpr int32 AX = 0, AY = 1, AZ = 2;

	float Damp(float L, float Dt) { return 1.f - FMath::Exp(-L * Dt); }
	float Smooth(float X, float A, float B) // THREE.MathUtils.smoothstep
	{
		if (X <= A) return 0.f;
		if (X >= B) return 1.f;
		const float T = (X - A) / (B - A);
		return T * T * (3.f - 2.f * T);
	}
	float Lerp(float A, float B, float T) { return A + (B - A) * T; }
	float Mod1(float X) { X = FMath::Fmod(X, 1.f); return X < 0.f ? X + 1.f : X; }

	FString Norm(const FString& S)
	{
		FString O;
		O.Reserve(S.Len());
		for (TCHAR C : S)
		{
			C = FChar::ToLower(C);
			if ((C >= 'a' && C <= 'z') || (C >= '0' && C <= '9')) O.AppendChar(C);
		}
		return O;
	}

	FVector AxisVec(int32 A) { return A == AX ? FVector(1, 0, 0) : A == AY ? FVector(0, 1, 0) : FVector(0, 0, 1); }
	FQuat AxisQ(int32 A, float Rad) { return FQuat(AxisVec(A), Rad); }

	// three.js Quaternion.setFromEuler, order XYZ
	FQuat QFromEuler(const FVector& E)
	{
		const double c1 = FMath::Cos(E.X / 2), c2 = FMath::Cos(E.Y / 2), c3 = FMath::Cos(E.Z / 2);
		const double s1 = FMath::Sin(E.X / 2), s2 = FMath::Sin(E.Y / 2), s3 = FMath::Sin(E.Z / 2);
		return FQuat(s1 * c2 * c3 + c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3, c1 * c2 * s3 + s1 * s2 * c3, c1 * c2 * c3 - s1 * s2 * s3);
	}
	// three.js Euler.setFromQuaternion, order XYZ
	FVector EulerFromQ(const FQuat& Q)
	{
		const double x = Q.X, y = Q.Y, z = Q.Z, w = Q.W;
		const double m11 = 1 - 2 * (y * y + z * z), m12 = 2 * (x * y - w * z), m13 = 2 * (x * z + w * y);
		const double m22 = 1 - 2 * (x * x + z * z), m23 = 2 * (y * z - w * x);
		const double m32 = 2 * (y * z + w * x), m33 = 1 - 2 * (x * x + y * y);
		FVector E;
		E.Y = FMath::Asin(FMath::Clamp(m13, -1.0, 1.0));
		if (FMath::Abs(m13) < 0.9999999)
		{
			E.X = FMath::Atan2(-m23, m33);
			E.Z = FMath::Atan2(-m12, m11);
		}
		else
		{
			E.X = FMath::Atan2(m32, m22);
			E.Z = 0;
		}
		return E;
	}

	double Det3(const double M[3][3])
	{
		return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
			- M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
			+ M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
	}
	bool Inv3(const double M[3][3], double O[3][3])
	{
		const double D = Det3(M);
		if (FMath::Abs(D) < 1e-30) return false;
		O[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) / D;
		O[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) / D;
		O[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) / D;
		O[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) / D;
		O[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) / D;
		O[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) / D;
		O[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) / D;
		O[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) / D;
		O[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) / D;
		return true;
	}

	// --- wings.js constants ---
	constexpr float MAX_SWEEP = 0.55f;
	constexpr float F_HOVER = 0.95f, F_CRUISE = 0.72f, F_FAST = 0.6f, F_CLIMB = 0.22f;
	constexpr float A_HOVER = 0.6f, A_CRUISE = 0.4f, A_FAST = 0.22f, A_CLIMB = 0.14f, A_MAX = 0.72f;
	constexpr float BIAS_HOVER = 0.14f, BIAS_CRUISE = 0.06f;
	constexpr float HAND_AMP = 1.25f, HAND_LAG = 0.1f, TIP_AMP = 1.5f, TIP_LAG = 0.19f;
	constexpr float FOLD_LEAD = 0.07f, FOLD_OPEN = 0.07f;
	constexpr float DOWN_SLOW = 0.5f, DOWN_CRUISE = 0.5f;
	constexpr float HEAVE_HOVER = 0.24f, HEAVE_CRUISE = 0.07f;

	float Wave(float Q, float D) { return Q < D ? FMath::Cos(PI * Q / D) : -FMath::Cos(PI * (Q - D) / (1.f - D)); }
	float PhaseRad(float Q, float D) { return Q < D ? PI * Q / D : PI + PI * (Q - D) / (1.f - D); }

	// --- flightrig.js constants ---
	constexpr float TUCK_THIGH = -1.15f, TUCK_SHIN = 1.75f, TUCK_ANKLE = 0.70f, TUCK_UPPER = -0.95f, TUCK_FORE = 1.55f, TUCK_WRIST = 0.55f, LEG_LOOSE = 0.30f;
	constexpr float CAMBER = 0.30f, WASHOUT = 0.22f, WRIST_FLEX = 0.5f, UPPER_FOLD = 0.12f, DIGIT_FOLD = 0.14f, SWEEP_DIGIT = 0.34f;
	constexpr float HANG_CAMBER = 0.62f, HANG_WASHOUT = 0.55f, HANG_SWEEP = 0.40f, HANG_WRIST = 0.60f, HANG_FIN = 0.35f;
	constexpr float LOAD_CAMBER = 0.16f, LOAD_TIP = 0.30f, LOAD_ASYM = 0.34f, LOAD_MAX = 16.f;
	constexpr float ROLL_CLOSE = 0.52f, ROLL_CURL = 0.34f, ROLL_CURL_TIP = 0.30f, ROLL_RAKE = 0.42f, ROLL_TWIST = 0.55f, ROLL_TIP = 0.45f;
	constexpr float ROLL_TUCK = 0.30f, ROLL_REACH = 0.16f, ROLL_FIN = 0.85f, ROLL_TAIL = 0.10f, ROLL_HEAD = 0.5f, ROLL_LEG = 0.35f;
	constexpr float FIN_SPREAD = 0.65f, FIN_RUDDER = 0.55f, FIN_ELEVATOR = 0.40f, TAIL_CARVE = 0.055f, TAIL_WAVE = 0.020f;
	constexpr float HEAD_LOOK = 0.42f, HEAD_PITCH = 0.30f, NECK_SHARE = 0.30f, CREST_FLAT = 0.45f, EAR_SWEEP = 0.55f;

	// --- dragonrig.js constants ---
	constexpr float FOLD_SAIL = 0.60f, FOLD_FIN = 0.55f;
	constexpr float FOLD_RATE_CLOSE = 3.2f, FOLD_RATE_OPEN = 7.0f;
	constexpr float STRIDE_FRONT = 0.42f, STRIDE_REAR = 0.50f, KNEE_BEND = 0.38f, STEPS_PER_M = 0.62f;
	constexpr float FOOT_LEVEL = 0.85f, FOOT_LIFT = 0.55f, FOOT_PUSH = 0.40f, TOE_CURL = 0.45f, TOE_SPLAY = 0.16f;

	// --- gait.js ---
	constexpr float GRAV = 9.81f;
	constexpr float HEAD_CARRY = 0.35f;
	struct FGaitDef { float Offsets[4]; float Duty; float Lift; };
	const FGaitDef GWalk = { { 0.25f, 0.75f, 0.0f, 0.5f }, 0.66f, 0.11f };
	const FGaitDef GTrot = { { 0.5f, 0.0f, 0.0f, 0.5f }, 0.44f, 0.15f };
	const FGaitDef GGallop = { { 0.62f, 0.5f, 0.0f, 0.1f }, 0.26f, 0.22f };

	// --- aim.js ---
	constexpr float AIM_HEAD_RATE = 10.f, AIM_NECK_SHARE = 0.20f, AIM_HEAD_SHARE = 0.40f;

	const TCHAR* SideStr[2] = { TEXT("L"), TEXT("R") };
}

UDragonWebRigComponent::UDragonWebRigComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UDragonWebRigComponent::IsWebSkeleton(const USkinnedAsset* Asset)
{
	if (!Asset) return false;
	const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
	for (int32 i = 0; i < Ref.GetNum(); ++i)
	{
		const FString N = Norm(Ref.GetBoneName(i).ToString());
		if (N == TEXT("wingfinger001l") || N == TEXT("wingshoulderl")) return true;   // the web dragon, or the Fulonimation one
	}
	return false;
}

FString UDragonWebRigComponent::WebBoneName(int32 G) const
{
	return G >= 0 && G < int32(UE_ARRAY_COUNT(GDragonWebBones)) ? FString(UTF8_TO_TCHAR(GDragonWebBones[G].Name)) : FString();
}

int32 UDragonWebRigComponent::Find(const FString& GltfName) const
{
	const int32* I = ByKey.Find(Norm(GltfName));
	return I ? *I : INDEX_NONE;
}

int32 UDragonWebRigComponent::FindF(const TCHAR* Base, int32 N, const TCHAR* Side) const
{
	// Base.00N.Side, e.g. Wing_Finger.001.L
	return Find(FString::Printf(TEXT("%s.%03d.%s"), Base, N, Side));
}

// ===========================================================================
// Bind
// ===========================================================================
bool UDragonWebRigComponent::Bind(UPoseableMeshComponent* InMesh)
{
	bBound = false;
	Mesh = InMesh;
	USkinnedAsset* Asset = Mesh ? Mesh->GetSkinnedAsset() : nullptr;
	if (!Asset) return false;
	const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
	const int32 NU = Ref.GetNum();

	// --- the glTF skeleton, from the baked table ----------------------------
	const int32 NG = UE_ARRAY_COUNT(GDragonWebBones);
	B.SetNum(NG);
	ByKey.Reset();
	for (int32 i = 0; i < NG; ++i)
	{
		const FDragonWebBoneDef& D = GDragonWebBones[i];
		FBone& Bo = B[i];
		Bo = FBone();
		Bo.Parent = D.Parent;
		Bo.RestQ = FQuat(D.R[0], D.R[1], D.R[2], D.R[3]).GetNormalized();
		Bo.RestT = FVector(D.T[0], D.T[1], D.T[2]);
		Bo.RestS = D.S;
		Bo.RestE = EulerFromQ(Bo.RestQ);
		ByKey.Add(Norm(UTF8_TO_TCHAR(D.Name)), i);
	}
	ResetPose();
	RecomputeModel();
	RestModel = ModelCache;

	// --- the Fulonimation Studios dragon: this skeleton runs headless and drives that rig ---
	bFulo = false;
	for (int32 u = 0; u < NU && !bFulo; ++u) bFulo = Norm(Ref.GetBoneName(u).ToString()) == TEXT("wingshoulderl");
	if (bFulo && !BindFulo(Ref)) return false;
	if (!bFulo) {
	// --- match UE bones by name (dots/underscores/case ignored) -------------
	UEParent.SetNum(NU);
	UEToGl.Init(INDEX_NONE, NU);
	UERefLocal = Ref.GetRefBonePose();
	UERefCS.SetNum(NU);
	UEAnimCS.SetNum(NU);
	int32 Matched = 0;
	for (int32 u = 0; u < NU; ++u)
	{
		UEParent[u] = Ref.GetParentIndex(u);
		UERefCS[u] = UEParent[u] == INDEX_NONE ? UERefLocal[u] : UERefLocal[u] * UERefCS[UEParent[u]];
		const int32* G = ByKey.Find(Norm(Ref.GetBoneName(u).ToString()));
		if (G && B[*G].UE == INDEX_NONE)
		{
			UEToGl[u] = *G;
			B[*G].UE = u;
			++Matched;
		}
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("RigDebug")))
	{
		FString Un = FString::Printf(TEXT("bind %s: matched %d of %d web bones, UE has %d\nunmatched web:"), *Asset->GetName(), Matched, NG, NU);
		for (int32 g = 0; g < B.Num(); ++g) if (B[g].UE == INDEX_NONE) Un += TEXT(" ") + FString(UTF8_TO_TCHAR(GDragonWebBones[g].Name));
		Un += TEXT("\nunmatched UE:");
		for (int32 u = 0; u < NU; ++u) if (UEToGl[u] == INDEX_NONE) Un += TEXT(" ") + Ref.GetBoneName(u).ToString();
		FFileHelper::SaveStringToFile(Un + TEXT("\n"), *(FPaths::ProjectSavedDir() / TEXT("RigDebug.txt")), FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
	}
	if (Matched < NG * 3 / 4 || Find(TEXT("Wing_Finger.001.L")) == INDEX_NONE || B[Find(TEXT("Wing_Finger.001.L"))].UE == INDEX_NONE)
	{
		UE_LOG(LogDragon, Warning, TEXT("WebRig: only %d of %d web bones found on %s; not the web dragon."), Matched, NG, *Asset->GetName());
		return false;
	}

	// --- fit A: glTF model space -> UE component space ----------------------
	// q = A p + c over every matched bone's rest position. 152 points spread over
	// a whole dragon pin a 3x3 down completely; nothing about the importer's axis
	// convention, handedness or units is assumed.
	{
		FVector Pg = FVector::ZeroVector, Pu = FVector::ZeroVector;
		int32 N = 0;
		for (int32 g = 0; g < NG; ++g)
		{
			if (B[g].UE == INDEX_NONE) continue;
			Pg += RestModel[g].GetLocation();
			Pu += UERefCS[B[g].UE].GetLocation();
			++N;
		}
		Pg /= N; Pu /= N;
		double H[3][3] = {}, K[3][3] = {};
		for (int32 g = 0; g < NG; ++g)
		{
			if (B[g].UE == INDEX_NONE) continue;
			const FVector P = RestModel[g].GetLocation() - Pg;
			const FVector Q = UERefCS[B[g].UE].GetLocation() - Pu;
			for (int32 r = 0; r < 3; ++r)
				for (int32 c = 0; c < 3; ++c)
				{
					H[r][c] += Q[r] * P[c];
					K[r][c] += P[r] * P[c];
				}
		}
		double KI[3][3], A[3][3] = {};
		if (!Inv3(K, KI))
		{
			UE_LOG(LogDragon, Warning, TEXT("WebRig: degenerate rest pose, cannot fit the basis."));
			return false;
		}
		for (int32 r = 0; r < 3; ++r)
			for (int32 c = 0; c < 3; ++c)
				for (int32 k = 0; k < 3; ++k) A[r][c] += H[r][k] * KI[k][c];
		const double DetA = Det3(A);
		AScale = FMath::Pow(FMath::Abs(DetA), 1.0 / 3.0);
		for (int32 r = 0; r < 3; ++r)
			for (int32 c = 0; c < 3; ++c) An[r][c] = A[r][c] / AScale;
		// Polar decomposition by iteration: the nearest orthogonal matrix.
		for (int32 It = 0; It < 12; ++It)
		{
			double I[3][3];
			if (!Inv3(An, I)) break;
			for (int32 r = 0; r < 3; ++r)
				for (int32 c = 0; c < 3; ++c) An[r][c] = 0.5 * (An[r][c] + I[c][r]);
		}
		ADet = Det3(An) < 0 ? -1.0 : 1.0;
		AOffset = Pu - ApplyAn(Pg) * AScale;
		double Err = 0;
		for (int32 g = 0; g < NG; ++g)
		{
			if (B[g].UE == INDEX_NONE) continue;
			Err += FVector::DistSquared(ApplyAn(RestModel[g].GetLocation()) * AScale + AOffset, UERefCS[B[g].UE].GetLocation());
		}
		Err = FMath::Sqrt(Err / N);
		UE_LOG(LogDragon, Log, TEXT("WebRig: %d/%d bones, glTF->UE basis [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f] x %.2f, det %+.0f, offset %s, fit rms %.3f units"),
			Matched, NG, An[0][0], An[0][1], An[0][2], An[1][0], An[1][1], An[1][2], An[2][0], An[2][1], An[2][2], AScale, ADet, *AOffset.ToString(), Err);
		if (Err > 0.05 * AScale)
		{
			UE_LOG(LogDragon, Warning, TEXT("WebRig: the imported rest pose does not match dragon_rigged_hd.glb's (rms %.1f). Re-export or regenerate DragonWebRigData.inl."), Err);
		}
	}

	// --- which way he faces, measured off the reference pose ----------------
	{
		const int32 H = Find(TEXT("Head")), T1 = Find(TEXT("Tail.001"));
		const int32 CL = Find(TEXT("Wing_Clavicle.L")), CR = Find(TEXT("Wing_Clavicle.R"));
		FVector Up = ApplyAn(FVector(0, 1, 0)).GetSafeNormal(); // the glb is Y-up
		FVector Fwd = (H >= 0 && T1 >= 0 && B[H].UE >= 0 && B[T1].UE >= 0)
			? UERefCS[B[H].UE].GetLocation() - UERefCS[B[T1].UE].GetLocation()
			: ApplyAn(FVector(0, 0, -1));
		Fwd = (Fwd - Up * FVector::DotProduct(Fwd, Up)).GetSafeNormal();
		const FQuat Body(FRotationMatrix::MakeFromXZ(Fwd, Up));
		const FQuat Inv = Body.Inverse();
		if (CL >= 0 && CR >= 0 && B[CL].UE >= 0 && B[CR].UE >= 0)
		{
			const FVector LR = Inv.RotateVector(UERefCS[B[CL].UE].GetLocation() - UERefCS[B[CR].UE].GetLocation());
			if (LR.Y > 0.f)
			{
				UE_LOG(LogDragon, Warning, TEXT("WebRig: the imported dragon is mirrored (left wing on +Y once facing +X)."));
			}
		}
		const double K = 100.0 / AScale; // glTF metres -> cm
		MeshRelative = FTransform(Inv, -Inv.RotateVector(AOffset) * K, FVector(K));
		UE_LOG(LogDragon, Log, TEXT("WebRig: forward %s up %s in component space; mesh relative %s"), *Fwd.ToString(), *Up.ToString(), *MeshRelative.ToString());
	}

	// --- membrane section ----------------------------------------------------
	MembraneMaterial = INDEX_NONE;
	{
		const TArray<FSkeletalMaterial>& Mats = Asset->GetMaterials();
		for (int32 m = 0; m < Mats.Num(); ++m)
		{
			const FString A = Mats[m].MaterialSlotName.ToString();
#if WITH_EDITORONLY_DATA
			const FString Bn = Mats[m].ImportedMaterialSlotName.ToString();
#else
			const FString Bn;
#endif
			const FString C = Mats[m].MaterialInterface ? Mats[m].MaterialInterface->GetName() : FString();
			if (A.Contains(TEXT("wing_membrane")) || Bn.Contains(TEXT("wing_membrane")) || C.Contains(TEXT("wing_membrane")))
			{
				MembraneMaterial = m;
				break;
			}
		}
		if (MembraneMaterial == INDEX_NONE) UE_LOG(LogDragon, Warning, TEXT("WebRig: no wing_membrane material slot; the wing will not be stowed."));
		bMembraneShown = false;
		SetMembraneShown(true);
	}

	}   // !bFulo

	// --- bone indices ---------------------------------------------------------
	for (int32 s = 0; s < 2; ++s)
	{
		const TCHAR* S = SideStr[s];
		auto F1 = [&](const TCHAR* Base) { return Find(FString::Printf(TEXT("%s.%s"), Base, S)); };
		Clav[s] = F1(TEXT("Wing_Clavicle"));
		WShoulder[s] = F1(TEXT("Wing_Shoulder"));
		WUpper[s] = F1(TEXT("Wing_UpperArm"));
		WFore[s] = F1(TEXT("Wing_Forearm"));
		for (int32 i = 0; i < 18; ++i) Finger[s][i] = FindF(TEXT("Wing_Finger"), i + 1, S);
		for (int32 i = 0; i < 6; ++i)
		{
			FinStrut[s][i] = FindF(TEXT("Tail_Fin_Strut"), i + 1, S);
			FinTip[s][i] = FindF(TEXT("Tail_Fin_Tip"), i + 1, S);
		}
		Thigh[s] = F1(TEXT("Thigh")); Shin[s] = F1(TEXT("Shin")); Ankle[s] = F1(TEXT("Ankle")); Toe[s] = F1(TEXT("Toe"));
		UpperArm[s] = F1(TEXT("UpperArm")); Forearm[s] = F1(TEXT("Forearm")); Wrist[s] = F1(TEXT("Wrist")); FrontToe[s] = F1(TEXT("Front_Toe"));
		for (int32 i = 0; i < 3; ++i)
		{
			FrontDigit[s][i] = FindF(TEXT("Front_Digit"), i + 1, S);
			HindDigit[s][i] = FindF(TEXT("Hind_Digit"), i + 1, S);
		}
		EarA1[s] = F1(TEXT("Ear_A.001")); EarA2[s] = F1(TEXT("Ear_A.002"));
		EarB1[s] = F1(TEXT("Ear_B.001")); EarB2[s] = F1(TEXT("Ear_B.002"));
		EarC[s] = F1(TEXT("Ear_C")); Eye[s] = F1(TEXT("Eye"));
	}
	for (int32 i = 0; i < 11; ++i) TailB[i] = Find(FString::Printf(TEXT("Tail.%03d"), i + 1));
	for (int32 i = 0; i < 3; ++i) NeckB[i] = Find(FString::Printf(TEXT("Neck.%03d"), i + 1));
	for (int32 i = 0; i < 8; ++i) Dorsal[i] = Find(FString::Printf(TEXT("Dorsal.%03d"), i + 1));
	SpineB[0] = Find(TEXT("Spine")); SpineB[1] = Find(TEXT("Spine.001")); SpineB[2] = Find(TEXT("Spine.002"));
	Hips = Find(TEXT("Hips")); HeadB = Find(TEXT("Head")); Jaw = Find(TEXT("Jaw"));

	// --- the fold list (dragonrig.js bindDragon, hd branch) -------------------
	FoldList.Reset();
	for (const FDragonWebFoldDef& D : GDragonWebFold)
	{
		const int32 I = Find(UTF8_TO_TCHAR(D.Name));
		if (I == INDEX_NONE) continue;
		FFoldEntry E;
		E.Bone = I;
		E.bSolved = true;
		E.FoldQ = FQuat(D.Q[0], D.Q[1], D.Q[2], D.Q[3]).GetNormalized();
		E.FromQ = B[I].RestQ;
		FoldList.Add(E);
	}
	for (int32 s = 0; s < 2; ++s)
	{
		for (int32 n = 1; n <= 9; ++n)
		{
			const int32 I = Find(FString::Printf(TEXT("Tail_Sail_Strut_%02d.%s"), n, SideStr[s]));
			if (I == INDEX_NONE) continue;
			FFoldEntry E; E.Bone = I; E.Axis = AZ; E.Amount = FOLD_SAIL * (s == 0 ? 1.f : -1.f);
			FoldList.Add(E);
		}
	}
	for (int32 i = 1; i <= 6; ++i)
	{
		const float K = (i - 1) / 5.f;
		for (int32 s = 0; s < 2; ++s)
		{
			if (FinStrut[s][i - 1] >= 0) { FFoldEntry E; E.Bone = FinStrut[s][i - 1]; E.Axis = AX; E.Amount = FOLD_FIN * (0.4f + K * 0.9f); FoldList.Add(E); }
		}
		for (int32 s = 0; s < 2; ++s)
		{
			if (FinTip[s][i - 1] >= 0) { FFoldEntry E; E.Bone = FinTip[s][i - 1]; E.Axis = AX; E.Amount = FOLD_FIN * (0.3f + K * 0.6f); FoldList.Add(E); }
		}
	}

	bGaitTried = bGaitOK = false;
	SnapFold(0.f);
	bBound = true;
	// the Fulonimation rig's bind pose is not a flying pose (the web dragon's was): pose it now
	if (bFulo) { RecomputeModel(); WriteFulo(); }
	return true;
}

// ===========================================================================
// three.js bone ops
// ===========================================================================
void UDragonWebRigComponent::ResetPose()
{
	for (FBone& Bo : B)
	{
		Bo.Q = Bo.RestQ; Bo.T = Bo.RestT; Bo.S = Bo.RestS; Bo.E = Bo.RestE; Bo.bE = true;
	}
	bModelDirty = true;
}

void UDragonWebRigComponent::SetE(int32 I, int32 Axis, float V)
{
	if (I < 0) return;
	FBone& Bo = B[I];
	Bo.E = Bo.RestE;
	Bo.E[Axis] += V;
	Bo.Q = QFromEuler(Bo.E);
	Bo.bE = true;
	bModelDirty = true;
}

void UDragonWebRigComponent::AddE(int32 I, int32 Axis, float V)
{
	if (I < 0) return;
	FBone& Bo = B[I];
	if (!Bo.bE) { Bo.E = EulerFromQ(Bo.Q); Bo.bE = true; }
	Bo.E[Axis] += V;
	Bo.Q = QFromEuler(Bo.E);
	bModelDirty = true;
}

void UDragonWebRigComponent::SetQ(int32 I, const FQuat& Q)
{
	if (I < 0) return;
	B[I].Q = Q.GetNormalized();
	B[I].bE = false;
	bModelDirty = true;
}

void UDragonWebRigComponent::RotateLocal(int32 I, int32 Axis, float V)
{
	if (I < 0) return;
	SetQ(I, B[I].Q * AxisQ(Axis, V)); // Object3D.rotateX/Y/Z
}

void UDragonWebRigComponent::RecomputeModel()
{
	ModelCache.SetNum(B.Num());
	for (int32 i = 0; i < B.Num(); ++i)
	{
		const FTransform L(B[i].Q, B[i].T, FVector(B[i].S));
		ModelCache[i] = B[i].Parent < 0 ? L : L * ModelCache[B[i].Parent];
	}
	bModelDirty = false;
}

const FTransform& UDragonWebRigComponent::Model(int32 I)
{
	if (bModelDirty) RecomputeModel();
	return ModelCache[I];
}

FVector UDragonWebRigComponent::ApplyAn(const FVector& V) const
{
	return FVector(An[0][0] * V.X + An[0][1] * V.Y + An[0][2] * V.Z,
		An[1][0] * V.X + An[1][1] * V.Y + An[1][2] * V.Z,
		An[2][0] * V.X + An[2][1] * V.Y + An[2][2] * V.Z);
}

FVector UDragonWebRigComponent::ApplyAnT(const FVector& V) const
{
	return FVector(An[0][0] * V.X + An[1][0] * V.Y + An[2][0] * V.Z,
		An[0][1] * V.X + An[1][1] * V.Y + An[2][1] * V.Z,
		An[0][2] * V.X + An[1][2] * V.Y + An[2][2] * V.Z);
}

// Web world = UE world with Y and Z swapped, in metres (Docs/FlightModel_FromWeb.md).
FVector UDragonWebRigComponent::ModelToWeb(const FVector& P) const
{
	const FVector W = CompToWorld.TransformPosition(ApplyAn(P) * AScale + AOffset);
	return FVector(W.X, W.Z, W.Y) / 100.0;
}

FVector UDragonWebRigComponent::ModelDirToWeb(const FVector& D) const
{
	const FVector W = CompToWorld.TransformVector(ApplyAn(D) * AScale);
	return FVector(W.X, W.Z, W.Y) / 100.0;
}

FVector UDragonWebRigComponent::WebToModel(const FVector& Wb) const
{
	const FVector C = CompToWorld.InverseTransformPosition(FVector(Wb.X, Wb.Z, Wb.Y) * 100.0);
	return ApplyAnT(C - AOffset) / AScale;
}

// ===========================================================================
// Fold (dragonrig.js)
// ===========================================================================
void UDragonWebRigComponent::SnapFold(float V)
{
	FoldTarget = Fold = FMath::Clamp(V, 0.f, 1.f);
	for (FFoldEntry& E : FoldList) if (E.bSolved) E.FromQ = B[E.Bone].RestQ;
}

void UDragonWebRigComponent::BeginFold()
{
	// The web snapshots the live pose here so the fold starts from the last
	// frame of the beat rather than popping to spread. B[] still holds last
	// frame's final pose, so that is exactly what this takes.
	for (FFoldEntry& E : FoldList) if (E.bSolved) E.FromQ = B[E.Bone].Q;
	FoldTarget = 1.f;
	bOpeningWings = false;
	bGaitFirst = true;
	for (FGaitLeg& L : Legs) { L.bPlant = false; L.bDown = L.bWasDown = true; }
	bStill = true;
}

void UDragonWebRigComponent::BeginUnfold()
{
	for (FFoldEntry& E : FoldList) if (E.bSolved) E.FromQ = B[E.Bone].RestQ;
	FoldTarget = 0.f;
	bOpeningWings = true;
}

float UDragonWebRigComponent::ApplyFold(float Dt)
{
	const float Rate = FoldTarget > Fold ? FOLD_RATE_CLOSE : FOLD_RATE_OPEN;
	Fold += (FoldTarget - Fold) * Damp(Rate, Dt);
	if (FMath::Abs(Fold - FoldTarget) < 0.001f) Fold = FoldTarget;
	for (const FFoldEntry& E : FoldList)
	{
		if (E.bSolved) SetQ(E.Bone, FQuat::Slerp(E.FromQ, E.FoldQ, Fold));
		else SetQ(E.Bone, B[E.Bone].RestQ * AxisQ(E.Axis, E.Amount * Fold));
	}
	FoldIK(Fold);
	return Fold;
}

void UDragonWebRigComponent::FoldIK(float K)
{
	if (K <= 0.001f) return;
	// Model frame: nose -Z, up +Y, .L on -X.
	auto Aim = [&](int32 Bone, int32 Child, const FVector& Dir)
	{
		if (Bone < 0 || Child < 0) return;
		const FVector P = Model(Bone).GetLocation();
		const FVector C = Model(Child).GetLocation();
		const double Len = FVector::Dist(P, C);
		const FQuat Was = B[Bone].Q;
		AimModel(Bone, C, P + Dir.GetSafeNormal() * Len);
		SetQ(Bone, FQuat::Slerp(Was, B[Bone].Q, K));
	};
	// How a bat-winged animal folds (and how he walks in the films): the upper arm lies back
	// along the flank, the forearm folds forward so the wrist rides just above the shoulder,
	// and the fingers sweep back AND DOWN the sides, each lower one further down, so the
	// membrane drapes over his flanks like a cloak - from behind a rounded A over the back,
	// not a flat shelf on top or a crossbar sticking out at the shoulders.
	auto P = [](const TCHAR* N, float V) { FParse::Value(FCommandLine::Get(), N, V); return V; };
	static const float ShOut = P(TEXT("-FoldShOut="), 0.05f);    // shoulder: how far out (0.95 made a crossbar at the shoulders)
	static const float ArmDown = P(TEXT("-FoldArmDown="), 0.12f); // humerus: down the flank
	static const float FDown0 = P(TEXT("-FoldDown0="), 0.10f);    // top finger: down
	static const float FDownK = P(TEXT("-FoldDownK="), 0.11f);    // each lower finger: further down
	static const float FOut0 = P(TEXT("-FoldOut0="), 0.14f);      // top finger: out (clears the back)
	static const float FOutK = P(TEXT("-FoldOutK="), 0.03f);      // each lower finger: further out (clears the flank)
	static const float FCurl = P(TEXT("-FoldCurl="), 0.15f);      // outer phalanx: curls further down, round the side
	static const float ShUp = P(TEXT("-FoldShUp="), 0.30f), ShBack = P(TEXT("-FoldShBack="), 0.8f);
	static const float ArmOut = P(TEXT("-FoldArmOut="), 0.06f);
	static const float ForeOut = P(TEXT("-FoldForeOut="), -0.02f), ForeUp = P(TEXT("-FoldForeUp="), 0.12f);
	for (int32 s = 0; s < 2; ++s)
	{
		const double X = s == 0 ? -1.0 : 1.0;   // outward
		// shoulder: up, a little out and back, so the arm clears the back without sticking out
		Aim(WShoulder[s], WUpper[s], FVector(X * ShOut, ShUp, ShBack));
		// humerus: back and down along the flank
		Aim(WUpper[s], WFore[s], FVector(X * ArmOut, -ArmDown, 1.0));
		// forearm: forward and up; the wrist ends just above the shoulder
		const int32 Hand0 = Finger[s][0];
		Aim(WFore[s], Hand0, FVector(X * ForeOut, ForeUp, -1.0));
		// fingers: back and down the side, fanned (d = 5 is the leading finger, on top)
		for (int32 d = 0; d < 6; ++d)
		{
			const double Rank = 5 - d;   // 0 for the top finger
			for (int32 g = 0; g < 2; ++g)
			{
				const int32 Bn = Finger[s][d * 3 + g], Ch = Finger[s][d * 3 + g + 1];
				const double Dn = FDown0 + FDownK * Rank + FCurl * g;
				const double Ou = FOut0 + FOutK * Rank - 0.08 * g;
				Aim(Bn, Ch, FVector(X * Ou, -Dn, 1.0));
			}
			// the last joint has no child to aim at: bend it off its parent's line (+ lays it down; sign found by test shots), so the
			// tips lie over the hips instead of standing out behind him like a shelf
			const int32 Tip = Finger[s][d * 3 + 2];
			if (Tip >= 0 && B[Tip].Parent >= 0)
			{
				const FVector Pt = Model(Tip).GetLocation();
				const FVector Along = (Pt - Model(B[Tip].Parent).GetLocation()).GetSafeNormal();
				static const float TipBend = [] { float V = 0.55f; FParse::Value(FCommandLine::Get(), TEXT("-FoldTip="), V); return V; }();
				const FVector Want = (Along + FVector(X * 0.05, TipBend, 0.0)).GetSafeNormal();
				const FQuat Was = B[Tip].Q;
				AimModel(Tip, Pt + Along, Pt + Want);
				SetQ(Tip, FQuat::Slerp(Was, B[Tip].Q, K));
			}
		}
	}
}

void UDragonWebRigComponent::ApplyStow()
{
	// BONES_HD.md: past half-fold the membrane is put away rather than folded,
	// and Wing_Shoulder collapses so the cut edge pulls in against his flank.
	const bool bStow = bStowWingsPastHalfFold && Fold > StowFrom;
	SetMembraneShown(!bStow);
	if (!bStowWingsPastHalfFold) return;
	const float K = Smooth(Fold, StowFrom, 1.f);
	if (K <= 0.f) return;
	for (int32 s = 0; s < 2; ++s)
	{
		if (WShoulder[s] < 0) continue;
		B[WShoulder[s]].S = B[WShoulder[s]].RestS * Lerp(1.f, StowShoulderScale, K);
		bModelDirty = true;
	}
}

void UDragonWebRigComponent::SetMembraneShown(bool bShow)
{
	if (bShow == bMembraneShown || !Mesh || MembraneMaterial == INDEX_NONE) { bMembraneShown = bShow; return; }
	bMembraneShown = bShow;
	USkinnedAsset* Asset = Mesh->GetSkinnedAsset();
	FSkeletalMeshRenderData* RD = Asset ? Asset->GetResourceForRendering() : nullptr;
	if (!RD) return;
	for (int32 Lod = 0; Lod < RD->LODRenderData.Num(); ++Lod)
	{
		const FSkeletalMeshLODInfo* Info = Asset->GetLODInfo(Lod);
		const TArray<FSkelMeshRenderSection>& Sections = RD->LODRenderData[Lod].RenderSections;
		for (int32 s = 0; s < Sections.Num(); ++s)
		{
			int32 Mat = Sections[s].MaterialIndex;
			if (Info && Info->LODMaterialMap.IsValidIndex(s) && Info->LODMaterialMap[s] != INDEX_NONE) Mat = Info->LODMaterialMap[s];
			if (Mat == MembraneMaterial) Mesh->ShowMaterialSection(MembraneMaterial, s, bShow, Lod);
		}
	}
}

// ===========================================================================
// wings.js
// ===========================================================================
void UDragonWebRigComponent::UpdateWings(float Dt, const FDragonWebRigInput& In)
{
	const FDragonFlightState& St = In.FS;
	const float Climb = St.Climb, SpeedT = St.SpeedT, Knife = FMath::Abs(St.Knife);
	const float Hang = St.Hang, Roll = FMath::Abs(St.Roll);
	const float SlowK = 1.f - Smooth(SpeedT, 0.04f, 0.32f);
	const float FastK = Smooth(SpeedT, 0.4f, 0.85f);
	const float ClimbK = FMath::Max(0.f, Climb), DiveK = FMath::Max(0.f, -Climb);

	// Flap-glide: count beats as the phase wraps; after a few, hold the wings.
	const bool bCanGlide = SlowK < 0.25f && Climb < 0.15f && !In.bBoost && Roll < 0.1f && Hang < 0.1f;
	if (WP < LastP)
	{
		if (!bGliding && bCanGlide && --BeatsLeft <= 0)
		{
			bGliding = true;
			GlideLeft = 1.2f + FMath::FRand() * 1.6f + FastK * 1.2f;
		}
	}
	LastP = WP;
	if (bGliding)
	{
		GlideLeft -= Dt;
		if (GlideLeft <= 0.f || !bCanGlide)
		{
			bGliding = false;
			BeatsLeft = 3 + FMath::FloorToInt(FMath::FRand() * 3.f);
		}
	}
	GlideK += ((bGliding ? 1.f : 0.f) - GlideK) * Damp(bGliding ? 2.2f : 5.f, Dt);

	float TargetAmp = Lerp(Lerp(A_CRUISE, A_FAST, FastK), A_HOVER, SlowK) + A_CLIMB * ClimbK;
	TargetAmp *= (1.f - DiveK * 0.85f) * (1.f - Knife * 0.6f) * (1.f - Hang * 0.95f) * (1.f - Roll * 0.95f) * (1.f - GlideK);
	TargetAmp = FMath::Min(A_MAX, TargetAmp);
	const float TargetBias = Lerp(BIAS_CRUISE, BIAS_HOVER, SlowK) * (1.f - GlideK) + 0.07f * GlideK;
	const float TargetSweep = FMath::Clamp(DiveK * 0.65f + SpeedT * 0.5f + Knife * 0.6f + Roll * 0.95f, 0.f, 1.f) * (1.f - SlowK * 0.6f);

	const float AmpRate = (TargetAmp > WAmp ? 4.5f : 2.2f) + Roll * 14.f;
	WAmp += (TargetAmp - WAmp) * Damp(AmpRate, Dt);
	WBias += (TargetBias - WBias) * Damp(3.f, Dt);
	WSweep += (TargetSweep - WSweep) * Damp(3.2f + Roll * 14.f, Dt);
	WDown += (Lerp(DOWN_CRUISE, DOWN_SLOW, FMath::Max(SlowK, ClimbK)) - WDown) * Damp(3.f, Dt);

	const float Hz = (Lerp(Lerp(F_CRUISE, F_FAST, FastK), F_HOVER, SlowK) + F_CLIMB * ClimbK)
		* FlapSpeed * (1.f - Hang * 0.85f) * (1.f - Roll * 0.7f) * (1.f - GlideK * 0.75f);
	WP = Mod1(WP + FMath::Max(0.12f, Hz) * Dt);

	const float E = Wave(WP, WDown);
	const float Elev = (WBias + E * WAmp) * FlapAmplitude;
	const float SweepA = WSweep * SweepAmount * MAX_SWEEP * TuckSign;
	if (Clav[0] >= 0) SetQ(Clav[0], B[Clav[0]].RestQ * AxisQ(AX, Elev) * AxisQ(AZ, SweepA));
	const float ER = Wave(Mod1(WP + 0.012f), WDown);
	if (Clav[1] >= 0) SetQ(Clav[1], B[Clav[1]].RestQ * AxisQ(AX, (WBias + ER * WAmp) * FlapAmplitude) * AxisQ(AZ, -SweepA));

	const float Hk = Lerp(HEAVE_CRUISE, HEAVE_HOVER, SlowK) * (WAmp / A_HOVER);
	WHeave = -E * Hk;
}

UDragonWebRigComponent::FBeat UDragonWebRigComponent::GetBeat() const
{
	const float K = WAmp / A_HOVER;
	const float E0 = Wave(WP, WDown);
	const float E1 = Wave(Mod1(WP - HAND_LAG), WDown);
	const float E2 = Wave(Mod1(WP - TIP_LAG), WDown);
	const float A0 = E0 * WAmp, A1 = E1 * WAmp * HAND_AMP, A2 = E2 * WAmp * TIP_AMP;
	const float U = Mod1(WP - (WDown - FOLD_LEAD) + 1.f) / (1.f - WDown + FOLD_LEAD - FOLD_OPEN);
	const float FoldU = (U >= 0.f && U <= 1.f) ? FMath::Pow(FMath::Sin(PI * U), 1.1f) : 0.f;
	const float Cup = WP < WDown ? FMath::Sin(PI * WP / WDown) : 0.f;
	FBeat R;
	R.Phase = PhaseRad(WP, WDown);
	R.Amp = WAmp / A_CRUISE * 0.6f;
	R.Glide = GlideK;
	R.Outer = A1 - A0;
	R.Tip = A2 - A1;
	R.Fold = FoldU * FMath::Min(1.2f, K * 1.1f);
	R.Cup = Cup * K;
	R.Size = K;
	return R;
}

// ===========================================================================
// flightrig.js
// ===========================================================================
void UDragonWebRigComponent::UpdateFlightRig(float Dt, const FDragonFlightState& St, const FBeat& Beat)
{
	const float Phase = Beat.Phase;

	STurn += (St.Turn - STurn) * Damp(4.0f, Dt);
	SClimb += (St.Climb - SClimb) * Damp(3.0f, Dt);
	SSpeed += (St.SpeedT - SSpeed) * Damp(2.5f, Dt);
	const float Knife = St.Knife;
	SHang += (St.Hang - SHang) * Damp(3.6f, Dt);
	const float WantLoad = FMath::Min(LOAD_MAX, St.Load) / LOAD_MAX;
	SLoad += (WantLoad - SLoad) * Damp(WantLoad > SLoad ? 9.f : 3.5f, Dt);
	SRoll += (St.Roll - SRoll) * Damp(14.f, Dt);
	const float RollMag = FMath::Abs(SRoll);

	// --- Legs: tucked, harder the faster he goes and in a roll ---
	const float Tuck = (1.f - LEG_LOOSE * (1.f - SSpeed)) * (1.f + ROLL_LEG * RollMag);
	for (int32 s = 0; s < 2; ++s)
	{
		const float Paddle = FMath::Sin(TimeS * 2.2f + (s == 0 ? 0.f : 1.1f)) * 0.05f;   // not from the beat phase: a fraction of it jumps every wrap
		SetE(Thigh[s], AX, TUCK_THIGH * Tuck + Paddle);
		SetE(Shin[s], AX, TUCK_SHIN * Tuck);
		SetE(Ankle[s], AX, TUCK_ANKLE * Tuck);
		SetE(UpperArm[s], AX, TUCK_UPPER * Tuck - Paddle);
		SetE(Forearm[s], AX, TUCK_FORE * Tuck);
		SetE(Wrist[s], AX, TUCK_WRIST * Tuck);
	}

	// --- Wing shape ---
	const float Outer = Beat.Outer, TipBend = Beat.Tip, FoldB = Beat.Fold, Cup = Beat.Cup;
	const float Working = (1.f - SHang) * (1.f - RollMag * 0.85f);
	const float Camber = CAMBER * (0.35f + 0.65f * Cup) * (1.f - SSpeed * 0.5f);
	for (int32 s = 0; s < 2; ++s)
	{
		const float Sign = s == 0 ? 1.f : -1.f;
		const float RollSide = SRoll * Sign;
		SetE(WFore[s], AZ, (WRIST_FLEX * FoldB * Working + HANG_WRIST * SHang + RollMag * ROLL_CLOSE + FMath::Max(0.f, RollSide) * ROLL_TUCK) * Sign);
		AddE(WFore[s], AX, Outer * Working);
		SetE(WUpper[s], AZ, UPPER_FOLD * FoldB * Working * Sign);

		for (int32 d = 0; d < 6; ++d)
		{
			const float K = d / 5.f;
			const float Whip = -TipBend * (0.25f + 0.75f * K) * Working;
			const float BeatCurl = (Camber + WASHOUT * K * Cup) * Working;
			const float HangCurl = (HANG_CAMBER + HANG_WASHOUT * K * K) * SHang;
			const float Asym = 1.f - LOAD_ASYM * STurn * Sign;
			const float Bow = (LOAD_CAMBER + LOAD_TIP * K * K) * SLoad * Asym * (1.f - SHang);
			const float Twist = -RollSide * (ROLL_TWIST + ROLL_TIP * K * K);
			const float Close = RollMag * (ROLL_CURL + ROLL_CURL_TIP * K * K);
			const float Curl = Whip + BeatCurl + HangCurl + Bow + Twist + Close;
			const float RollSweep = RollMag * ROLL_RAKE * (0.4f + K)
				+ FMath::Max(0.f, RollSide) * ROLL_TUCK * (0.4f + K)
				- FMath::Max(0.f, -RollSide) * ROLL_REACH * K;
			const float Rake = SWEEP_DIGIT * SSpeed * K * (1.f - SHang) + HANG_SWEEP * K * SHang
				+ RollSweep + DIGIT_FOLD * FoldB * Working * (0.3f + K);
			for (int32 g = 0; g < 3; ++g)
			{
				const int32 N = Finger[s][d * 3 + g];
				SetE(N, AZ, Curl * (g == 0 ? 0.45f : g == 1 ? 0.33f : 0.22f) * Sign);
				if (g == 0) AddE(N, AX, Rake);
			}
		}
	}

	// --- Tail fins: a horizontal tailplane, never a hanging flap ---
	// The bind pose holds both fins flat out from the tail. In flight they stay in that plane:
	// they fan wider when slow (more area, more control) and sweep back at speed; both trail up
	// together to pitch him up, oppositely to roll and turn (elevons); and they flutter a little in the airflow.
	// Rotations are about the tail's own up and side axes, so they follow the tail as it bends.
	{
		static const float FanTest = [] { float V = 1.f; FParse::Value(FCommandLine::Get(), TEXT("-FinFan="), V); return V; }();
		const float FanOpen = FanTest * (FMath::Lerp(0.45f, 0.10f, FMath::Clamp(SSpeed * 1.5f, 0.f, 1.f)) + 0.15f * SHang - FMath::Abs(Knife) * 0.15f);
		const float ElevUp = 0.32f * SClimb;
		const float Diff = 0.28f * STurn + 0.30f * SRoll;
		for (int32 s = 0; s < 2; ++s)
		{
			const float Sign = s == 0 ? 1.f : -1.f;   // .L sits on -X
			for (int32 i = 0; i < 6; ++i)
			{
				const int32 St_ = FinStrut[s][i];
				if (St_ < 0 || B[St_].Parent < 0) continue;
				const float K = i / 5.f;
				const FQuat ParRest = RestModel[B[St_].Parent].GetRotation();
				const FVector UpP = ParRest.Inverse().RotateVector(FVector(0, 1, 0));
				const FVector SideP = ParRest.Inverse().RotateVector(FVector(1, 0, 0));
				const float Flutter = FMath::Sin(TimeS * 13.f + i * 0.9f + s * 2.1f) * 0.025f * (0.3f + SSpeed);
				const float Pitch = ElevUp + Diff * Sign + Flutter;          // + = trailing edge up
				const float Fan = FanOpen * (0.35f + 0.65f * K);
				SetQ(St_, FQuat(UpP, -Fan * Sign) * FQuat(SideP, -Pitch * (0.5f + 0.5f * K)) * B[St_].RestQ);
			}
		}
	}

	// --- Tail ---
	// A long tail is not a rod: the body's heave on each beat runs down it as a travelling wave that
	// grows toward the tip, it sags a little when slow, curves into turns, and drifts slightly side to side.
	{
		if (TailDownSign == 0.f && TailB[0] >= 0 && TailB[10] >= 0)
		{
			// which way +X on the tail bones turns the tip down (model up is +Y)
			const FQuat Saved = B[TailB[0]].Q;
			const double Y0 = Model(TailB[10]).GetLocation().Y;
			RotateLocal(TailB[0], AX, 0.1f);
			const double Y1 = Model(TailB[10]).GetLocation().Y;
			SetQ(TailB[0], Saved);
			TailDownSign = Y1 < Y0 ? 1.f : -1.f;
		}
		const float BeatAmp = FMath::Clamp(WAmp / A_CRUISE, 0.f, 1.6f) * (1.f - GlideK * 0.7f);
		for (int32 i = 1; i <= 11; ++i)
		{
			const float K = i / 11.f;
			SetE(TailB[i - 1], AZ, (TAIL_CARVE * 1.6f * STurn + ROLL_TAIL * SRoll) * (0.35f + K)
				+ FMath::Sin(TimeS * 1.1f - K * 2.0f) * 0.006f);
			AddE(TailB[i - 1], AX, FMath::Sin(Phase - 0.8f - K * 2.6f) * (0.010f + 0.022f * K) * BeatAmp
				+ SClimb * 0.02f + TailDownSign * 0.012f * (1.f - SSpeed) * (1.f - K));
		}
	}

	// --- Head, neck, crest, ears ---
	for (int32 i = 0; i < 3; ++i) SetE(NeckB[i], AZ, (HEAD_LOOK * STurn + ROLL_HEAD * SRoll) * NECK_SHARE);
	SetE(HeadB, AZ, HEAD_LOOK * STurn + ROLL_HEAD * SRoll);
	AddE(HeadB, AX, -HEAD_PITCH * SClimb);
	SetE(Jaw, AX, 0.f);

	const float Flat = CREST_FLAT * SSpeed;
	for (int32 i = 0; i < 8; ++i) SetE(Dorsal[i], AX, Flat);
	for (int32 s = 0; s < 2; ++s)
	{
		const float Back = EAR_SWEEP * SSpeed * (s == 0 ? 1.f : -1.f);
		SetE(EarA1[s], AZ, Back);
		SetE(EarA2[s], AZ, Back * 0.6f);
		SetE(EarB1[s], AZ, Back * 0.7f);
		SetE(EarB2[s], AZ, Back * 0.4f);
		SetE(EarC[s], AZ, Back * 0.5f);
	}

	const float Blink = FMath::Max(0.f, FMath::Sin(TimeS * 0.9f) - 0.965f) * 9.f;
	SetE(Eye[0], AX, FMath::Clamp(Blink, 0.f, 1.f) * 0.35f);
	SetE(Eye[1], AX, FMath::Clamp(Blink, 0.f, 1.f) * 0.35f);

	// --- Head held steady through the beat ---
	// The body heaves up and down under each wingbeat; like a bird or a bat, he lets the neck take
	// most of it so the head (and his eyes) keep a smooth line.
	if (NeckB[0] >= 0 && HeadB >= 0)
	{
		if (NeckUpSign == 0.f)
		{
			const FQuat Saved = B[NeckB[0]].Q;
			const double Y0 = Model(HeadB).GetLocation().Y;
			RotateLocal(NeckB[0], AX, 0.1f);
			const double Y1 = Model(HeadB).GetLocation().Y;
			SetQ(NeckB[0], Saved);
			NeckUpSign = Y1 > Y0 ? 1.f : -1.f;
			const float WorldScale = FMath::Max(1e-3f, (float)ModelDirToWeb(FVector(0, 1, 0)).Size());
			NeckLenM = FMath::Max(0.3f, (float)FVector::Dist(Model(NeckB[0]).GetLocation(), Model(HeadB).GetLocation()) * WorldScale);
		}
		static const float Steady = [] { float V = 0.8f; FParse::Value(FCommandLine::Get(), TEXT("-HeadSteady="), V); return V; }();
		const float Counter = FMath::Clamp(-WHeave / NeckLenM, -0.15f, 0.15f) * Steady * NeckUpSign;
		AddE(NeckB[0], AX, Counter * 0.6f);
		if (NeckB[1] >= 0) AddE(NeckB[1], AX, Counter * 0.4f);
	}
}

// ===========================================================================
// aim.js (applyToRig)
// ===========================================================================
void UDragonWebRigComponent::ApplyAim(float Dt, const FDragonWebRigInput& In)
{
	const float WantYaw = In.bAimActive ? In.AimYaw : 0.f;
	const float WantPitch = In.bAimActive ? In.AimPitch : 0.f;
	const float K = Damp(AIM_HEAD_RATE, Dt);
	AimYaw += (WantYaw - AimYaw) * K;
	AimPitch += (WantPitch - AimPitch) * K;
	if (In.bGrounded) return; // main.js applies it in the flight block only
	for (int32 i = 0; i < 3; ++i)
	{
		AddE(NeckB[i], AZ, AimYaw * AIM_NECK_SHARE);
		AddE(NeckB[i], AX, -AimPitch * AIM_NECK_SHARE);
	}
	AddE(HeadB, AZ, AimYaw * AIM_HEAD_SHARE);
	AddE(HeadB, AX, -AimPitch * AIM_HEAD_SHARE);
}

// ===========================================================================
// dragonrig.js update() — the ground
// ===========================================================================
void UDragonWebRigComponent::UpdateGround(float Dt, const FDragonWebRigInput& In)
{
	const float Speed = FMath::Abs(In.GroundSpeed);
	const float MaxSpeed = In.MaxGroundSpeed > 0.f ? In.MaxGroundSpeed : 1.9f;
	const float T = FMath::Clamp(Speed / MaxSpeed, 0.f, 1.f);
	MoveAmt += (T - MoveAmt) * Damp(7.f, Dt);
	GPhase += Speed * STEPS_PER_M * PI * 2.f * Dt;
	GPhase = FMath::Fmod(GPhase, 2000.f * PI); // float precision; every use is periodic in a multiple of 2pi

	const float Jostle = MoveAmt > 0.01f ? FMath::Sin(GPhase * 2.f) * 0.05f * MoveAmt : 0.f;
	ApplyFold(Dt);
	for (int32 s = 0; s < 2; ++s)
	{
		AddE(WShoulder[s], AX, Jostle);
		AddE(WUpper[s], AX, Jostle * 0.5f);
	}

	if (In.GroundAtM)
	{
		if (!bGaitTried) { bGaitTried = true; bGaitOK = CreateGait(); }
		if (bGaitOK) { UpdateGait(Dt, In); return; }
	}
	UpdateSineLegs(Dt);
}

void UDragonWebRigComponent::UpdateSineLegs(float Dt)
{
	struct FG { int32 Part, Flex, Ank, ToeI; const int32* Digits; float Off, Amp; };
	const FG Gait[4] = {
		{ UpperArm[0], Forearm[0], Wrist[0], FrontToe[0], FrontDigit[0], 0.f, STRIDE_FRONT },
		{ UpperArm[1], Forearm[1], Wrist[1], FrontToe[1], FrontDigit[1], PI, STRIDE_FRONT },
		{ Thigh[0], Shin[0], Ankle[0], Toe[0], HindDigit[0], PI, STRIDE_REAR },
		{ Thigh[1], Shin[1], Ankle[1], Toe[1], HindDigit[1], 0.f, STRIDE_REAR },
	};
	for (const FG& Gd : Gait)
	{
		const float Th = GPhase + Gd.Off;
		const float Swing = FMath::Sin(Th);
		const float Recovery = FMath::Max(0.f, -FMath::Cos(Th));
		const float Stance = 1.f - Recovery;
		const float HipA = Swing * Gd.Amp * MoveAmt;
		const float KneeA = -Recovery * KNEE_BEND * MoveAmt;
		SetE(Gd.Part, AX, HipA);
		SetE(Gd.Flex, AX, KneeA);
		const float Level = -(HipA + KneeA) * FOOT_LEVEL * Stance;
		const float Lift = Recovery * FOOT_LIFT * MoveAmt;
		SetE(Gd.Ank, AX, Level + Lift);
		const float Push = FMath::Max(0.f, FMath::Sin(Th - 2.35f)) * FOOT_PUSH * MoveAmt;
		SetE(Gd.ToeI, AX, Push - Recovery * TOE_CURL * MoveAmt);
		const float Load = FMath::Max(0.f, FMath::Cos(Th)) * MoveAmt;
		for (int32 i = 0; i < 3; ++i)
		{
			SetE(Gd.Digits[i], AZ, (i - 1) * TOE_SPLAY * Load);
			AddE(Gd.Digits[i], AX, Push * 0.5f - Lift * 0.5f);
		}
	}

	const float Breath = FMath::Sin(GPhase * 0.16f + 1.3f) * 0.012f;
	SetE(SpineB[0], AX, Breath + FMath::Sin(GPhase * 2.f) * 0.02f * MoveAmt);
	AddE(SpineB[0], AZ, FMath::Sin(GPhase) * 0.035f * MoveAmt);
	for (int32 i = 0; i < 3; ++i) SetE(NeckB[i], AX, -FMath::Sin(GPhase * 2.f) * 0.012f * MoveAmt);
	SetE(HeadB, AX, -FMath::Sin(GPhase * 2.f) * 0.02f * MoveAmt);
	AddE(HeadB, AY, FMath::Sin(GPhase * 0.31f) * 0.05f);
	for (int32 i = 0; i < 11; ++i)
	{
		const float K = i / 11.f;
		SetE(TailB[i], AY, FMath::Sin(GPhase * 0.7f - K * 2.2f) * (0.02f + 0.05f * MoveAmt));
		AddE(TailB[i], AX, FMath::Sin(GPhase * 0.45f - K * 1.6f) * 0.012f);
		if (TailCurl != 0.f) AddE(TailB[i], AY, TailCurl / 11.f);
	}
}

// ===========================================================================
// gait.js
// ===========================================================================
bool UDragonWebRigComponent::CreateGait()
{
	const int32 Ids[4][5] = {
		{ UpperArm[0], Forearm[0], Wrist[0], FrontToe[0], FrontDigit[0][1] >= 0 ? FrontDigit[0][1] : FrontDigit[0][0] },
		{ UpperArm[1], Forearm[1], Wrist[1], FrontToe[1], FrontDigit[1][1] >= 0 ? FrontDigit[1][1] : FrontDigit[1][0] },
		{ Thigh[0], Shin[0], Ankle[0], Toe[0], HindDigit[0][1] >= 0 ? HindDigit[0][1] : HindDigit[0][0] },
		{ Thigh[1], Shin[1], Ankle[1], Toe[1], HindDigit[1][1] >= 0 ? HindDigit[1][1] : HindDigit[1][0] },
	};
	for (int32 i = 0; i < 4; ++i)
		for (int32 j = 0; j < 4; ++j)
			if (Ids[i][j] < 0) return false;

	// Measured on the bind pose: lengths and heights in world units, directions
	// in the model's own frame so they turn with him.
	const FVector RootW = ModelToWeb(FVector::ZeroVector);
	auto W = [&](int32 I) { return ModelToWeb(RestModel[I].GetLocation()); };
	auto Lc = [&](int32 I) { return RestModel[I].GetLocation(); };
	for (int32 i = 0; i < 4; ++i)
	{
		FGaitLeg& L = Legs[i];
		L = FGaitLeg();
		L.A = Ids[i][0]; L.B = Ids[i][1]; L.C = Ids[i][2]; L.D = Ids[i][3]; L.E = Ids[i][4];
		L.bFore = i < 2;
		const FVector Aw = W(L.A), Bw = W(L.B), Cw = W(L.C), Dw = W(L.D);
		L.L1 = FVector::Dist(Aw, Bw);
		L.L2 = FVector::Dist(Bw, Cw);
		L.L3 = FVector::Dist(Cw, Dw);
		L.ToeLen = L.E >= 0 ? FVector::Dist(Dw, W(L.E)) : 0.12f;
		L.ToeH = Dw.Y - RootW.Y;
		if (L.E >= 0)
		{
			// the toes' own rest angle below level (nose is -Z, up +Y): they reach forward and down to the claws
			const FVector Dv = Lc(L.E) - Lc(L.D);
			L.ToePitch0 = FMath::Atan2(-Dv.Y, FMath::Max(-Dv.Z, 1e-3));
			L.TipH = W(L.E).Y - RootW.Y;
		}
		const FVector A = Lc(L.A), Bv = Lc(L.B), C = Lc(L.C), D = Lc(L.D);
		L.Home = D;
		const FVector Axis = (C - A).GetSafeNormal();
		FVector Kb = Bv - A;
		L.Pole = (Kb - Axis * FVector::DotProduct(Kb, Axis)).GetSafeNormal();
		const FVector Md = C - D;
		L.Meta0 = FMath::Atan2(Md.Z * 1.0, Md.Y); // TAILWARD_SIGN = +1: his nose is toward -Z
	}
	UE_LOG(LogDragon, Log, TEXT("WebRig toes: pitch FL %.2f HL %.2f, tip h %.3f %.3f, toe h %.3f %.3f"), Legs[0].ToePitch0, Legs[2].ToePitch0, Legs[0].TipH, Legs[2].TipH, Legs[0].ToeH, Legs[2].ToeH);
	HipH = (W(Legs[2].A).Y + W(Legs[3].A).Y) / 2.0 - RootW.Y;
	HipsRestPos = Hips >= 0 ? B[Hips].RestT : FVector::ZeroVector;
	bGaitFirst = true;
	UE_LOG(LogDragon, Log, TEXT("WebRig gait: hip %.2f m, legs FL %.2f FR %.2f HL %.2f HR %.2f, toe %.2f"), HipH,
		Legs[0].L1 + Legs[0].L2 + Legs[0].L3, Legs[1].L1 + Legs[1].L2 + Legs[1].L3,
		Legs[2].L1 + Legs[2].L2 + Legs[2].L3, Legs[3].L1 + Legs[3].L2 + Legs[3].L3, Legs[0].ToeH);
	return HipH > 0.05f;
}

void UDragonWebRigComponent::GaitAim(int32 Bone, const FVector& ChildW, const FVector& TargetW)
{
	// Turn `Bone` about its own origin so its child at ChildW would sit at TargetW.
	// Done in model space: the same rotation, without the world's handedness in it.
	if (Bone < 0) return;
	const FTransform& M = Model(Bone);
	const FVector P = M.GetLocation();
	const FQuat BoneQ = M.GetRotation();
	FVector U = WebToModel(ChildW) - P, V = WebToModel(TargetW) - P;
	if (U.SizeSquared() < 1e-10 || V.SizeSquared() < 1e-10) return;
	U.Normalize(); V.Normalize();
	const FQuat Q = FQuat::FindBetweenNormals(U, V);
	const FQuat NewM = Q * BoneQ;
	const int32 Par = B[Bone].Parent;
	const FQuat ParentQ = Par >= 0 ? Model(Par).GetRotation() : FQuat::Identity;
	SetQ(Bone, ParentQ.Inverse() * NewM);
}

void UDragonWebRigComponent::AimModel(int32 Bone, const FVector& ChildM, const FVector& TargetM)
{
	if (Bone < 0) return;
	const FTransform& M = Model(Bone);
	const FVector P = M.GetLocation();
	FVector U = ChildM - P, V = TargetM - P;
	if (U.SizeSquared() < 1e-12 || V.SizeSquared() < 1e-12) return;
	U.Normalize(); V.Normalize();
	const FQuat NewM = FQuat::FindBetweenNormals(U, V) * M.GetRotation();
	const int32 Par = B[Bone].Parent;
	const FQuat ParentQ = Par >= 0 ? Model(Par).GetRotation() : FQuat::Identity;
	SetQ(Bone, ParentQ.Inverse() * NewM);
}

void UDragonWebRigComponent::FlightLegs(float K, float Ready, float Dt)
{
	if (!bGaitTried) { bGaitTried = true; bGaitOK = CreateGait(); }
	if (!bGaitOK || K <= 0.001f) return;
	// The model's own frame: nose toward -Z, up +Y (glTF).
	const FVector Fwd(0, 0, -1), Up(0, 1, 0);
	const FVector Back = -Fwd, Down = -Up;
	for (FGaitLeg& L : Legs)
	{
		const FVector A = Model(L.A).GetLocation();
		const double L1 = FVector::Dist(Model(L.A).GetLocation(), Model(L.B).GetLocation());
		const double L2 = FVector::Dist(Model(L.B).GetLocation(), Model(L.C).GetLocation());
		const double L3 = FVector::Dist(Model(L.C).GetLocation(), Model(L.D).GetLocation());
		const double Ls = L1 + L2;
		const double ToeL = L.E >= 0 ? FVector::Dist(Model(L.D).GetLocation(), Model(L.E).GetLocation()) : L3 * 0.4;
		const FVector Side = FVector(FMath::Sign(A.X) * 1.0, 0, 0);   // keep the feet a little in toward the body line
		FVector Ct, Dt3, Et, Pole;
		if (!L.bFore)
		{
			// Hind: thigh back along the belly (hip extended ~50 deg), knee eased, ankle opened so
			// the foot trails toward the tail with the toes relaxed. Ready: swing forward and down.
			const FVector CruiseC = A + Back * (0.78 * Ls) + Down * (0.34 * Ls) - Side * (0.04 * Ls);
			const FVector ReadyC = A + Back * (0.05 * Ls) + Down * (0.92 * Ls);
			Ct = FMath::Lerp(CruiseC, ReadyC, (double)Ready);
			Dt3 = Ct + FMath::Lerp(Back * (0.92 * L3) + Down * (0.30 * L3), Fwd * (0.25 * L3) + Down * (0.95 * L3), (double)Ready);
			Et = Dt3 + FMath::Lerp(Back * (0.75 * ToeL) + Down * (0.55 * ToeL), Fwd * (0.95 * ToeL) + Down * (0.2 * ToeL), (double)Ready);
			Pole = FMath::Lerp(Down, Fwd, (double)Ready);
		}
		else
		{
			// Fore: elbow drawn back against the ribs, forearm forward and down under the chest,
			// wrist flexed so the paw curls back toward the sternum. Ready: reach down and forward.
			const FVector CruiseC = A + Down * (0.50 * Ls) + Fwd * (0.06 * Ls) - Side * (0.05 * Ls);
			const FVector ReadyC = A + Down * (0.88 * Ls) + Fwd * (0.25 * Ls);
			Ct = FMath::Lerp(CruiseC, ReadyC, (double)Ready);
			Dt3 = Ct + FMath::Lerp(Back * (0.75 * L3) + Up * (0.55 * L3), Fwd * (0.30 * L3) + Down * (0.95 * L3), (double)Ready);
			Et = Dt3 + FMath::Lerp(Back * (0.55 * ToeL) + Up * (0.75 * ToeL), Fwd * (0.95 * ToeL) + Down * (0.15 * ToeL), (double)Ready);
			Pole = FMath::Lerp(Back + Up * 0.3, Back, (double)Ready);
		}
		// Save the current local rotations, solve, then blend toward the solution by K.
		const int32 Ch[4] = { L.A, L.B, L.C, L.D };
		FQuat Was[4];
		for (int32 j = 0; j < 4; ++j) Was[j] = Ch[j] >= 0 ? B[Ch[j]].Q : FQuat::Identity;
		FVector ToT = Ct - A;
		double D = FMath::Clamp(ToT.Size(), FMath::Abs(L1 - L2) + 1e-3, Ls * 0.995);
		const FVector Axis = ToT.GetSafeNormal();
		const FVector Cc = A + Axis * D;
		const double Along = (L1 * L1 - L2 * L2 + D * D) / (2 * D);
		const double H = FMath::Sqrt(FMath::Max(0.0, L1 * L1 - Along * Along));
		FVector Pl = Pole - Axis * FVector::DotProduct(Pole, Axis);
		if (Pl.SizeSquared() < 1e-8) Pl = Down; Pl.Normalize();
		const FVector Knee = A + Axis * Along + Pl * H;
		AimModel(L.A, Model(L.B).GetLocation(), Knee);
		AimModel(L.B, Model(L.C).GetLocation(), Cc);
		AimModel(L.C, Model(L.D).GetLocation(), Dt3);
		if (L.E >= 0)
		{
			// Toes keep their own bend against the foot (a fixed aim crushed the claws into the paw),
			// eased a third straighter when tucked so the paw trails pointed and relaxed.
			auto AngX = [](const FVector& V) { return FMath::Atan2(V.Y, V.Z); };   // angle in the side (YZ) plane
			const FVector RC = RestModel[L.C].GetLocation(), RD = RestModel[L.D].GetLocation(), RE = RestModel[L.E].GetLocation();
			const float Bend0 = FMath::UnwindRadians(AngX(RE - RD) - AngX(RD - RC));
			const FVector Meta = Model(L.D).GetLocation() - Model(L.C).GetLocation();
			const float TA = AngX(Meta) + Bend0 * (1.f - 0.35f * (1.f - Ready));
			const FVector Dir(0.0, FMath::Sin(TA), FMath::Cos(TA));
			AimModel(L.D, Model(L.E).GetLocation(), Model(L.D).GetLocation() + Dir * ToeL);
			(void)Et;
		}
		for (int32 j = 0; j < 4; ++j)
			if (Ch[j] >= 0) SetQ(Ch[j], FQuat::Slerp(Was[j], B[Ch[j]].Q, K));
		static int32 Dumped = 0;
		if (Dumped < 4 && FParse::Param(FCommandLine::Get(), TEXT("RigDebug")))
		{
			++Dumped;
			FString Sx = FString::Printf(TEXT("leg fore=%d A=%s B=%s C=%s D=%s | wantC=%s wantD=%s knee=%s | L1 %.3f L2 %.3f L3 %.3f\n"), L.bFore ? 1 : 0,
				*Model(L.A).GetLocation().ToString(), *Model(L.B).GetLocation().ToString(), *Model(L.C).GetLocation().ToString(), *Model(L.D).GetLocation().ToString(),
				*Ct.ToString(), *Dt3.ToString(), *Knee.ToString(), L1, L2, L3);
			if (Dumped == 1)
			{
				const int32 HeadI = HeadB, TailI = TailB[10];
				Sx = FString::Printf(TEXT("head %s tail %s nose(web) %s up(web) %s\n"), HeadI >= 0 ? *Model(HeadI).GetLocation().ToString() : TEXT("-"),
					TailI >= 0 ? *Model(TailI).GetLocation().ToString() : TEXT("-"), *ModelDirToWeb(FVector(0, 0, -1)).ToString(), *ModelDirToWeb(FVector(0, 1, 0)).ToString()) + Sx;
			}
			FFileHelper::SaveStringToFile(Sx, *(FPaths::ProjectSavedDir() / TEXT("RigDebug.txt")), FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
		}
	}
}

void UDragonWebRigComponent::UpdateGait(float RawDt, const FDragonWebRigInput& In)
{
	static const bool bBindTest = FParse::Param(FCommandLine::Get(), TEXT("GaitBind"));
	if (bBindTest) return;   // test: leave the legs in the model's bind pose
	const float Dt = FMath::Min(RawDt, 0.05f);
	auto GroundAt = [&](double X, double Z) { return (double)In.GroundAtM((float)X, (float)Z); };
	auto WPos = [&](int32 I) { return ModelToWeb(Model(I).GetLocation()); };

	// --- Where is he going? Measured, not told ---
	const FVector Pos = ModelToWeb(FVector::ZeroVector);
	const FVector Fz = ModelDirToWeb(FVector(0, 0, 1)).GetSafeNormal();
	const float YawNow = FMath::Atan2(Fz.X, Fz.Z);
	if (bGaitFirst) { PrevPos = Pos; PrevYaw = YawNow; bGaitFirst = false; }
	FVector V = (Pos - PrevPos) / FMath::Max(RawDt, 1e-4f);
	V.Y = 0;
	GVel = FMath::Lerp(GVel, V, (double)(1.f - FMath::Exp(-14.f * Dt)));
	float Dy = YawNow - PrevYaw;
	Dy = FMath::Atan2(FMath::Sin(Dy), FMath::Cos(Dy));
	YawRate += (Dy / FMath::Max(RawDt, 1e-4f) - YawRate) * (1.f - FMath::Exp(-10.f * Dt));
	PrevPos = Pos; PrevYaw = YawNow;
	const float Speed = GVel.Size();

	const float Fr = (Speed * Speed) / (GRAV * HipH);
	const float ToTrot = Smooth(Fr, 0.32f, 0.62f), ToGallop = Smooth(Fr, 1.9f, 3.2f);
	const float WalkK = 1.f - ToTrot, TrotK = ToTrot * (1.f - ToGallop), GallopK = ToGallop;
	const float Duty = GWalk.Duty * WalkK + GTrot.Duty * TrotK + GGallop.Duty * GallopK;
	const float LiftH = (GWalk.Lift * WalkK + GTrot.Lift * TrotK + GGallop.Lift * GallopK) * HipH;
	float Offsets[4];
	for (int32 i = 0; i < 4; ++i) Offsets[i] = GWalk.Offsets[i] * WalkK + GTrot.Offsets[i] * TrotK + GGallop.Offsets[i] * GallopK;

	const float Turning = FMath::Abs(YawRate) * 1.4f;
	const float EffV = FMath::Max(Speed, Turning);
	const float Vv = FMath::Max(EffV, 0.6f);
	const float Hz = FMath::Sqrt(1.f / HipH) * ((0.75f + 0.42f * Vv) * WalkK + (1.55f + 0.16f * Vv) * TrotK + (2.3f + 0.07f * Vv) * GallopK);
	const float T = 1.f / Hz;

	// Legs, spine, neck, head and tail are at bind already (ResetPose).
	const bool bMoving = EffV > 0.12f;
	if (bMoving) bStill = false;
	else if (!bStill)
	{
		bool bSettled = true;
		for (const FGaitLeg& L : Legs)
		{
			const FVector Home = ModelToWeb(L.Home);
			if (!L.bDown || (L.bPlant && FMath::Sqrt(FMath::Square(L.Plant.X - Home.X) + FMath::Square(L.Plant.Z - Home.Z)) > 0.12)) bSettled = false;
		}
		if (bSettled) bStill = true;
	}
	if (!bStill) GPhase01 = FMath::Fmod(GPhase01 + Dt / T, 1.f);

	const float Amp = bStill ? 0.f : FMath::Clamp(EffV / 1.2f, 0.35f, 1.f);
	const float Ph = GPhase01 * PI * 2.f;
	const float Size = HipH;
	const float Bob = Size * (bStill ? 0.f :
		(-FMath::Cos(Ph * 2.f) * 0.012f * WalkK + -FMath::Cos(Ph * 2.f) * 0.03f * TrotK +
		 (FMath::Sin(Ph - 0.6f) * 0.05f + 0.045f) * GallopK) * Amp);
	const float Sway = bStill ? 0.f : FMath::Sin(Ph) * 0.035f * WalkK * Amp;
	const float Heave = bStill ? 0.f : FMath::Sin(Ph + 0.9f) * 0.03f * GallopK;
	const float WantLean = FMath::Clamp(FMath::Atan2(Speed * YawRate, GRAV), -0.38f, 0.38f);
	Lean += (WantLean - Lean) * (1.f - FMath::Exp(-6.f * Dt));

	const float WorldScale = FMath::Max(1e-3f, (float)ModelDirToWeb(FVector(0, 1, 0)).Size());
	if (Hips >= 0)
	{
		B[Hips].T = HipsRestPos;
		B[Hips].T.Y += Bob / WorldScale;
		bModelDirty = true;
	}
	const float Flex = bStill ? 0.f : FMath::Sin(Ph + 0.4f) * 0.14f * GallopK;
	const float FLEX_SPLIT[3] = { 1.0f, 0.25f, -0.85f };
	for (int32 i = 0; i < 3; ++i)
	{
		RotateLocal(SpineB[i], AX, Flex * FLEX_SPLIT[i] + (i == 0 ? Heave : 0.f));
		RotateLocal(SpineB[i], AY, FMath::Sin(Ph) * 0.05f * WalkK * Amp * (i == 1 ? 1.f : 0.5f) - YawRate * 0.05f);
		RotateLocal(SpineB[i], AZ, (Sway + Lean * 0.6f) * (i == 0 ? 1.f : 0.4f));
	}
	const float TimeMs = TimeS * 1000.f;
	const float Breath = FMath::Sin(TimeMs * 0.0011f) * 0.012f;
	RotateLocal(SpineB[1], AX, Breath * (bStill ? 1.f : 0.3f));

	// --- Feet ---
	FVector Fwd = ModelDirToWeb(FVector(0, 0, 1));
	Fwd.Y = 0; Fwd = Fwd.GetSafeNormal();
	const FVector Nose = -Fwd;
	const FVector UpW(0, 1, 0);

	for (int32 i = 0; i < 4; ++i)
	{
		FGaitLeg& L = Legs[i];
		const float P = bStill ? 0.f : Mod1(GPhase01 - Offsets[i]);
		const bool bDown = bStill || P < Duty;
		const FVector Home = ModelToWeb(L.Home);
		const FVector R = Home - Pos;
		const FVector FootVel = GVel + FVector(R.Z * YawRate, 0, -R.X * YawRate) * 0.6;
		const FVector Reach = FootVel * (Duty * T * 0.5f);
		FVector Target = Home + Reach;
		Target.Y = GroundAt(Target.X, Target.Z) + L.ToeH;

		if (!L.bPlant)
		{
			L.Plant = Home;
			L.Plant.Y = GroundAt(Home.X, Home.Z) + L.ToeH;
			L.bPlant = true;
		}
		if (bDown)
		{
			if (!L.bWasDown) L.Plant = Target;
			if (bStill) L.Plant.Y = GroundAt(L.Plant.X, L.Plant.Z) + L.ToeH;
			L.Foot = L.Plant;
		}
		else
		{
			if (L.bWasDown) L.LiftFrom = L.Plant;
			const float S = (P - Duty) / (1.f - Duty);
			const float K = S * S * (3.f - 2.f * S);
			L.Foot = FMath::Lerp(L.LiftFrom, Target, (double)K);
			L.Foot.Y += FMath::Sin(PI * FMath::Pow(S, 0.8f)) * LiftH * Amp;
		}
		L.P = P;
		L.S = bDown ? P / FMath::Max(Duty, 1e-3f) : (P - Duty) / (1.f - Duty);
		L.bWasDown = bDown;
		L.bDown = bDown;
	}

	// --- Solve the legs ---
	for (FGaitLeg& L : Legs)
	{
		float Meta;
		if (L.bDown)
		{
			const float S = bStill ? 0.4f : L.S;
			Meta = L.Meta0 + Lerp(0.18f, -0.55f, FMath::Pow(S, 1.6f)) * Amp * (L.bFore ? 0.9f : 1.f);
		}
		else
		{
			const float S = L.S;
			Meta = L.Meta0 + (L.bFore ? -0.85f : -0.7f) * FMath::Sin(PI * FMath::Min(1.f, S * 1.2f)) * Amp
				+ Lerp(-0.55f, 0.18f, S) * Amp * 0.6f;
		}
		const FVector Back = -Nose;
		const FVector C = L.Foot + UpW * (FMath::Cos(Meta) * L.L3) + Back * (FMath::Sin(Meta) * L.L3);

		const FVector A = WPos(L.A);
		FVector ToT = C - A;
		double D = ToT.Size();
		const double MaxD = (L.L1 + L.L2) * 0.999, MinD = FMath::Abs(L.L1 - L.L2) + 0.01;
		D = FMath::Clamp(D, MinD, MaxD);
		const FVector Axis = ToT.GetSafeNormal();
		const FVector Ct = A + Axis * D;
		const double Along = (L.L1 * L.L1 - L.L2 * L.L2 + D * D) / (2 * D);
		const double H = FMath::Sqrt(FMath::Max(0.0, L.L1 * L.L1 - Along * Along));
		FVector PoleW = ModelDirToWeb(L.Pole);
		PoleW = (PoleW - Axis * FVector::DotProduct(PoleW, Axis)).GetSafeNormal();
		const FVector Knee = A + Axis * Along + PoleW * H;

		GaitAim(L.A, WPos(L.B), Knee);
		GaitAim(L.B, WPos(L.C), Ct);
		GaitAim(L.C, WPos(L.D), L.Foot);
		static const bool bToeAim = !FParse::Param(FCommandLine::Get(), TEXT("NoToeAim"));
		if (L.E >= 0 && bToeAim)
		{
			// toes fold under as the paw leaves the ground, then open and reach just before it lands
			const float Curl = L.bDown ? 0.f : (0.38f * FMath::Sin(PI * FMath::Min(1.f, L.S * 1.5f))
				- (L.S > 0.6f ? 0.28f * FMath::Sin(PI * (L.S - 0.6f) / 0.4f) : 0.f)) * Amp;
			// keep the toes at their own rest angle to the ground; forcing them level folds the claws up into the paw
			const float Ang = L.ToePitch0 + Curl;
			FVector Tip = L.Foot + Nose * (FMath::Cos(Ang) * L.ToeLen) + UpW * (-FMath::Sin(Ang) * L.ToeLen);
			Tip.Y = FMath::Max(Tip.Y, L.bDown ? GroundAt(Tip.X, Tip.Z) + FMath::Max(0.0f, L.TipH) * 0.8 : -1e9);
			GaitAim(L.D, WPos(L.E), Tip);
		}
	}

	// --- Head and tail ---
	HeadYawLag += (FMath::Clamp(YawRate * 0.35f, -0.4f, 0.4f) - HeadYawLag) * (1.f - FMath::Exp(-5.f * Dt));
	const float Nod = bStill ? 0.f : FMath::Sin(Ph * 2.f + 0.5f) * 0.05f * WalkK * Amp;
	Carry += ((bStill ? 0.f : HEAD_CARRY) - Carry) * (1.f - FMath::Exp(-3.f * Dt));
	for (int32 i = 0; i < 3; ++i)
	{
		RotateLocal(NeckB[i], AX, -Heave * 0.5f + Flex * 0.15f + Nod * 0.4f + Carry * (i == 0 ? 0.5f : 0.25f));
		RotateLocal(NeckB[i], AY, HeadYawLag * 0.3f);
		RotateLocal(NeckB[i], AZ, -(Sway + Lean * 0.6f) * 0.3f);
	}
	RotateLocal(HeadB, AX, -Heave * 0.4f + Nod * 0.6f - Bob * 0.8f);
	RotateLocal(HeadB, AY, HeadYawLag * 0.35f + FMath::Sin(TimeMs * 0.00031f) * (bStill ? 0.06f : 0.015f));
	for (int32 i = 0; i < 11; ++i)
	{
		const float K = i / 11.f;
		RotateLocal(TailB[i], AY, -YawRate * 0.12f * (0.4f + K) +
			FMath::Sin(Ph - K * 2.4f) * (0.05f * WalkK + 0.03f * TrotK) * Amp +
			FMath::Sin(TimeMs * 0.0009f - K * 2.f) * (bStill ? 0.03f : 0.01f));
		RotateLocal(TailB[i], AX, -0.06f * GallopK * (1.f - K) + FMath::Sin(Ph * 2.f - K * 2.f) * 0.03f * GallopK);
	}
	TailCarriage(GroundAt);
}

// A long tail is not a rod held out level: it falls from the hips, then the
// last third rises again a little so the tip skims clear of the ground.
void UDragonWebRigComponent::TailCarriage(TFunctionRef<double(double, double)> GroundAt)
{
	if (TailB[0] < 0 || TailB[10] < 0) return;
	auto WPos = [&](int32 I) { return ModelToWeb(Model(I).GetLocation()); };
	if (TailDownSign == 0.f)
	{
		const FQuat Saved = B[TailB[0]].Q;
		const double Y0 = WPos(TailB[10]).Y;
		RotateLocal(TailB[0], AX, 0.1f);
		const double Y1 = WPos(TailB[10]).Y;
		SetQ(TailB[0], Saved);
		TailDownSign = Y1 < Y0 ? 1.f : -1.f;
	}
	FQuat Base[11];
	for (int32 i = 0; i < 11; ++i) Base[i] = B[TailB[i]].Q;
	float Scale = 1.f;
	for (int32 Pass = 0; Pass < 4; ++Pass)
	{
		for (int32 i = 0; i < 11; ++i)
		{
			const float A = i < 5 ? 0.05f : (i < 8 ? 0.015f : -0.025f);
			SetQ(TailB[i], Base[i] * AxisQ(AX, A * Scale * TailDownSign));
		}
		double Clear = 1e9;
		for (int32 i = 2; i < 11; ++i)
		{
			const FVector P = WPos(TailB[i]);
			Clear = FMath::Min(Clear, P.Y - GroundAt(P.X, P.Z));
		}
		if (Clear > 0.12) break;
		Scale *= 0.55f;
	}
}

// ===========================================================================
// Frame
// ===========================================================================
float UDragonWebRigComponent::Animate(float Dt, const FDragonWebRigInput& In)
{
	if (!bBound || !Mesh) return 0.f;
	TimeS = FMath::Fmod(TimeS + Dt, 3600.f);
	CompToWorld = Mesh->GetComponentTransform();

	// The mode-change hooks, in case the owner did not call them (main.js land() /
	// takeOff()). Before the reset, so a fold starts from last frame's pose.
	if (In.bGrounded && FoldTarget < 1.f) BeginFold();
	if (!In.bGrounded && FoldTarget > 0.f) BeginUnfold();

	// Every system writes rest + delta, so start each frame from bind. (In the web
	// the bones keep last frame's value, but every bone that is not written this
	// frame has been released to rest by the mode change, so this is the same.)
	ResetPose();

	float HeaveM = 0.f;
	if (!In.bGrounded)
	{
		// main.js frame loop, flight: beat, then the flight rig reading the beat,
		// then the fold over both while the wings are still opening, then the aim.
		UpdateWings(Dt, In);
		HeaveM = WHeave;
		UpdateFlightRig(Dt, In.FS, GetBeat());
		{
			// Legs ready to land when slow (hovering, flaring in), tucked away at speed.
			const float WantReady = 1.f - Smooth(In.FS.SpeedT, 0.025f, 0.055f);   // SpeedT 1 = 335 m/s: ready below ~10 m/s, tucked by ~18
			SFlightReady += (WantReady - SFlightReady) * Damp(2.5f, Dt);
			FlightLegs(1.f, SFlightReady * 0.85f, Dt);
		}
		FuloAirK += ((1.f - SFlightReady) - FuloAirK) * Damp(3.f, Dt);
		if (bOpeningWings)
		{
			if (ApplyFold(Dt) <= 0.001f) bOpeningWings = false;
		}
		ApplyAim(Dt, In);
	}
	else
	{
		FuloAirK += (0.f - FuloAirK) * Damp(4.f, Dt);
		WHeave = 0.f;
		if (In.bFlaring)
		{
			// Fold on the way down, not once he has stopped. Everything else is at rest.
			ApplyFold(Dt);
		}
		else
		{
			FoldTarget = 1.f;
			UpdateGround(Dt, In);
		}
		ApplyAim(Dt, In);
	}

	ApplyStow();
	WriteToMesh();
	return HeaveM;
}

void UDragonWebRigComponent::WriteToMesh()
{
	if (bFulo) { WriteFulo(); return; }
	if (bModelDirty) RecomputeModel();
	const int32 NU = UEParent.Num();
	if (Mesh->BoneSpaceTransforms.Num() != NU) return;
	for (int32 u = 0; u < NU; ++u)
	{
		const int32 G = UEToGl[u];
		const int32 Par = UEParent[u];
		if (G >= 0)
		{
			// glTF component-space delta, carried through A.
			const FTransform D = RestModel[G].Inverse() * ModelCache[G];
			const FQuat Dq = D.GetRotation();
			const FVector Ax = ApplyAn(FVector(Dq.X, Dq.Y, Dq.Z)) * ADet;
			const FQuat Qu = FQuat(Ax.X, Ax.Y, Ax.Z, Dq.W).GetNormalized();
			const FVector Tu = ApplyAn(D.GetTranslation()) * AScale;
			UEAnimCS[u] = UERefCS[u] * FTransform(Qu, Tu, D.GetScale3D());
		}
		else
		{
			UEAnimCS[u] = Par == INDEX_NONE ? UERefLocal[u] : UERefLocal[u] * UEAnimCS[Par];
		}
		Mesh->BoneSpaceTransforms[u] = Par == INDEX_NONE ? UEAnimCS[u] : UEAnimCS[u].GetRelativeTransform(UEAnimCS[Par]);
	}
	Mesh->MarkRefreshTransformDirty();
}

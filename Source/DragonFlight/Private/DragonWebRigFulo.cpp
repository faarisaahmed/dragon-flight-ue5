// The Night Fury by Fulonimation Studios (https://ko-fi.com/fulonimationstudios), driven by
// this component's own motion.
//
// The web rig keeps running exactly as before on its own (now invisible) skeleton: wingbeat,
// glide, turns, tail, head, gait. Each frame that pose is carried onto the Fulonimation rig:
//   - controls (hips, chest, head, foot IK targets, knee/elbow poles, jaw) follow the web bone
//     they stand in for, rigidly;
//   - wing segments, fingers and the tail are aimed along their web counterparts (so the two rigs'
//     different rest poses don't matter), membrane ribs between two fingers by a blend of the two;
//   - folding is the model's own WingsFold action, blended in as the web fold closes;
// and then the rig's constraints (stretchy spine and neck, membrane, IK legs, fins) are evaluated
// as Blender would (FDragonFuloRig).

#include "DragonWebRigComponent.h"
#include "DragonFlight.h"
#include "Components/PoseableMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

namespace
{
	FString NormF(const FString& S)
	{
		FString O;
		for (TCHAR C : S) { C = FChar::ToLower(C); if (C != '.' && C != '_' && C != ' ') O.AppendChar(C); }
		return O;
	}

	// Fulo armature (Z up, nose -Y, .L on +X) <-> web glTF (Y up, nose -Z, .L on -X): (x,y,z) -> (-x, z, y), its own inverse.
	FVector FW(const FVector& V) { return FVector(-V.X, V.Z, V.Y); }
	FQuat FWQ(const FQuat& Q) { const FVector A = FW(FVector(Q.X, Q.Y, Q.Z)); return FQuat(A.X, A.Y, A.Z, Q.W); }

	FFuloM4 FromUE(const FTransform& T)
	{
		const FMatrix M = T.ToMatrixWithScale();
		FFuloM4 R = FFuloM4::Identity();
		for (int32 r = 0; r < 3; ++r) for (int32 c = 0; c < 4; ++c) R.M[r][c] = M.M[c][r];
		return R;
	}

	FTransform ToUE(const FFuloM4& A)
	{
		FMatrix M = FMatrix::Identity;
		for (int32 r = 0; r < 4; ++r) for (int32 c = 0; c < 3; ++c) M.M[r][c] = A.M[c][r];
		M.M[3][0] = A.M[0][3]; M.M[3][1] = A.M[1][3]; M.M[3][2] = A.M[2][3];
		return FTransform(M);
	}

	const TCHAR* GFinNames[] = { TEXT("Black"), TEXT("Red"), TEXT("Brown"), TEXT("Blue"), TEXT("Purple"), TEXT("GreenFlames"), TEXT("StrikeClass") };
}

int32 UDragonWebRigComponent::SavedFinStyle()
{
	int32 V = 1;   // the red fin of the first film
	if (GConfig) GConfig->GetInt(TEXT("/Script/DragonFlight.DragonControls"), TEXT("FinStyle"), V, GGameUserSettingsIni);
	FParse::Value(FCommandLine::Get(), TEXT("FinStyle="), V);   // test runs
	return FMath::Clamp(V, 0, 7);
}

void UDragonWebRigComponent::SaveFinStyle(int32 Style)
{
	if (!GConfig) return;
	GConfig->SetInt(TEXT("/Script/DragonFlight.DragonControls"), TEXT("FinStyle"), FMath::Clamp(Style, 0, 7), GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

const TCHAR* UDragonWebRigComponent::FinStyleName(int32 Style)
{
	static const TCHAR* Shown[] = { TEXT("Black"), TEXT("Red (first film)"), TEXT("Brown"), TEXT("Blue"), TEXT("Purple"), TEXT("Green flames"), TEXT("Strike class"), TEXT("Bare frame") };
	return Shown[FMath::Clamp(Style, 0, 7)];
}

void UDragonWebRigComponent::SetFinStyle(int32 Style)
{
	FinStyle = FMath::Clamp(Style, 0, 7);
	if (!bFulo || !Mesh || FinMaterial == INDEX_NONE) return;
	const bool bShow = FinStyle < 7;
	USkinnedAsset* Asset = Mesh->GetSkinnedAsset();
	if (Asset && Asset->GetResourceForRendering())
		for (int32 Lod = 0; Lod < Asset->GetResourceForRendering()->LODRenderData.Num(); ++Lod)
		{
			const TArray<FSkelMeshRenderSection>& Sec = Asset->GetResourceForRendering()->LODRenderData[Lod].RenderSections;
			for (int32 s = 0; s < Sec.Num(); ++s) if (Sec[s].MaterialIndex == FinMaterial) Mesh->ShowMaterialSection(FinMaterial, s, bShow, Lod);
		}
	if (!bShow) return;
	UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(FinMaterial));
	if (!MID) MID = Mesh->CreateDynamicMaterialInstance(FinMaterial);
	const FString Path = FString::Printf(TEXT("/Game/DragonFulo/Textures/T_Fulo_Fin_%s.T_Fulo_Fin_%s"), GFinNames[FinStyle], GFinNames[FinStyle]);
	if (UTexture2D* T = LoadObject<UTexture2D>(nullptr, *Path))
		if (MID) MID->SetTextureParameterValue(TEXT("BaseColorTex"), T);
}

bool UDragonWebRigComponent::BindFulo(const FReferenceSkeleton& Ref)
{
	Fulo = MakeShared<FDragonFuloRig>();
	if (!Fulo->Init()) return false;
	FDragonFuloRig& R = *Fulo;
	const int32 NF = R.Num(), NU = Ref.GetNum();

	// --- UE bones by name
	TMultiMap<FString, int32> FByKey;
	for (int32 i = 0; i < NF; ++i) FByKey.Add(NormF(R.Name(i)), i);
	FuloToUE.Init(INDEX_NONE, NF);
	UEToFulo.Init(INDEX_NONE, NU);
	UEParent.SetNum(NU);
	UERefLocal = Ref.GetRefBonePose();
	UERefCS.SetNum(NU);
	UEAnimCS.SetNum(NU);
	int32 Matched = 0;
	for (int32 u = 0; u < NU; ++u)
	{
		UEParent[u] = Ref.GetParentIndex(u);
		UERefCS[u] = UEParent[u] == INDEX_NONE ? UERefLocal[u] : UERefLocal[u] * UERefCS[UEParent[u]];
		// exact names first (hSup.3.L and hSup3.L differ only by a dot), then dots/underscores/case ignored
		int32 F = R.Find(Ref.GetBoneName(u).ToString());
		if (F >= 0 && FuloToUE[F] != INDEX_NONE) F = INDEX_NONE;
		if (F < 0)
		{
			TArray<int32> Cands; FByKey.MultiFind(NormF(Ref.GetBoneName(u).ToString()), Cands, true);
			for (int32 C : Cands) if (FuloToUE[C] == INDEX_NONE) { F = C; break; }
		}
		if (F >= 0) { FuloToUE[F] = u; UEToFulo[u] = F; ++Matched; }
	}
	if (Matched < NF * 9 / 10)
	{
		UE_LOG(LogDragon, Warning, TEXT("FuloRig: only %d of %d bones matched on the mesh"), Matched, NF);
		return false;
	}

	// --- Fulo armature -> UE component: least squares over every matched bone's rest head
	{
		FVector Pg = FVector::ZeroVector, Pu = FVector::ZeroVector;
		int32 N = 0;
		for (int32 i = 0; i < NF; ++i) if (FuloToUE[i] >= 0) { Pg += R.RestHead(i); Pu += UERefCS[FuloToUE[i]].GetLocation(); ++N; }
		Pg /= N; Pu /= N;
		double H[3][3] = {}, K[3][3] = {};
		for (int32 i = 0; i < NF; ++i)
		{
			if (FuloToUE[i] < 0) continue;
			const FVector P = R.RestHead(i) - Pg, Q = UERefCS[FuloToUE[i]].GetLocation() - Pu;
			for (int32 r = 0; r < 3; ++r) for (int32 c = 0; c < 3; ++c) { H[r][c] += Q[r] * P[c]; K[r][c] += P[r] * P[c]; }
		}
		// A = H K^-1
		FFuloM4 KM = FFuloM4::Identity();
		for (int32 r = 0; r < 3; ++r) for (int32 c = 0; c < 3; ++c) KM.M[r][c] = K[r][c];
		const FFuloM4 KI = KM.Inverse();
		FuloC = FFuloM4::Identity();
		for (int32 r = 0; r < 3; ++r) for (int32 c = 0; c < 3; ++c)
		{
			double V = 0; for (int32 k = 0; k < 3; ++k) V += H[r][k] * KI.M[k][c];
			FuloC.M[r][c] = V;
		}
		const FVector T = Pu - FuloC.TransformVector(Pg);
		FuloC.SetCol(3, T);
		double Err = 0;
		for (int32 i = 0; i < NF; ++i) if (FuloToUE[i] >= 0) Err += FVector::DistSquared(FuloC.TransformPosition(R.RestHead(i)), UERefCS[FuloToUE[i]].GetLocation());
		UE_LOG(LogDragon, Log, TEXT("FuloRig: %d/%d bones, armature->component fit rms %.3f, scale %.2f"), Matched, NF, FMath::Sqrt(Err / N), FuloC.Col(0).Size());
	}
	FuloK.SetNum(NF);
	for (int32 i = 0; i < NF; ++i)
		FuloK[i] = FuloToUE[i] >= 0 ? (FuloC * R.RestMat(i)).Inverse() * FromUE(UERefCS[FuloToUE[i]]) : FFuloM4::Identity();

	// --- where the model stands in the web frame: same size as the web dragon, hips over its
	// hips, claws on its ground
	{
		static const float Size = [] { float V = 1.15f; FParse::Value(FCommandLine::Get(), TEXT("FuloSize="), V); return V; }();
		FuloSW = FDragonFuloRig::ObjectScale() * Size;
		const int32 Hc = R.Find(TEXT("HipsControl.1"));
		const int32 WH = Find(TEXT("Hips"));
		double FootZ = 1e9;
		for (int32 i = 0; i < NF; ++i)
		{
			const FString& N = R.Name(i);
			if (N.StartsWith(TEXT("Claw")) || N.StartsWith(TEXT("BFoot.")) || N.StartsWith(TEXT("Foot."))) FootZ = FMath::Min(FootZ, R.RestHead(i).Z);
		}
		double WebFoot = 1e9;
		for (int32 g = 0; g < B.Num(); ++g)
		{
			const FString N = WebBoneName(g);
			if (N.Contains(TEXT("Digit")) || N.StartsWith(TEXT("Toe."))) WebFoot = FMath::Min(WebFoot, RestModel[g].GetLocation().Y);
		}
		const FVector HipsW = WH >= 0 ? RestModel[WH].GetLocation() : FVector::ZeroVector;
		const FVector HipsF = Hc >= 0 ? R.RestHead(Hc) : FVector::ZeroVector;
		FuloOffW = HipsW - FW(HipsF) * FuloSW;
		FuloOffW.Y = WebFoot - FootZ * FuloSW;
	}
	// mesh placement in the pawn, as the web mesh is placed: pawn = 100 * Rwp * webPoint, Rwp: web (x,y,z) -> pawn (-z, x, y)
	{
		const FFuloM4 CI = FuloC.Inverse();
		auto ToPawn = [&](const FVector& U) -> FVector
		{
			const FVector PF = CI.TransformPosition(U);
			const FVector W = FW(PF) * FuloSW + FuloOffW;
			return FVector(-W.Z, W.X, W.Y) * 100.0;
		};
		const FVector O = ToPawn(FVector::ZeroVector);
		const FVector X = ToPawn(FVector(1, 0, 0)) - O, Y = ToPawn(FVector(0, 1, 0)) - O, Z = ToPawn(FVector(0, 0, 1)) - O;
		MeshRelative = FTransform(FMatrix(FPlane(X, 0), FPlane(Y, 0), FPlane(Z, 0), FPlane(O, 1)));
		UE_LOG(LogDragon, Log, TEXT("FuloRig: mesh relative %s"), *MeshRelative.ToString());
	}

	// --- what follows what
	FuloFollows.Reset(); FuloDirs.Reset(); FuloRibs.Reset();
	auto Follow = [&](const TCHAR* F, const TCHAR* WPos, const TCHAR* WRot)
	{
		FFuloFollow X; X.Bone = R.Find(F); X.WebPos = Find(WPos); X.WebRot = Find(WRot);
		if (X.Bone >= 0 && X.WebPos >= 0 && X.WebRot >= 0) FuloFollows.Add(X);
		else UE_LOG(LogDragon, Warning, TEXT("FuloRig: no follow %s <- %s/%s"), F, WPos, WRot);
	};
	Follow(TEXT("HipsControl.1"), TEXT("Hips"), TEXT("Hips"));
	Follow(TEXT("ChestControl.1"), TEXT("Spine.002"), TEXT("Spine.002"));

	for (int32 s = 0; s < 2; ++s)
	{
		const TCHAR* FS = s == 0 ? TEXT("L") : TEXT("R");
		const TCHAR* WS = s == 0 ? TEXT("L") : TEXT("R");
		Follow(*FString::Printf(TEXT("BFootIK.%s"), FS), *FString::Printf(TEXT("Ankle.%s"), WS), *FString::Printf(TEXT("Toe.%s"), WS));
		Follow(*FString::Printf(TEXT("FFootIK.%s"), FS), *FString::Printf(TEXT("Wrist.%s"), WS), *FString::Printf(TEXT("Front_Toe.%s"), WS));
		Follow(*FString::Printf(TEXT("KneePoint.%s"), FS), TEXT("Hips"), TEXT("Hips"));
		Follow(*FString::Printf(TEXT("ShoulderPoint.%s"), FS), TEXT("Spine.002"), TEXT("Spine.002"));
	}
	FuloHead = R.Find(TEXT("HeadControl.1"));
	FuloNeckStart = R.Find(TEXT("NeckStart.1"));
	FuloWingFold[0] = R.Find(TEXT("WingFold.L"));
	FuloWingFold[1] = R.Find(TEXT("WingFold.R"));

	// wings: arm segments, then the six fingers paired by sweep (0 deg straight out, 90 deg back to the tail)
	for (int32 s = 0; s < 2; ++s)
	{
		const TCHAR* S = s == 0 ? TEXT("L") : TEXT("R");
		const double Out = s == 0 ? 1.0 : -1.0;   // Fulo .L is +X
		auto Sweep = [&](const FVector& D) { return FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X * Out)); };   // Fulo frame
		auto Seg = [&](const TCHAR* F, const TCHAR* W)
		{
			FFuloDir X; X.Bone = R.Find(FString::Printf(TEXT("%s.%s"), F, S)); X.Web = Find(FString::Printf(TEXT("%s.%s"), W, S)); X.Side = s; X.bWing = true;
			if (X.Bone >= 0 && X.Web >= 0) FuloDirs.Add(X);
			return FuloDirs.Num() - 1;
		};
		Seg(TEXT("WingShoulder"), TEXT("Wing_Shoulder"));
		Seg(TEXT("WElbow"), TEXT("Wing_UpperArm"));
		const int32 ArmSeg = Seg(TEXT("WArm"), TEXT("Wing_Forearm"));
		// Fulo fingers: KnuckleN -> three long bones
		struct FFinger { float Sweep; int32 Bones[3]; };
		TArray<FFinger> FF;
		for (int32 k = 1; k <= 6; ++k)
		{
			const int32 Kn = R.Find(FString::Printf(TEXT("Knuckle%d.%s"), k, S));
			if (Kn < 0) continue;
			FFinger F; int32 b = Kn;
			for (int32 j = 0; j < 3; ++j)
			{
				int32 Child = INDEX_NONE;
				for (int32 c = 0; c < NF; ++c) if (R.Parent(c) == b && R.Name(c).StartsWith(TEXT("Bone."))) { Child = c; break; }
				F.Bones[j] = Child; if (Child >= 0) b = Child;
			}
			if (F.Bones[0] < 0) continue;
			F.Sweep = Sweep(R.RestTail(F.Bones[2] >= 0 ? F.Bones[2] : F.Bones[0]) - R.RestHead(F.Bones[0]));
			FF.Add(F);
		}
		// web fingers: Wing_Finger.(3j+1..3j+3)
		struct FWF { float Sweep; int32 Bones[3]; };
		TArray<FWF> WF;
		for (int32 j = 0; j < 6; ++j)
		{
			FWF W;
			for (int32 k = 0; k < 3; ++k) W.Bones[k] = Find(FString::Printf(TEXT("Wing_Finger.%03d.%s"), j * 3 + k + 1, S));
			if (W.Bones[0] < 0 || W.Bones[2] < 0) continue;
			const FVector D = FW(RestModel[W.Bones[2]].GetLocation() - RestModel[W.Bones[0]].GetLocation()) + FW(RestModel[W.Bones[2]].GetRotation().RotateVector(FVector(0, 1, 0))) * 0.5;
			W.Sweep = Sweep(D);
			WF.Add(W);
		}
		FF.Sort([](const FFinger& A, const FFinger& Bb) { return A.Sweep < Bb.Sweep; });
		WF.Sort([](const FWF& A, const FWF& Bb) { return A.Sweep < Bb.Sweep; });
		TArray<int32> FingerSeg;   // FuloDirs index of each finger's first bone, in sweep order
		TArray<float> FingerSweep;
		for (int32 f = 0; f < FMath::Min(FF.Num(), WF.Num()); ++f)
		{
			for (int32 j = 0; j < 3; ++j)
			{
				if (FF[f].Bones[j] < 0 || WF[f].Bones[j] < 0) continue;
				FFuloDir X; X.Bone = FF[f].Bones[j]; X.Web = WF[f].Bones[j]; X.Side = s; X.bWing = true;
				FuloDirs.Add(X);
				if (j == 0) { FingerSeg.Add(FuloDirs.Num() - 1); FingerSweep.Add(FF[f].Sweep); }
			}
			UE_LOG(LogDragon, Log, TEXT("FuloRig: finger %s %d (sweep %.0f) <- web %d (sweep %.0f)"), S, f, FF[f].Sweep, f, WF[f].Sweep);
		}
		// membrane ribs: the SMem chains, between the two fingers they lie between
		for (int32 c = 0; c < NF; ++c)
		{
			const FString& N = R.Name(c);
			if (!N.StartsWith(TEXT("SMem")) || !N.EndsWith(FString(TEXT(".")) + S)) continue;
			const FString& PN = R.Name(R.Parent(c));
			if (PN.StartsWith(TEXT("Mem")) || PN.StartsWith(TEXT("SMem"))) continue;   // only chain roots
			// its direction: root head to the deepest bone of its chain
			int32 Deep = c;
			for (bool bMore = true; bMore;)
			{
				bMore = false;
				for (int32 k = 0; k < NF; ++k) if (R.Parent(k) == Deep) { Deep = k; bMore = true; break; }
			}
			FFuloRib Rb; Rb.Bone = c; Rb.Side = s;
			Rb.RestDir = (R.RestTail(Deep) - R.RestHead(c)).GetSafeNormal();
			const float Sw = Sweep(Rb.RestDir);
			int32 Lo = INDEX_NONE, Hi = INDEX_NONE;
			for (int32 f = 0; f < FingerSweep.Num(); ++f) { if (FingerSweep[f] <= Sw) Lo = f; if (FingerSweep[f] > Sw && Hi == INDEX_NONE) Hi = f; }
			Rb.A = Lo >= 0 ? FingerSeg[Lo] : ArmSeg;
			Rb.B = Hi >= 0 ? FingerSeg[Hi] : ArmSeg;
			const float SA = Lo >= 0 ? FingerSweep[Lo] : Sw, SB = Hi >= 0 ? FingerSweep[Hi] : Sw + 30.f;
			Rb.W = SB > SA ? FMath::Clamp((Sw - SA) / (SB - SA), 0.f, 1.f) : 0.f;
			if (Lo < 0) { Rb.A = ArmSeg; Rb.W = Hi >= 0 ? 0.6f : 0.f; }
			FuloRibs.Add(Rb);
		}
	}
	// tail: Fulo tail bones (FullTailRot.1 down the chain, through the fin) aimed along the web tail at the same arc
	{
		TArray<int32> Chain;
		for (int32 b = R.Find(TEXT("Bone")); b >= 0;)
		{
			Chain.Add(b);
			int32 Next = INDEX_NONE;
			for (int32 c = 0; c < NF; ++c)
				if (R.Parent(c) == b && (R.Name(c) == TEXT("Bone") || (R.Name(c).StartsWith(TEXT("Bone.")) && !R.Name(c).EndsWith(TEXT(".L")) && !R.Name(c).EndsWith(TEXT(".R")) && R.BoneLength(c) > 0.015f)))
				{ Next = c; break; }
			b = Next;
		}
		const int32 Root = R.Find(TEXT("FullTailRot.1"));
		const FVector R0 = Root >= 0 ? R.RestHead(Root) : R.RestHead(Chain[0]);
		const double Total = FVector::Dist(R0, R.RestHead(R.Find(TEXT("Bone.003")) >= 0 ? R.Find(TEXT("Bone.003")) : Chain.Last()));
		FuloTailWeb.Reset(); FuloTailArc.Reset();
		double WebLen = 0; FVector Prev = FVector::ZeroVector;
		for (int32 i = 0; i < 11; ++i)
		{
			const int32 W = Find(FString::Printf(TEXT("Tail.%03d"), i + 1));
			if (W < 0) continue;
			const FVector P = RestModel[W].GetLocation();
			if (FuloTailWeb.Num()) WebLen += FVector::Dist(P, Prev);
			Prev = P;
			FuloTailWeb.Add(W); FuloTailArc.Add(WebLen);
		}
		for (float& A : FuloTailArc) A /= FMath::Max(WebLen, 1e-3);
		for (int32 b : Chain)
		{
			if (R.BoneLength(b) < 0.06f) continue;   // the short joint bones just follow
			FFuloDir X; X.Bone = b; X.Arc = FMath::Clamp(float(FVector::Dist(R0, R.RestHead(b)) / Total), 0.f, 1.f);
			FuloDirs.Add(X);
		}
	}

	// tail fins: each Fulo strut turns as the web strut nearest it in fan angle (same side) turns
	// against the tail; the ears as the web ears turn against the head
	FuloRels.Reset();
	{
		struct FS { int32 Bone; int32 Side; float Ang; int32 Parent; };
		TArray<FS> Web, Ful;
		auto FanAng = [](const FVector& D) { return FMath::RadiansToDegrees(FMath::Atan2(FMath::Abs(D.X), D.Y)); };   // 0 straight back, 90 straight out
		for (int32 s = 0; s < 2; ++s)
			for (int32 i = 0; i < 6; ++i)
			{
				const int32 St = FindF(TEXT("Tail_Fin_Strut"), i + 1, s == 0 ? TEXT("L") : TEXT("R"));
				const int32 Tp = FindF(TEXT("Tail_Fin_Tip"), i + 1, s == 0 ? TEXT("L") : TEXT("R"));
				if (St < 0 || Tp < 0) continue;
				FS X; X.Bone = St; X.Side = s; X.Parent = B[St].Parent;
				X.Ang = FanAng(FW(RestModel[Tp].GetLocation() - RestModel[St].GetLocation()));
				Web.Add(X);
			}
		for (int32 i = 0; i < NF; ++i)
		{
			const FString& N = R.Name(i);
			const bool bStrut = (N.StartsWith(TEXT("F1.R")) || N.StartsWith(TEXT("F3.R")) || N.StartsWith(TEXT("F5.R")) || N.StartsWith(TEXT("F7.R")) || N.StartsWith(TEXT("F9.R")))
				&& !N.StartsWith(TEXT("F1.R.001"));
			if (!bStrut) continue;
			const FVector D = R.RestTail(i) - R.RestHead(i);
			FS X; X.Bone = i; X.Side = R.RestHead(i).X > 0.0 ? 0 : 1; X.Ang = FanAng(D); X.Parent = R.Parent(i);
			Ful.Add(X);
		}
		for (const FS& F : Ful)
		{
			const FS* Best = nullptr;
			for (const FS& W : Web) if (W.Side == F.Side && (!Best || FMath::Abs(W.Ang - F.Ang) < FMath::Abs(Best->Ang - F.Ang))) Best = &W;
			if (!Best) continue;
			FFuloRel Rl; Rl.Bone = F.Bone; Rl.Web = Best->Bone; Rl.WebParent = Best->Parent;
			FuloRels.Add(Rl);
		}
		for (int32 s = 0; s < 2; ++s)
		{
			const TCHAR* S = s == 0 ? TEXT("L") : TEXT("R");
			auto Ear = [&](const TCHAR* F, const TCHAR* W)
			{
				FFuloRel Rl; Rl.Bone = R.Find(FString::Printf(TEXT("%s.%s"), F, S)); Rl.Web = Find(FString::Printf(TEXT("%s.%s"), W, S));
				Rl.WebParent = Rl.Web >= 0 ? B[Rl.Web].Parent : INDEX_NONE;
				if (Rl.Bone >= 0 && Rl.Web >= 0 && Rl.WebParent >= 0) FuloRels.Add(Rl);
			};
			Ear(TEXT("TopEarStart"), TEXT("Ear_A.001"));
			Ear(TEXT("BigEar2"), TEXT("Ear_B.001"));
			Ear(TEXT("SideEar2"), TEXT("Ear_C"));
			Ear(TEXT("SmallEar1"), TEXT("Ear_C"));
		}
	}

	// --- the fin's material slot
	FinMaterial = INDEX_NONE;
	if (const USkinnedAsset* Asset = Mesh ? Mesh->GetSkinnedAsset() : nullptr)
	{
		const TArray<FSkeletalMaterial>& Mats = Asset->GetMaterials();
		for (int32 m = 0; m < Mats.Num(); ++m) if (Mats[m].MaterialSlotName.ToString().Equals(TEXT("Fin"), ESearchCase::IgnoreCase)) FinMaterial = m;
	}
	MembraneMaterial = INDEX_NONE;
	// test runs: -FuloSkin=Name=Value,Name=Value ... sets the hide's material parameters (Body and Claws)
	{
		FString Skin;
		if (FParse::Value(FCommandLine::Get(), TEXT("FuloSkin="), Skin, false) && Mesh)
		{
			TArray<FString> Pairs; Skin.ParseIntoArray(Pairs, TEXT(","));
			const TArray<FSkeletalMaterial>& Mats = Mesh->GetSkinnedAsset()->GetMaterials();
			for (int32 m = 0; m < Mats.Num(); ++m)
			{
				const FString SN = Mats[m].MaterialSlotName.ToString();
				if (!SN.Equals(TEXT("Body"), ESearchCase::IgnoreCase)) continue;
				UMaterialInstanceDynamic* MID = Mesh->CreateDynamicMaterialInstance(m);
				for (const FString& Pr : Pairs)
				{
					FString K, V;
					if (!Pr.Split(TEXT("="), &K, &V)) continue;
					if (K == TEXT("Tint")) MID->SetVectorParameterValue(TEXT("Tint"), FLinearColor(FCString::Atof(*V), FCString::Atof(*V), FCString::Atof(*V)));
					else MID->SetScalarParameterValue(FName(*K), FCString::Atof(*V));
				}
			}
		}
	}
	UE_LOG(LogDragon, Log, TEXT("FuloRig: bound: %d follows, %d aimed bones, %d membrane ribs, %d relative"), FuloFollows.Num(), FuloDirs.Num(), FuloRibs.Num(), FuloRels.Num());
	FString Unmatched;
	for (int32 i = 0; i < NF; ++i) if (FuloToUE[i] < 0) Unmatched += R.Name(i) + TEXT(" ");
	// (Shipping writes no log: leave a note of the bind for tests)
	FFileHelper::SaveStringToFile(FString::Printf(TEXT("matched %d/%d (unmatched: %s), follows %d, aimed %d, ribs %d, rel %d, fin slot %d\nmesh relative %s\nC col0 %s col1 %s col2 %s off %s\n"),
		Matched, NF, *Unmatched, FuloFollows.Num(), FuloDirs.Num(), FuloRibs.Num(), FuloRels.Num(), FinMaterial, *MeshRelative.ToString(),
		*FuloC.Col(0).ToString(), *FuloC.Col(1).ToString(), *FuloC.Col(2).ToString(), *FuloC.Col(3).ToString()),
		*(FPaths::ProjectSavedDir() / TEXT("FuloBind.txt")));
	return true;
}

void UDragonWebRigComponent::WriteFulo()
{
	{
		static int32 Calls = 0;
		if (++Calls == 3) FFileHelper::SaveStringToFile(FString::Printf(TEXT("WriteFulo running, mesh %d, bones %d/%d\n"), Mesh ? 1 : 0, Mesh ? Mesh->BoneSpaceTransforms.Num() : -1, UEParent.Num()), *(FPaths::ProjectSavedDir() / TEXT("FuloWrite.txt")));
	}
	if (!Fulo.IsValid() || !Mesh) return;
	if (bModelDirty) RecomputeModel();
	FDragonFuloRig& R = *Fulo;
	R.ResetPose();

	// web pose -> Fulo armature space
	auto ToF = [&](const FVector& PW) { return FW((PW - FuloOffW) / FuloSW); };
	auto ToW = [&](const FVector& PF) { return FW(PF) * FuloSW + FuloOffW; };
	auto WebDelta = [&](int32 G) -> FQuat { return FWQ(ModelCache[G].GetRotation() * RestModel[G].GetRotation().Inverse()); };
	auto WebMove = [&](int32 G, const FVector& PF) -> FVector
	{
		return ToF(ModelCache[G].TransformPosition(RestModel[G].InverseTransformPosition(ToW(PF))));
	};
	auto WebDir = [&](int32 G) { return FW(ModelCache[G].GetRotation().RotateVector(FVector(0, 1, 0))); };

	// blinks: the model's own eyelids (Blink.1's scale drives the Blink actions), a quick close
	// every few seconds, now and then a double
	{
		static const int32 BlinkC = R.Find(TEXT("Blink.1"));
		static const float Force = [] { float V = -1.f; FParse::Value(FCommandLine::Get(), TEXT("FuloBlink="), V); return V; }();
		if (BlinkC >= 0)
		{
			auto Bump = [](float T, float Dur) { return T >= 0.f && T <= Dur ? FMath::Sin(PI * T / Dur) : 0.f; };
			const float P = FMath::Fmod(TimeS, 4.7f), P2 = FMath::Fmod(TimeS + 1.3f, 11.3f);
			float Closed = FMath::Max(Bump(P, 0.2f), Bump(P2, 0.18f));
			if (Force >= 0.f) Closed = Force;
			R.Scale[BlinkC] = FVector(1.f, 1.f, 1.f + 0.5f * FMath::Clamp(Closed, 0.f, 1.f));
		}
	}
	const float FoldK = FMath::Clamp(Fold, 0.f, 1.f);
	if (FuloWingFold[0] >= 0) R.Loc[FuloWingFold[0]] = FVector(-FoldK, 0, 0);
	if (FuloWingFold[1] >= 0) R.Loc[FuloWingFold[1]] = FVector(FoldK, 0, 0);

	// desired armature poses for followers, and desired world directions for aimed bones
	TMap<int32, FFuloM4> Want;
	for (const FFuloFollow& F : FuloFollows)
	{
		const FVector P = WebMove(F.WebPos, R.RestHead(F.Bone));
		const FQuat Q = (WebDelta(F.WebRot) * R.RestMat(F.Bone).Rot()).GetNormalized();
		Want.Add(F.Bone, FFuloM4::FromLRS(P, Q, FVector::OneVector));
	}
	// head: carried by the chest, its neck swung as the web neck swings, its own turn the web head's
	if (FuloHead >= 0 && FuloNeckStart >= 0)
	{
		const int32 WC = Find(TEXT("Spine.002")), WN = Find(TEXT("Neck.001")), WHd = Find(TEXT("Head"));
		if (WC >= 0 && WN >= 0 && WHd >= 0)
		{
			const FVector NeckF = WebMove(WC, R.RestHead(FuloNeckStart));
			const FQuat ChestQ = WebDelta(WC);
			const FVector VR = ChestQ.RotateVector(FW(RestModel[WHd].GetLocation() - RestModel[WN].GetLocation()));
			const FVector VP = FW(ModelCache[WHd].GetLocation() - ModelCache[WN].GetLocation());
			const FQuat Bend = FQuat::FindBetweenVectors(VR, VP);
			const double Stretch = FMath::Clamp(VP.Size() / FMath::Max(VR.Size(), 1e-4), 0.85, 1.15);
			const FVector Off = Bend.RotateVector(ChestQ.RotateVector(R.RestHead(FuloHead) - R.RestHead(FuloNeckStart))) * Stretch;
			const FQuat Q = (WebDelta(WHd) * R.RestMat(FuloHead).Rot()).GetNormalized();
			Want.Add(FuloHead, FFuloM4::FromLRS(NeckF + Off, Q, FVector::OneVector));
		}
	}
	// aimed bones: wing segments and fingers along their web bone, tail bones along the web tail
	TArray<FVector> DirWant; DirWant.SetNum(FuloDirs.Num());
	TArray<FQuat> SwingW; SwingW.SetNum(FuloDirs.Num());   // rest direction -> wanted, whole-wing (for the ribs)
	TMap<int32, int32> DirOf;
	for (int32 d = 0; d < FuloDirs.Num(); ++d)
	{
		const FFuloDir& X = FuloDirs[d];
		FVector D;
		if (X.Web >= 0) D = WebDir(X.Web);
		else
		{
			// the web tail's direction at this arc
			int32 k = 0;
			while (k + 1 < FuloTailArc.Num() && FuloTailArc[k + 1] < X.Arc) ++k;
			const int32 k1 = FMath::Min(k + 1, FuloTailWeb.Num() - 1);
			const float T = k1 > k ? FMath::Clamp((X.Arc - FuloTailArc[k]) / FMath::Max(FuloTailArc[k1] - FuloTailArc[k], 1e-4f), 0.f, 1.f) : 0.f;
			D = FMath::Lerp(WebDir(FuloTailWeb[k]), WebDir(FuloTailWeb[k1]), T);
		}
		DirWant[d] = D.GetSafeNormal();
		SwingW[d] = FQuat::FindBetweenNormals(R.RestMat(X.Bone).Rot().RotateVector(FVector(0, 1, 0)), DirWant[d]);
		DirOf.Add(X.Bone, d);
	}
	TMap<int32, FQuat> RelOf;
	for (const FFuloRel& Rl : FuloRels)
	{
		const FQuat DW = ModelCache[Rl.Web].GetRotation() * RestModel[Rl.Web].GetRotation().Inverse();
		const FQuat DP = ModelCache[Rl.WebParent].GetRotation() * RestModel[Rl.WebParent].GetRotation().Inverse();
		RelOf.Add(Rl.Bone, FWQ(DW * DP.Inverse()).GetNormalized());
	}
	TMap<int32, int32> RibOf;
	for (int32 r = 0; r < FuloRibs.Num(); ++r) RibOf.Add(FuloRibs[r].Bone, r);
	const float WingOpen = 1.f - FoldK;

	R.PreBone = [&](int32 i)
	{
		const FFuloM4 P0 = R.PreMat(i) * R.ActionLocal(i);
		const FQuat Q0 = P0.Rot();
		if (const FFuloM4* W = Want.Find(i))
		{
			// basis that puts the bone at W: (Pre * A)^-1 * W
			const FFuloM4 Bm = P0.Inverse() * *W;
			R.Loc[i] = Bm.Loc(); R.Rot[i] = Bm.Rot(); R.Scale[i] = FVector::OneVector;
			return;
		}
		if (const FQuat* Rel = RelOf.Find(i))
		{
			// turned against its parent as the web bone turns against its own
			R.Rot[i] = (Q0.Inverse() * *Rel * Q0).GetNormalized();
			return;
		}
		FVector Want3 = FVector::ZeroVector;
		float K = 1.f;
		if (const int32* D = DirOf.Find(i)) { Want3 = DirWant[*D]; K = FuloDirs[*D].bWing ? WingOpen : 1.f; }
		else if (const int32* Rr = RibOf.Find(i))
		{
			// the rib turns as the two fingers either side of it turn (from rest, whole wing)
			const FFuloRib& Rb = FuloRibs[*Rr];
			const FQuat Sw = FQuat::Slerp(SwingW[Rb.A], SwingW[Rb.B], Rb.W);
			Want3 = Sw.RotateVector(R.RestMat(i).Rot().RotateVector(FVector(0, 1, 0)));
			K = WingOpen;
		}
		else return;
		if (Want3.IsNearlyZero() || K <= 0.f) return;
		const FVector Y = Q0.RotateVector(FVector(0, 1, 0));
		const FQuat Sw = FQuat::Slerp(FQuat::Identity, FQuat::FindBetweenNormals(Y, Want3.GetSafeNormal()), K);
		R.Rot[i] = (Q0.Inverse() * Sw * Q0).GetNormalized();
	};
	// the jaw: the model rests with the mouth a little open; closed, plus whatever the web jaw does
	{
		static const int32 FJaw = R.Find(TEXT("Jaw.1"));
		static const float Close = [] { float V = 0.f; FParse::Value(FCommandLine::Get(), TEXT("FuloJawClose="), V); return V; }();
		const int32 WJ = Find(TEXT("Jaw"));
		if (FJaw >= 0)
		{
			FQuat Q = FQuat(FVector(1, 0, 0), FMath::DegreesToRadians(Close));
			if (WJ >= 0)
			{
				// the web jaw's opening relative to its head, about the same hinge
				const FQuat Rel = (ModelCache[WJ].GetRotation() * RestModel[WJ].GetRotation().Inverse());
				const int32 WH = Find(TEXT("Head"));
				const FQuat HeadRel = WH >= 0 ? ModelCache[WH].GetRotation() * RestModel[WH].GetRotation().Inverse() : FQuat::Identity;
				const FQuat Open = HeadRel.Inverse() * Rel;
				FVector Ax; float Ang; Open.ToAxisAndAngle(Ax, Ang);
				Q = FQuat(FVector(1, 0, 0), FMath::DegreesToRadians(Close) - FMath::Abs(Ang)) ;
			}
			R.Rot[FJaw] = Q;
		}
	}
	R.Evaluate();
	R.PreBone = nullptr;
	if (FParse::Param(FCommandLine::Get(), TEXT("FuloDebug")))
	{
		static int32 Frames = 0;
		if (++Frames == 5 || Frames == 60)
		{
			FString Out;
			for (const TCHAR* N : { TEXT("Jaw.1"), TEXT("Jaw.2"), TEXT("Head.1"), TEXT("HeadControl.1"), TEXT("WArm.L"), TEXT("WingFold.L") })
			{
				const int32 I = R.Find(N);
				if (I < 0) continue;
				const FQuat Local = (R.PreMat(I).Inverse() * R.PoseMat(I)).Rot();
				Out += FString::Printf(TEXT("%s: rot %s local %s  ue %d  loc %s\n"), N, *R.Rot[I].ToString(), *Local.ToString(), FuloToUE[I], *R.Head(I).ToString());
			}
			FFileHelper::SaveStringToFile(Out, *(FPaths::ProjectSavedDir() / TEXT("FuloDebug.txt")));
		}
	}

	// out to the mesh
	const int32 NU = UEParent.Num();
	if (Mesh->BoneSpaceTransforms.Num() != NU) return;
	for (int32 u = 0; u < NU; ++u)
	{
		const int32 F = UEToFulo[u], Par = UEParent[u];
		if (F >= 0) UEAnimCS[u] = ToUE(FuloC * R.PoseMat(F) * FuloK[F]);
		else UEAnimCS[u] = Par == INDEX_NONE ? UERefLocal[u] : UERefLocal[u] * UEAnimCS[Par];
		Mesh->BoneSpaceTransforms[u] = Par == INDEX_NONE ? UEAnimCS[u] : UEAnimCS[u].GetRelativeTransform(UEAnimCS[Par]);
	}
	Mesh->MarkRefreshTransformDirty();
}

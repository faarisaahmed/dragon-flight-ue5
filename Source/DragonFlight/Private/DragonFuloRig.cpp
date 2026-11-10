#include "DragonFuloRig.h"
#include "DragonFuloRigData.inl"

// ===========================================================================
// FFuloM4
// ===========================================================================
FFuloM4 FFuloM4::Identity()
{
	FFuloM4 R;
	for (int32 r = 0; r < 4; ++r) for (int32 c = 0; c < 4; ++c) R.M[r][c] = r == c ? 1.0 : 0.0;
	return R;
}

FFuloM4 FFuloM4::FromRows(const float* A)
{
	FFuloM4 R;
	for (int32 r = 0; r < 4; ++r) for (int32 c = 0; c < 4; ++c) R.M[r][c] = A[r * 4 + c];
	return R;
}

static void RotCols(const FQuat& Q, FVector& X, FVector& Y, FVector& Z)
{
	X = Q.RotateVector(FVector(1, 0, 0));
	Y = Q.RotateVector(FVector(0, 1, 0));
	Z = Q.RotateVector(FVector(0, 0, 1));
}

FFuloM4 FFuloM4::FromLRS(const FVector& L, const FQuat& Q, const FVector& S)
{
	FFuloM4 R = Identity();
	FVector X, Y, Z;
	RotCols(Q, X, Y, Z);
	R.SetCol(0, X * S.X); R.SetCol(1, Y * S.Y); R.SetCol(2, Z * S.Z); R.SetCol(3, L);
	return R;
}

FFuloM4 FFuloM4::FromRot(const FQuat& Q, const FVector& Pivot)
{
	// rotate about Pivot: T(p) R T(-p)
	FFuloM4 R = FromLRS(FVector::ZeroVector, Q, FVector::OneVector);
	R.SetCol(3, Pivot - Q.RotateVector(Pivot));
	return R;
}

FFuloM4 FFuloM4::operator*(const FFuloM4& B) const
{
	FFuloM4 R;
	for (int32 r = 0; r < 4; ++r)
		for (int32 c = 0; c < 4; ++c)
			R.M[r][c] = M[r][0] * B.M[0][c] + M[r][1] * B.M[1][c] + M[r][2] * B.M[2][c] + M[r][3] * B.M[3][c];
	return R;
}

FVector FFuloM4::TransformPosition(const FVector& P) const
{
	return FVector(M[0][0] * P.X + M[0][1] * P.Y + M[0][2] * P.Z + M[0][3],
	               M[1][0] * P.X + M[1][1] * P.Y + M[1][2] * P.Z + M[1][3],
	               M[2][0] * P.X + M[2][1] * P.Y + M[2][2] * P.Z + M[2][3]);
}

FVector FFuloM4::TransformVector(const FVector& V) const
{
	return FVector(M[0][0] * V.X + M[0][1] * V.Y + M[0][2] * V.Z,
	               M[1][0] * V.X + M[1][1] * V.Y + M[1][2] * V.Z,
	               M[2][0] * V.X + M[2][1] * V.Y + M[2][2] * V.Z);
}

FQuat FFuloM4::Rot() const
{
	const FVector X = Col(0).GetSafeNormal(), Y = Col(1).GetSafeNormal();
	// re-orthogonalize (stretch-to and IK can leave a little skew): keep Y, rebuild Z and X
	const FVector Z = FVector::CrossProduct(X, Y).GetSafeNormal();
	const FVector X2 = FVector::CrossProduct(Y, Z);
	const FMatrix RM(FPlane(X2, 0), FPlane(Y, 0), FPlane(Z, 0), FPlane(0, 0, 0, 1));
	return FQuat(RM).GetNormalized();
}

FFuloM4 FFuloM4::Inverse() const
{
	// affine: [A t; 0 1]^-1 = [A^-1, -A^-1 t]
	const double a = M[0][0], b = M[0][1], c = M[0][2], d = M[1][0], e = M[1][1], f = M[1][2], g = M[2][0], h = M[2][1], i = M[2][2];
	const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	const double det = a * A + b * B + c * C;
	FFuloM4 R = Identity();
	if (FMath::Abs(det) < 1e-12) return R;
	const double id = 1.0 / det;
	R.M[0][0] = A * id; R.M[0][1] = -(b * i - c * h) * id; R.M[0][2] = (b * f - c * e) * id;
	R.M[1][0] = B * id; R.M[1][1] = (a * i - c * g) * id;  R.M[1][2] = -(a * f - c * d) * id;
	R.M[2][0] = C * id; R.M[2][1] = -(a * h - b * g) * id; R.M[2][2] = (a * e - b * d) * id;
	const FVector T = Loc();
	for (int32 r = 0; r < 3; ++r) R.M[r][3] = -(R.M[r][0] * T.X + R.M[r][1] * T.Y + R.M[r][2] * T.Z);
	return R;
}

namespace
{
	enum EType { TCopyRot = 1, TAction = 2, TStretch = 3, TDamped = 4, TCopyXf = 5, TIK = 6 };
	enum ESpace { SWorld = 0, SLocal = 1, SParLocal = 2 };
	enum EMix { MReplace = 0, MAdd = 1, MBefore = 2, MAfter = 3, MBeforeSplit = 5, MAfterSplit = 6, MBeforeFull = 7, MAfterFull = 8 };

	FQuat FromBlenderQ(const float* W) { return FQuat(W[1], W[2], W[3], W[0]).GetNormalized(); }

	FFuloM4 Blend(const FFuloM4& A, const FFuloM4& B, float T)
	{
		FVector La, Sa, Lb, Sb; FQuat Qa, Qb;
		A.ToLRS(La, Qa, Sa); B.ToLRS(Lb, Qb, Sb);
		return FFuloM4::FromLRS(FMath::Lerp(La, Lb, T), FQuat::Slerp(Qa, Qb, T).GetNormalized(), FMath::Lerp(Sa, Sb, T));
	}

	/** Rotate M's axes about its own location so its Y points along Dir (shortest arc), keeping scale. */
	void SwingY(FFuloM4& M, const FVector& Dir)
	{
		const FVector S = M.Size();
		const FQuat R = M.Rot();
		const FVector Y = R.RotateVector(FVector(0, 1, 0));
		const FQuat Sw = FQuat::FindBetweenNormals(Y, Dir.GetSafeNormal());
		M = FFuloM4::FromLRS(M.Loc(), (Sw * R).GetNormalized(), S);
	}
}

// ===========================================================================
// Rig
// ===========================================================================
float FDragonFuloRig::ObjectScale() { return GFuloObjectScale; }

bool FDragonFuloRig::Init()
{
	const int32 N = UE_ARRAY_COUNT(GFuloBones);
	Names.SetNum(N); Parents.SetNum(N); Deform.SetNum(N); Rest.SetNum(N); RestInv.SetNum(N); Offs.SetNum(N);
	Pose.SetNum(N); Pre.SetNum(N); Lengths.SetNum(N); FileLoc.SetNum(N); FileRot.SetNum(N); FileScale.SetNum(N);
	Loc.SetNum(N); Rot.SetNum(N); Scale.SetNum(N); Cons.SetNum(N);
	ByName.Reset();
	for (int32 i = 0; i < N; ++i)
	{
		const FFuloBoneDef& D = GFuloBones[i];
		Names[i] = UTF8_TO_TCHAR(D.Name);
		ByName.Add(Names[i], i);
		Parents[i] = D.Parent;
		Deform[i] = D.Deform;
		Rest[i] = FFuloM4::FromRows(D.Rest);
		RestInv[i] = Rest[i].Inverse();
		Lengths[i] = D.Length;
		FileLoc[i] = FVector(D.Loc[0], D.Loc[1], D.Loc[2]);
		FileRot[i] = FromBlenderQ(D.Rot);
		FileScale[i] = FVector(D.Scale[0], D.Scale[1], D.Scale[2]);
	}
	for (int32 i = 0; i < N; ++i) Offs[i] = Parents[i] >= 0 ? RestInv[Parents[i]] * Rest[i] : Rest[i];

	Actions.Reset();
	for (const FFuloActionDef& A : GFuloActions) { FAction X; X.FrameStart = A.FrameStart; X.FrameEnd = A.FrameEnd; Actions.Add(X); }
	for (const FFuloConDef& D : GFuloCons)
	{
		if (D.Mute) continue;
		FCon C;
		C.Type = D.Type; C.Target = D.Target; C.OwnerSpace = D.OwnerSpace; C.TargetSpace = D.TargetSpace; C.Mix = D.Mix;
		C.Influence = D.Influence; C.HeadTail = D.HeadTail; C.Action = D.Action; C.Channel = D.Channel;
		C.FrameStart = D.FrameStart; C.FrameEnd = D.FrameEnd; C.Min = D.Min; C.Max = D.Max;
		C.RestLength = D.RestLength; C.Bulge = D.Bulge; C.Volume = D.Volume; C.ChainCount = D.ChainCount; C.Pole = D.Pole; C.bUseTail = D.UseTail;
		if (C.Type == TAction && C.Action >= 0)
		{
			const FFuloActionDef& A = GFuloActions[C.Action];
			for (int32 t = 0; t < A.NumTracks; ++t)
				if (GFuloTracks[A.FirstTrack + t].Bone == D.Owner) { C.Track = A.FirstTrack + t; break; }
			if (C.Track == INDEX_NONE) continue;   // the action does not move this bone
		}
		// a target that is the owner's own descendant is a dependency cycle Blender skips
		if (C.Target >= 0 && (C.Target == D.Owner || IsDescendant(C.Target, D.Owner)) && C.Type != TIK) continue;
		if ((C.Type == TCopyRot || C.Type == TCopyXf || C.Type == TStretch || C.Type == TDamped || C.Type == TAction || C.Type == TIK) && C.Target < 0) continue;
		Cons[D.Owner].Add(C);
	}

	// evaluation order: a bone after its parent and after every bone its constraints read
	Order.Reset();
	TArray<uint8> Mark; Mark.Init(0, N);
	TFunction<void(int32)> Visit = [&](int32 i)
	{
		if (Mark[i] == 2) return;
		if (Mark[i] == 1) return;   // cycle: leave it
		Mark[i] = 1;
		if (Parents[i] >= 0) Visit(Parents[i]);
		for (const FCon& C : Cons[i])
		{
			if (C.Target >= 0 && !IsDescendant(C.Target, i)) Visit(C.Target);
			if (C.Pole >= 0) Visit(C.Pole);
		}
		Mark[i] = 2;
		Order.Add(i);
	};
	for (int32 i = 0; i < N; ++i) Visit(i);
	ResetPose();
	Evaluate();
	return true;
}

int32 FDragonFuloRig::Find(const FString& Name) const
{
	const int32* I = ByName.Find(Name);
	return I ? *I : INDEX_NONE;
}

bool FDragonFuloRig::IsDescendant(int32 I, int32 Of) const
{
	for (int32 p = Parents.IsValidIndex(I) ? Parents[I] : -1; p >= 0; p = Parents[p]) if (p == Of) return true;
	return false;
}

void FDragonFuloRig::ResetPose()
{
	Loc = FileLoc; Rot = FileRot; Scale = FileScale;
}

FFuloM4 FDragonFuloRig::ToSpace(int32 Bone, const FFuloM4& P, int32 Space) const
{
	if (Space == SLocal) return Pre[Bone].Inverse() * P;
	if (Space == SParLocal) return RestInv[Bone] * P;
	return P;
}

FFuloM4 FDragonFuloRig::FromSpace(int32 Bone, const FFuloM4& M, int32 Space) const
{
	if (Space == SLocal) return Pre[Bone] * M;
	if (Space == SParLocal) return Rest[Bone] * M;
	return M;
}

FFuloM4 FDragonFuloRig::ActionMatrix(const FCon& C) const
{
	const FFuloM4 T = ToSpace(C.Target, Pose[C.Target], C.TargetSpace);
	double V = 0.0;
	switch (C.Channel)
	{
	case 0: V = T.M[0][3]; break;
	case 1: V = T.M[1][3]; break;
	case 2: V = T.M[2][3]; break;
	case 6: V = T.Col(0).Size(); break;
	case 7: V = T.Col(1).Size(); break;
	case 8: V = T.Col(2).Size(); break;
	default: V = 0.0; break;
	}
	double S = FMath::Abs(C.Max - C.Min) > 1e-9 ? (V - C.Min) / (C.Max - C.Min) : 0.0;
	S = FMath::Clamp(S, 0.0, 1.0);
	const double F = C.FrameStart + S * (C.FrameEnd - C.FrameStart);
	const FFuloActionDef& A = GFuloActions[C.Action];
	const int32 NF = A.FrameEnd - A.FrameStart + 1;
	const double K = FMath::Clamp(F - A.FrameStart, 0.0, double(NF - 1));
	const int32 K0 = FMath::FloorToInt32(K), K1 = FMath::Min(K0 + 1, NF - 1);
	const float W = float(K - K0);
	const float* P0 = &GFuloKeys[(GFuloTracks[C.Track].FirstKey + K0) * 10];
	const float* P1 = &GFuloKeys[(GFuloTracks[C.Track].FirstKey + K1) * 10];
	const FVector L = FMath::Lerp(FVector(P0[0], P0[1], P0[2]), FVector(P1[0], P1[1], P1[2]), W);
	const FQuat Q = FQuat::Slerp(FromBlenderQ(P0 + 3), FromBlenderQ(P1 + 3), W).GetNormalized();
	const FVector Sc = FMath::Lerp(FVector(P0[7], P0[8], P0[9]), FVector(P1[7], P1[8], P1[9]), W);
	return FFuloM4::FromLRS(L, Q, Sc);
}

FFuloM4 FDragonFuloRig::ActionLocal(int32 I) const
{
	FFuloM4 R = FFuloM4::Identity();
	for (const FCon& C : Cons[I])
		if (C.Type == TAction) R = ActionMatrix(C) * R;   // each BEFORE: applied ahead of what is there
	return R;
}

void FDragonFuloRig::Apply(int32 Bone, const FCon& C, FFuloM4& P)
{
	const FFuloM4 Before = P;
	switch (C.Type)
	{
	case TCopyRot:
	{
		const FFuloM4 O = ToSpace(Bone, P, C.OwnerSpace);
		const FFuloM4 T = ToSpace(C.Target, Pose[C.Target], C.TargetSpace);
		FVector L, S; FQuat Qo;
		O.ToLRS(L, Qo, S);
		const FQuat Qt = T.Rot();
		FQuat Qn = Qo;
		if (C.Mix == MAfter) Qn = Qo * Qt;
		else if (C.Mix == MBefore) Qn = Qt * Qo;
		else Qn = Qt;
		P = FromSpace(Bone, FFuloM4::FromLRS(L, Qn.GetNormalized(), S), C.OwnerSpace);
		break;
	}
	case TAction:
	{
		const FFuloM4 A = ActionMatrix(C);
		const FFuloM4 O = ToSpace(Bone, P, C.OwnerSpace);
		FVector La, Sa, Lo, So; FQuat Qa, Qo;
		A.ToLRS(La, Qa, Sa); O.ToLRS(Lo, Qo, So);
		const FFuloM4 LR = FFuloM4::FromLRS(La, Qa, FVector::OneVector) * FFuloM4::FromLRS(Lo, Qo, FVector::OneVector);
		const FFuloM4 R = (C.Mix == MAfterSplit)
			? FFuloM4::FromLRS(Lo, Qo, FVector::OneVector) * FFuloM4::FromLRS(La, Qa, FVector::OneVector)
			: LR;
		P = FromSpace(Bone, FFuloM4::FromLRS(R.Loc(), R.Rot(), Sa * So), C.OwnerSpace);
		break;
	}
	case TStretch:
	{
		// Blender stretchto_evaluate, SWING_Y keep-axis, world space (armature space times the object's scale)
		const FVector Size = P.Size();
		const FVector TgtHead = Pose[C.Target].Loc();
		const FVector TgtPt = C.HeadTail > 0.f ? FMath::Lerp(TgtHead, Tail(C.Target), C.HeadTail) : TgtHead;
		const FVector Vec = TgtPt - P.Loc();
		double Dist = Vec.Size() * GFuloObjectScale;
		if (Dist < 1e-9) break;
		Dist /= FMath::Max(Size.Y, 1e-6);
		FCon& Mut = const_cast<FCon&>(C);
		if (Mut.RestLength <= 0.f) Mut.RestLength = Dist;
		const double SY = Dist / Mut.RestLength;
		double Bulge = FMath::Pow(Mut.RestLength / Dist, double(C.Bulge));
		double SX = 1.0, SZ = 1.0;
		switch (C.Volume)
		{
		case 0: SX = SZ = FMath::Sqrt(Bulge); break;   // VOLUME_XZX
		case 1: SX = Bulge; break;
		case 2: SZ = Bulge; break;
		default: break;
		}
		FFuloM4 R = P;
		SwingY(R, Vec);
		const FQuat Q = R.Rot();
		P = FFuloM4::FromLRS(P.Loc(), Q, FVector(Size.X * SX, Size.Y * SY, Size.Z * SZ));
		break;
	}
	case TDamped:
	{
		const FVector TgtHead = Pose[C.Target].Loc();
		const FVector TgtPt = C.HeadTail > 0.f ? FMath::Lerp(TgtHead, Tail(C.Target), C.HeadTail) : TgtHead;
		const FVector Vec = TgtPt - P.Loc();
		if (Vec.SizeSquared() < 1e-12) break;
		SwingY(P, Vec);
		break;
	}
	case TCopyXf:
	{
		if (C.OwnerSpace != SWorld || C.TargetSpace != SWorld) break;   // the local variants are muted in the file
		FFuloM4 T = Pose[C.Target];
		if (C.HeadTail > 0.f) T.SetCol(3, FMath::Lerp(T.Loc(), Tail(C.Target), C.HeadTail));
		P = T;
		break;
	}
	case TIK:
		SolveIK(Bone, C);
		P = Pose[Bone];
		break;
	default: break;
	}
	if (C.Influence < 0.999f && C.Type != TIK) P = Blend(Before, P, C.Influence);
}

void FDragonFuloRig::SolveIK(int32 Tip, const FCon& C)
{
	// The chain: Tip and its ChainCount-1 ancestors, root first.
	TArray<int32> Chain;
	for (int32 b = Tip; b >= 0 && Chain.Num() < FMath::Max(1, C.ChainCount); b = Parents[b]) Chain.Insert(b, 0);
	const int32 N = Chain.Num();
	if (N < 2) return;
	TArray<FVector> P; P.SetNum(N + 1);
	TArray<double> Len; Len.SetNum(N);
	for (int32 k = 0; k < N; ++k) P[k] = Pose[Chain[k]].Loc();
	P[N] = Tail(Chain[N - 1]);
	for (int32 k = 0; k < N; ++k) Len[k] = FVector::Dist(P[k], P[k + 1]);
	const FVector Target = Pose[C.Target].Loc();
	const FVector Root = P[0];
	// the pole fixes the plane the joints bend in
	FVector PlaneN = FVector::ZeroVector;
	if (C.Pole >= 0)
	{
		PlaneN = FVector::CrossProduct(Target - Root, Pose[C.Pole].Loc() - Root).GetSafeNormal();
	}
	auto ToPlane = [&](FVector& X) { if (!PlaneN.IsZero()) X -= PlaneN * FVector::DotProduct(X - Root, PlaneN); };
	for (int32 k = 1; k <= N; ++k) ToPlane(P[k]);
	for (int32 It = 0; It < 12; ++It)
	{
		P[N] = Target;
		for (int32 k = N - 1; k >= 0; --k) P[k] = P[k + 1] + (P[k] - P[k + 1]).GetSafeNormal() * Len[k];
		P[0] = Root;
		for (int32 k = 0; k < N; ++k) { P[k + 1] = P[k] + (P[k + 1] - P[k]).GetSafeNormal() * Len[k]; if (k + 1 < N) ToPlane(P[k + 1]); }
		if (FVector::DistSquared(P[N], Target) < 1e-8) break;
	}
	// rotate each bone onto its new segment, carrying its children's local poses down the chain
	for (int32 k = 0; k < N; ++k)
	{
		const int32 B = Chain[k];
		const FFuloM4 OldNext = k + 1 < N ? Pose[Chain[k + 1]] : FFuloM4::Identity();
		const FFuloM4 OldLocalNext = k + 1 < N ? Pre[Chain[k + 1]].Inverse() * OldNext : FFuloM4::Identity();
		FFuloM4 M = Pose[B];
		M.SetCol(3, P[k]);
		const FVector Seg = P[k + 1] - P[k];
		if (!Seg.IsNearlyZero())
		{
			// the bone's head-to-next direction (its Y, scaled) onto the solved segment
			const FVector Cur = (k + 1 < N ? OldNext.Loc() : Tail(B)) - Pose[B].Loc();
			const FQuat Sw = FQuat::FindBetweenVectors(Cur, Seg);
			M = FFuloM4::FromLRS(P[k], (Sw * M.Rot()).GetNormalized(), M.Size());
		}
		Pose[B] = M;
		if (k + 1 < N)
		{
			const int32 Nx = Chain[k + 1];
			Pre[Nx] = Pose[B] * Offs[Nx];
			Pose[Nx] = Pre[Nx] * OldLocalNext;
		}
	}
}

void FDragonFuloRig::Evaluate()
{
	for (int32 i : Order)
	{
		const int32 P = Parents[i];
		Pre[i] = P >= 0 ? Pose[P] * Offs[i] : Rest[i];
		if (PreBone) PreBone(i);
		FFuloM4 M = Pre[i] * FFuloM4::FromLRS(Loc[i], Rot[i].GetNormalized(), Scale[i]);
		Pose[i] = M;
		for (const FCon& C : Cons[i])
		{
			Apply(i, C, M);
			Pose[i] = M;
		}
	}
}

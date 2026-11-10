#pragma once

#include "CoreMinimal.h"

/** 4x4 in Blender's convention: column vectors, M[row][col], translation in column 3. */
struct FFuloM4
{
	double M[4][4];
	static FFuloM4 Identity();
	static FFuloM4 FromRows(const float* RowMajor16);
	/** T * R * S, as Blender builds a pose channel's basis. */
	static FFuloM4 FromLRS(const FVector& L, const FQuat& Q, const FVector& S);
	static FFuloM4 FromRot(const FQuat& Q, const FVector& Pivot);
	FFuloM4 operator*(const FFuloM4& B) const;
	FVector TransformPosition(const FVector& P) const;
	FVector TransformVector(const FVector& V) const;
	FVector Col(int32 C) const { return FVector(M[0][C], M[1][C], M[2][C]); }
	void SetCol(int32 C, const FVector& V) { M[0][C] = V.X; M[1][C] = V.Y; M[2][C] = V.Z; }
	FVector Loc() const { return Col(3); }
	FVector Size() const { return FVector(Col(0).Size(), Col(1).Size(), Col(2).Size()); }
	/** The rotation of the normalized axes (a proper rotation is assumed). */
	FQuat Rot() const;
	void ToLRS(FVector& L, FQuat& Q, FVector& S) const { L = Loc(); S = Size(); Q = Rot(); }
	/** General affine inverse (scale and shear allowed). */
	FFuloM4 Inverse() const;
};

/**
 * The Night Fury rig by Fulonimation Studios, evaluated the way Blender evaluates it:
 * pose channels (location, rotation, scale per bone) through the bone hierarchy, then each
 * bone's constraints in order (copy rotation, action, stretch-to, damped track, copy transforms,
 * IK). The game drives the rig's own controls (wing fold, body/head controls, foot IK targets...)
 * and, through PreBone, any bone directly; the constraints do the rest, as in the source file.
 */
class DRAGONFLIGHT_API FDragonFuloRig
{
public:
	bool Init();
	/** World (constraint) units per armature unit: the armature object's scale in the source file. */
	static float ObjectScale();
	bool IsValid() const { return Rest.Num() > 0; }
	int32 Num() const { return Rest.Num(); }
	int32 Find(const FString& Name) const;
	const FString& Name(int32 I) const { return Names[I]; }
	int32 Parent(int32 I) const { return Parents[I]; }
	bool IsDeform(int32 I) const { return Deform[I]; }
	bool IsDescendant(int32 I, int32 Of) const;

	/** Back to the file's pose (the creator's pose of the controls). */
	void ResetPose();
	/** Pose channels: Blender basis (location in the bone's rest frame, rotation, scale). */
	TArray<FVector> Loc;
	TArray<FQuat> Rot;
	TArray<FVector> Scale;

	/** Called for each bone once its parent is final and before its own pose; may set its channels. */
	TFunction<void(int32 Bone)> PreBone;
	void Evaluate();

	const FFuloM4& RestMat(int32 I) const { return Rest[I]; }
	const FFuloM4& PoseMat(int32 I) const { return Pose[I]; }
	/** The bone's frame before its own channels: parent's final pose * rest offset (or its rest, for a root). */
	const FFuloM4& PreMat(int32 I) const { return Pre[I]; }
	/** The bone's action constraints at the current control values, as one local matrix (applied before its channels). */
	FFuloM4 ActionLocal(int32 I) const;
	float BoneLength(int32 I) const { return Lengths[I]; }
	FVector Head(int32 I) const { return Pose[I].Loc(); }
	FVector Tail(int32 I) const { return Pose[I].TransformPosition(FVector(0, Lengths[I], 0)); }
	FVector RestHead(int32 I) const { return Rest[I].Loc(); }
	FVector RestTail(int32 I) const { return Rest[I].TransformPosition(FVector(0, Lengths[I], 0)); }

private:
	struct FCon
	{
		int32 Type = 0, Target = INDEX_NONE, OwnerSpace = 0, TargetSpace = 0, Mix = 0;
		float Influence = 1.f, HeadTail = 0.f;
		int32 Action = INDEX_NONE, Channel = -1, FrameStart = 0, FrameEnd = 0;
		float Min = 0.f, Max = 0.f, RestLength = 0.f, Bulge = 1.f;
		int32 Volume = 0, ChainCount = 0, Pole = INDEX_NONE;
		bool bUseTail = true;
		int32 Track = INDEX_NONE;   // this owner's keys in Action
	};
	struct FAction { int32 FrameStart = 0, FrameEnd = 0; };
	FFuloM4 ToSpace(int32 Bone, const FFuloM4& PoseM, int32 Space) const;
	FFuloM4 FromSpace(int32 Bone, const FFuloM4& M, int32 Space) const;
	FFuloM4 ActionMatrix(const FCon& C) const;
	void Apply(int32 Bone, const FCon& C, FFuloM4& P);
	void SolveIK(int32 Tip, const FCon& C);

	TArray<FString> Names;
	TMap<FString, int32> ByName;
	TArray<int32> Parents;
	TArray<bool> Deform;
	TArray<FFuloM4> Rest, RestInv, Offs, Pose, Pre;
	TArray<float> Lengths;
	TArray<FVector> FileLoc, FileScale;
	TArray<FQuat> FileRot;
	TArray<TArray<FCon>> Cons;
	TArray<FAction> Actions;
	TArray<int32> Order;   // evaluation order: parents and constraint targets first
};

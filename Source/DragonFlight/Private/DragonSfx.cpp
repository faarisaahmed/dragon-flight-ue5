#include "DragonSfx.h"
#include "DragonFlight.h"
#include "DragonSky.h"
#include "DragonWorldSubsystem.h"
#include "Sound/SoundWaveProcedural.h"
#include "Sound/SoundAttenuation.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"

namespace
{
	constexpr int32 RATE = 48000;

	struct FRng
	{
		uint32 S;
		explicit FRng(uint32 Seed) : S(Seed * 2654435761u + 1u) {}
		float U() { S = S * 1664525u + 1013904223u; return ((S >> 8) & 0xFFFF) / 65535.f; }   // 0..1
		float N() { return U() * 2.f - 1.f; }                                                  // -1..1
	};

	/** One-pole low-pass coefficient for a cutoff in Hz. */
	float LpK(float Hz) { return 1.f - FMath::Exp(-2.f * PI * FMath::Max(Hz, 1.f) / RATE); }

	void Normalise(TArray<float>& X, float Peak)
	{
		float M = 1e-6f;
		for (float V : X) M = FMath::Max(M, FMath::Abs(V));
		const float G = Peak / M;
		for (float& V : X) V *= G;
	}

	TArray<int16> ToPCM(const TArray<float>& X)
	{
		TArray<int16> Out; Out.SetNumUninitialized(X.Num());
		// soft clip so the loudest moments saturate like a real recording, not a digital edge
		for (int32 i = 0; i < X.Num(); ++i) Out[i] = (int16)(FMath::Clamp(FMath::Tanh(X[i] * 1.2f) / FMath::Tanh(1.2f), -1.f, 1.f) * 30000.f);
		return Out;
	}

	// ---- Plasma shot -------------------------------------------------------------
	TArray<int16> MakeShot(uint32 Seed)
	{
		FRng R(Seed);
		const float Len = 1.5f;
		const int32 N = (int32)(Len * RATE);
		TArray<float> X; X.SetNumZeroed(N);
		double Ph = 0.0;
		float Lp = 0.f, Lp2 = 0.f, Hp = 0.f, Body = 0.f, BodyPh = 0.f, BN = 0.f;
		const float Top = 2500.f + 400.f * R.N(), Bottom = 650.f + 120.f * R.N();
		for (int32 i = 0; i < N; ++i)
		{
			const float t = (float)i / RATE;
			// pitch: up in the first 0.1 s (the charge leaving), then a long fall as it flies away
			const float F = t < 0.10f ? FMath::Lerp(1300.f, Top, t / 0.10f) : Bottom + (Top - Bottom) * FMath::Exp(-(t - 0.10f) * 2.8f);
			const float Vib = 1.f + 0.018f * FMath::Sin(t * 2.f * PI * 9.f) + 0.006f * R.N();
			Ph += 2.0 * PI * F * Vib / RATE;
			const float Tone = FMath::Sin(Ph) + 0.4f * FMath::Sin(2.0 * Ph + 0.3) + 0.18f * FMath::Sin(3.0 * Ph + 1.1);
			// breath: noise ring-modulated onto the pitch, so the shriek is airy, not a pure whistle
			const float Wn = R.N();
			Lp += (Wn - Lp) * LpK(F * 0.6f);
			const float Breath = Lp * FMath::Sin(Ph * 1.0) * 2.4f;
			const float Env = FMath::Clamp(t / 0.035f, 0.f, 1.f) * FMath::Exp(-t * 2.1f);
			float S = (Tone * 0.55f + Breath * 0.6f) * Env;
			// charge crackle in the first 80 ms
			if (t < 0.09f && R.U() < 0.02f) S += R.N() * 0.9f * (1.f - t / 0.09f);
			// the body: a low whoomp of displaced air
			BodyPh += 2.f * PI * FMath::Lerp(120.f, 55.f, FMath::Min(1.f, t / 0.18f)) / RATE;
			BN += (R.N() - BN) * LpK(700.f);
			Body = (FMath::Sin(BodyPh) * 0.9f + BN * 1.6f) * FMath::Exp(-t * 14.f) * FMath::Clamp(t / 0.004f, 0.f, 1.f);
			// fizz: high crackle trailing the bolt
			const float Hn = R.N(); Hp += (Hn - Hp) * LpK(3000.f); Lp2 += ((Hn - Hp) - Lp2) * LpK(9000.f);
			const float Fizz = Lp2 * 0.35f * FMath::Exp(-t * 3.5f) * (R.U() < 0.25f ? 1.f : 0.3f);
			X[i] = S + Body + Fizz;
		}
		Normalise(X, 0.95f);
		return ToPCM(X);
	}

	// ---- Blast ------------------------------------------------------------------
	TArray<int16> MakeBlast(uint32 Seed, bool bWater)
	{
		FRng R(Seed);
		const float Len = bWater ? 3.5f : 4.5f;
		const int32 N = (int32)(Len * RATE);
		TArray<float> X; X.SetNumZeroed(N);
		float Brown = 0.f, RLp = 0.f, RLp2 = 0.f, BoomPh = 0.f, CrackLp = 0.f, Hiss = 0.f, HissLp = 0.f, Swell = 1.f, SwellT = 1.f;
		int32 SwellTimer = 0;
		float Click = 0.f;
		for (int32 i = 0; i < N; ++i)
		{
			const float t = (float)i / RATE;
			const float W = R.N();
			// crack: the first instant, broadband
			CrackLp += (W - CrackLp) * 0.6f;
			const float Crack = CrackLp * FMath::Exp(-t / 0.007f) * 1.4f;
			// boom: a falling sub tone
			BoomPh += 2.f * PI * FMath::Lerp(72.f, 34.f, FMath::Min(1.f, t / 0.6f)) / RATE;
			const float Boom = FMath::Sin(BoomPh) * FMath::Exp(-t * 2.4f) * FMath::Clamp(t / 0.006f, 0.f, 1.f) * 1.1f;
			// rumble: brown noise through a closing low-pass, in uneven swells (echoes off the hills)
			Brown = Brown * 0.997f + W * 0.06f;
			const float Cut = FMath::Lerp(1400.f, 110.f, FMath::Min(1.f, t / 1.6f));
			RLp += (Brown - RLp) * LpK(Cut); RLp2 += (RLp - RLp2) * LpK(Cut * 1.3f);
			if (--SwellTimer <= 0) { SwellT = 0.55f + 0.6f * R.U(); SwellTimer = (int32)(RATE * (0.15f + 0.35f * R.U())); }
			Swell += (SwellT - Swell) * 0.0004f;
			const float Rumble = RLp2 * 9.f * FMath::Exp(-t * 0.95f) * Swell * FMath::Clamp(t / 0.02f, 0.f, 1.f);
			float S = Crack + Boom + Rumble;
			if (!bWater)
			{
				// debris: clods and stones landing, thinning out
				const float Rate = 260.f * FMath::Exp(-t * 2.2f) + 3.f * FMath::Exp(-t * 0.6f);
				if (t > 0.15f && R.U() < Rate / RATE) Click = (0.3f + 0.5f * R.U()) * FMath::Exp(-t * 0.9f);
				Click *= 0.992f;
				S += Click * R.N() * 0.6f;
			}
			else
			{
				// the sea thrown up and falling back: a long hiss that peaks as the spray comes down
				Hiss += (W - Hiss) * LpK(5000.f); HissLp += (Hiss - HissLp) * LpK(700.f);
				const float Env = FMath::SmoothStep(0.05f, 0.5f, t) * FMath::Exp(-FMath::Max(0.f, t - 0.8f) * 1.6f);
				S += (Hiss - HissLp) * 1.3f * Env;
			}
			X[i] = S;
		}
		Normalise(X, 0.98f);
		return ToPCM(X);
	}

	// ---- Thunder ----------------------------------------------------------------
	TArray<int16> MakeThunder(uint32 Seed, bool bNear)
	{
		FRng R(Seed);
		const float Len = bNear ? 7.5f : 9.f;
		const int32 N = (int32)(Len * RATE);
		TArray<float> X; X.SetNumZeroed(N);
		float Brown = 0.f, L1 = 0.f, L2 = 0.f, HP = 0.f, Rip = 0.f, Env = 0.f, Target = 0.f;
		int32 Timer = 0;
		for (int32 i = 0; i < N; ++i)
		{
			const float t = (float)i / RATE;
			const float W = R.N();
			Brown = Brown * 0.998f + W * 0.05f;
			const float Cut = bNear ? FMath::Lerp(900.f, 160.f, FMath::Min(1.f, t / 2.f)) : 140.f;
			L1 += (Brown - L1) * LpK(Cut); L2 += (L1 - L2) * LpK(Cut);
			// rolling: bursts of different loudness, the way the stroke's length reaches you in pieces
			if (--Timer <= 0) { Target = FMath::Pow(R.U(), 1.6f) * 1.2f; Timer = (int32)(RATE * (0.12f + 0.5f * R.U())); }
			Env += (Target - Env) * 0.00035f;
			const float Shape = FMath::Clamp(t / (bNear ? 0.05f : 0.6f), 0.f, 1.f) * FMath::Exp(-t * (bNear ? 0.45f : 0.38f));
			float S = L2 * 14.f * (0.35f + Env) * Shape;
			if (bNear)
			{
				// the crack and the tearing sound right after it
				HP += (W - HP) * LpK(2500.f);
				S += (W - HP) * 1.6f * FMath::Exp(-t / 0.012f);
				if (t < 0.9f && R.U() < 0.004f) Rip = 0.7f * R.U();
				Rip *= 0.9985f;
				S += Rip * L1 * 6.f * (1.f - t / 0.9f) * (t < 0.9f ? 1.f : 0.f);
			}
			X[i] = S;
		}
		Normalise(X, bNear ? 0.98f : 0.85f);
		return ToPCM(X);
	}

	// ---- Wingbeat ----------------------------------------------------------------
	TArray<int16> MakeFlap(uint32 Seed)
	{
		// A big membrane wing: a deep whump of displaced air as the stroke drives down, the
		// skin snapping taut at the start, a flutter of the trailing edge, and a soft rush.
		FRng R(Seed);
		const float Len = 0.75f;
		const int32 N = (int32)(Len * RATE);
		TArray<float> X; X.SetNumZeroed(N);
		float L1 = 0.f, L2 = 0.f, B1 = 0.f, Hp = 0.f;
		for (int32 i = 0; i < N; ++i)
		{
			const float t = (float)i / RATE;
			const float W = R.N();
			L1 += (W - L1) * LpK(260.f); L2 += (L1 - L2) * LpK(180.f);
			const float Env = FMath::Pow(FMath::Clamp(t / 0.08f, 0.f, 1.f), 1.5f) * FMath::Exp(-FMath::Max(0.f, t - 0.08f) * 7.f);
			const float Flutter = 0.75f + 0.25f * FMath::Sin(t * 2.f * PI * (34.f + 6.f * R.N() * 0.1f));
			const float Whump = L2 * 16.f * Env * Flutter;
			// the snap of the membrane going taut
			Hp += (W - Hp) * LpK(1800.f);
			const float Snap = (W - Hp) * 0.5f * FMath::Exp(-t / 0.012f) * (t < 0.04f ? 1.f : 0.f);
			// air rush over the trailing edge
			B1 += (W - B1) * LpK(900.f);
			const float Rush = B1 * 0.6f * Env;
			X[i] = Whump + Snap + Rush;
		}
		Normalise(X, 0.9f);
		return ToPCM(X);
	}

	struct FBank
	{
		TArray<TArray<int16>> Shot, Blast, Splash, ThunderNear, ThunderFar, Flap;
		USoundAttenuation* Near = nullptr;   // shots: heard to ~1.5 km
		USoundAttenuation* Far = nullptr;    // blasts: to ~6 km, dull with distance
		uint32 Pick = 1;
	};
	FBank& Bank()
	{
		static FBank B;
		if (B.Shot.Num() == 0)
		{
			for (uint32 s = 0; s < 3; ++s)
			{
				B.Shot.Add(MakeShot(11 + s));
				B.Blast.Add(MakeBlast(21 + s, false));
				B.Splash.Add(MakeBlast(31 + s, true));
				B.ThunderNear.Add(MakeThunder(41 + s, true));
				B.ThunderFar.Add(MakeThunder(51 + s, false));
				B.Flap.Add(MakeFlap(61 + s));
			}
			auto MakeAtt = [](float InnerCm, float FalloffCm, float LpfMaxHz)
			{
				USoundAttenuation* A = NewObject<USoundAttenuation>(GetTransientPackage());
				A->AddToRoot();
				FSoundAttenuationSettings& S = A->Attenuation;
				S.bAttenuate = true; S.bSpatialize = true;
				S.AttenuationShape = EAttenuationShape::Sphere;
				S.AttenuationShapeExtents = FVector(InnerCm, 0, 0);
				S.FalloffDistance = FalloffCm;
				S.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
				S.dBAttenuationAtMax = -50.f;
				S.bAttenuateWithLPF = true;
				S.LPFRadiusMin = InnerCm; S.LPFRadiusMax = InnerCm + FalloffCm;
				S.LPFFrequencyAtMin = 20000.f; S.LPFFrequencyAtMax = LpfMaxHz;
				return A;
			};
			B.Near = MakeAtt(1500.f, 150000.f, 2500.f);
			B.Far = MakeAtt(4000.f, 600000.f, 900.f);
		}
		return B;
	}

	void PlayPCM(const UObject* Ctx, const TArray<int16>& PCM, const FVector& At, float Volume, float Pitch, USoundAttenuation* Att)
	{
		UWorld* W = Ctx ? Ctx->GetWorld() : nullptr;
		if (!W || PCM.Num() == 0) return;
		USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(GetTransientPackage());
		Wave->SetSampleRate(RATE);
		Wave->NumChannels = 1;
		Wave->Duration = (float)PCM.Num() / RATE;
		Wave->bLooping = false;
		Wave->SoundGroup = SOUNDGROUP_Default;
		Wave->QueueAudio(reinterpret_cast<const uint8*>(PCM.GetData()), PCM.Num() * sizeof(int16));
		UGameplayStatics::PlaySoundAtLocation(W, Wave, At, FRotator::ZeroRotator, Volume, Pitch, 0.f, Att);
	}
}

void DragonSfx::PlasmaShot(const UObject* Ctx, const FVector& At)
{
	FBank& B = Bank();
	const int32 i = (B.Pick++) % B.Shot.Num();
	PlayPCM(Ctx, B.Shot[i], At, 0.9f, 0.95f + 0.1f * ((B.Pick * 7) % 10) / 10.f, B.Near);
}

void DragonSfx::PlasmaBlast(const UObject* Ctx, const FVector& At, bool bWater)
{
	FBank& B = Bank();
	const int32 i = (B.Pick++) % B.Blast.Num();
	PlayPCM(Ctx, bWater ? B.Splash[i] : B.Blast[i], At, 1.f, 0.92f + 0.12f * ((B.Pick * 3) % 10) / 10.f, B.Far);
}

void DragonSfx::Thunder(const UObject* Ctx, const FVector& At, float DistanceCm)
{
	FBank& B = Bank();
	const int32 i = (B.Pick++) % B.ThunderNear.Num();
	const bool bNear = DistanceCm < 250000.f;
	// Thunder is heard everywhere: not spatialised by the attenuation, just quieter far away.
	const float Vol = FMath::Clamp(1.2f - DistanceCm / 1500000.f, 0.25f, 1.f);
	UWorld* W = Ctx ? Ctx->GetWorld() : nullptr;
	if (!W) return;
	const TArray<int16>& PCM = bNear ? B.ThunderNear[i] : B.ThunderFar[i];
	USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(GetTransientPackage());
	Wave->SetSampleRate(RATE); Wave->NumChannels = 1; Wave->Duration = (float)PCM.Num() / RATE; Wave->bLooping = false;
	Wave->QueueAudio(reinterpret_cast<const uint8*>(PCM.GetData()), PCM.Num() * sizeof(int16));
	UGameplayStatics::PlaySound2D(W, Wave, Vol, 0.9f + 0.2f * ((B.Pick * 5) % 10) / 10.f);
}

void DragonSfx::Wingbeat(const UObject* Ctx, const FVector& At, float Strength, float Seconds)
{
	if (Strength < 0.05f) return;
	FBank& B = Bank();
	const int32 i = (B.Pick++) % B.Flap.Num();
	// A longer, slower stroke sounds deeper.
	const float Pitch = FMath::Clamp(0.42f / FMath::Max(0.15f, Seconds), 0.7f, 1.35f);
	PlayPCM(Ctx, B.Flap[i], At, FMath::Clamp(Strength, 0.f, 1.f) * 0.8f, Pitch, B.Near);
}

// ===========================================================================
// Ambience
// ===========================================================================
UDragonAmbience::UDragonAmbience()
{
	bAutoActivate = false;
	bAllowSpatialization = false;
	PrimaryComponentTick.bCanEverTick = true;
}

void UDragonAmbience::BeginPlay()
{
	Super::BeginPlay();
	State = MakeShared<FState, ESPMode::ThreadSafe>();
	Wave = NewObject<USoundWaveProcedural>(this);
	Wave->SetSampleRate(RATE);
	Wave->NumChannels = 2;
	Wave->Duration = INDEFINITELY_LOOPING_DURATION;
	Wave->bLooping = true;
	TWeakPtr<FState, ESPMode::ThreadSafe> Weak = State;
	Wave->OnSoundWaveProceduralUnderflow = FOnSoundWaveProceduralUnderflow::CreateLambda([Weak](USoundWaveProcedural* W, int32 SamplesRequired)
	{
		TSharedPtr<FState, ESPMode::ThreadSafe> S = Weak.Pin();
		if (!S || !W) return;
		const int32 Frames = FMath::Max(256, SamplesRequired / 2);
		TArray<int16> Out; Out.SetNumUninitialized(Frames * 2);
		auto Rnd = [&S]() { S->Seed = S->Seed * 1664525u + 1013904223u; return ((S->Seed >> 9) & 0x7FFF) / 16384.f - 1.f; };
		const float TRain = S->Rain.load(std::memory_order_relaxed), TSurf = S->Surf.load(std::memory_order_relaxed);
		const float TSea = S->Sea.load(std::memory_order_relaxed), TStorm = S->Storm.load(std::memory_order_relaxed);
		const float KRainHp = LpK(1100.f), KRainLp = LpK(7500.f), KSurf = LpK(520.f), KSurf2 = LpK(2600.f), KSea = LpK(380.f);
		for (int32 i = 0; i < Frames; ++i)
		{
			S->SmRain += (TRain - S->SmRain) * 0.00002f; S->SmSurf += (TSurf - S->SmSurf) * 0.00002f;
			S->SmSea += (TSea - S->SmSea) * 0.00002f; S->SmStorm += (TStorm - S->SmStorm) * 0.00002f;
			const float NL = Rnd(), NR = Rnd();
			// Rain: a band of hiss, and drops ticking close by.
			S->RainHpL += (NL - S->RainHpL) * KRainHp; S->RainHpR += (NR - S->RainHpR) * KRainHp;
			S->RainLpL += ((NL - S->RainHpL) - S->RainLpL) * KRainLp; S->RainLpR += ((NR - S->RainHpR) - S->RainLpR) * KRainLp;
			if (Rnd() > 1.f - 0.0016f * S->SmRain) { if (Rnd() > 0.f) S->DropL = 0.5f + 0.5f * Rnd(); else S->DropR = 0.5f + 0.5f * Rnd(); }
			S->DropL *= 0.93f; S->DropR *= 0.93f;
			const float RainA = S->SmRain * 0.55f;
			float L = (S->RainLpL + S->DropL * NL * 0.6f) * RainA, R = (S->RainLpR + S->DropR * NR * 0.6f) * RainA;
			// Storm wind: deep brown roar.
			S->BrownL = S->BrownL * 0.998f + NL * 0.04f; S->BrownR = S->BrownR * 0.998f + NR * 0.04f;
			L += S->BrownL * S->SmStorm * 1.4f; R += S->BrownR * S->SmStorm * 1.4f;
			// Surf: waves that build, break and draw back over pebbles, every 6-12 s.
			if (--S->WaveTimer <= 0) { S->WaveTarget = 1.f; S->WaveTimer = (int32)(RATE * (6.f + 6.f * (Rnd() * 0.5f + 0.5f))); }
			S->WaveEnv += (S->WaveTarget - S->WaveEnv) * (S->WaveTarget > S->WaveEnv ? 0.00004f : 0.000012f);
			if (S->WaveEnv > 0.97f) S->WaveTarget = 0.12f;
			S->SurfLp1L += (S->BrownL * 6.f - S->SurfLp1L) * KSurf; S->SurfLp1R += (S->BrownR * 6.f - S->SurfLp1R) * KSurf;
			S->SurfLp2L += (NL - S->SurfLp2L) * KSurf2; S->SurfLp2R += (NR - S->SurfLp2R) * KSurf2;
			const float Crash = S->WaveEnv * S->WaveEnv;
			L += (S->SurfLp1L * (0.3f + Crash) + S->SurfLp2L * 0.35f * Crash) * S->SmSurf;
			R += (S->SurfLp1R * (0.3f + Crash) + S->SurfLp2R * 0.35f * Crash) * S->SmSurf;
			// Open sea: a soft slow wash.
			S->Phase += 1.0 / RATE;
			const float Wash = 0.55f + 0.45f * FMath::Sin(S->Phase * 2.0 * PI / 5.3) * FMath::Sin(S->Phase * 2.0 * PI / 8.9 + 1.0);
			S->SeaLpL += (S->BrownL * 5.f - S->SeaLpL) * KSea; S->SeaLpR += (S->BrownR * 5.f - S->SeaLpR) * KSea;
			L += S->SeaLpL * Wash * S->SmSea * 0.6f; R += S->SeaLpR * Wash * S->SmSea * 0.6f;
			Out[i * 2] = (int16)FMath::Clamp(L * 12000.f, -32000.f, 32000.f);
			Out[i * 2 + 1] = (int16)FMath::Clamp(R * 12000.f, -32000.f, 32000.f);
		}
		W->QueueAudio(reinterpret_cast<const uint8*>(Out.GetData()), Out.Num() * sizeof(int16));
	});
	SetSound(Wave);
	SetVolumeMultiplier(MasterVolume);
	Play();
}

void UDragonAmbience::TickComponent(float Dt, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(Dt, TickType, ThisTickFunction);
	if (!State) return;
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager) return;
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	const UDragonWorldSubsystem* WS = GetWorld()->GetGameInstance() ? GetWorld()->GetGameInstance()->GetSubsystem<UDragonWorldSubsystem>() : nullptr;

	// How much shore is close by: probe a ring around the camera twice a second.
	ProbeTimer -= Dt;
	if (ProbeTimer <= 0.f && WS)
	{
		ProbeTimer = 0.5f;
		int32 Hits = 0, N = 0;
		for (const float Rm : { 40.f, 110.f, 260.f })
			for (int32 k = 0; k < 12; ++k)
			{
				const float A = k * PI / 6.f + Rm;
				const double G = WS->HeightAt(Cam.X + FMath::Cos(A) * Rm * 100.f, Cam.Y + FMath::Sin(A) * Rm * 100.f) / 100.0;
				++N; if (G > -4.0 && G < 3.0) Hits += Rm < 100.f ? 2 : 1;
			}
		ShoreNear = FMath::Clamp(Hits / (N * 0.45f), 0.f, 1.f);
	}
	const double Ground = WS ? WS->HeightAt(Cam.X, Cam.Y) / 100.0 : -50.0;
	const float AltSea = (float)(Cam.Z / 100.0);
	const float Low = 1.f - FMath::SmoothStep(30.f, 260.f, AltSea);
	float Wind = 1.f, Rain = 0.f;
	if (const ADragonSky* Sky = ADragonSky::Get(this)) { Wind = Sky->GetWeatherState().Wind; Rain = Sky->GetWeatherState().Rain; }
	State->Rain.store(Rain, std::memory_order_relaxed);
	State->Storm.store(FMath::Clamp((Wind - 1.3f) / 1.0f, 0.f, 1.f), std::memory_order_relaxed);
	State->Surf.store(ShoreNear * Low * FMath::Clamp(0.5f + 0.4f * Wind, 0.5f, 1.4f), std::memory_order_relaxed);
	State->Sea.store((Ground < -3.0 ? 1.f : 0.3f) * (1.f - FMath::SmoothStep(10.f, 120.f, AltSea)) * (1.f - ShoreNear * 0.5f), std::memory_order_relaxed);
}

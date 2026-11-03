#include "DragonWindAudio.h"
#include "Sound/SoundWaveProcedural.h"

namespace
{
	constexpr int32 RATE = 48000;
}

UDragonWindAudio::UDragonWindAudio()
{
	bAutoActivate = false;
	bAllowSpatialization = false;
	bIsUISound = false;
}

void UDragonWindAudio::BeginPlay()
{
	Super::BeginPlay();
	State = MakeShared<FState, ESPMode::ThreadSafe>();
	Wave = NewObject<USoundWaveProcedural>(this);
	Wave->SetSampleRate(RATE);
	Wave->NumChannels = 2;
	Wave->Duration = INDEFINITELY_LOOPING_DURATION;
	Wave->bLooping = true;
	Wave->SoundGroup = SOUNDGROUP_Default;

	TWeakPtr<FState, ESPMode::ThreadSafe> Weak = State;
	Wave->OnSoundWaveProceduralUnderflow = FOnSoundWaveProceduralUnderflow::CreateLambda([Weak](USoundWaveProcedural* W, int32 SamplesRequired)
	{
		TSharedPtr<FState, ESPMode::ThreadSafe> S = Weak.Pin();
		if (!S || !W) return;
		const int32 Frames = FMath::Max(256, SamplesRequired / 2);
		TArray<int16> Out;
		Out.SetNumUninitialized(Frames * 2);
		const float Target = S->Speed.load(std::memory_order_relaxed);
		const float Turbo = S->Turbo.load(std::memory_order_relaxed);
		auto Rand = [&S]() { S->Seed = S->Seed * 1664525u + 1013904223u; return ((S->Seed >> 9) & 0x7FFF) / 16384.f - 1.f; };
		for (int32 i = 0; i < Frames; ++i)
		{
			S->SmoothSpeed += (Target - S->SmoothSpeed) * 0.00005f;
			const float V = S->SmoothSpeed;
			// Gusts: a new target every couple of seconds, eased toward.
			if (--S->GustTimer <= 0) { S->GustTarget = Rand() * 0.5f + 0.5f; S->GustTimer = (int32)(RATE * (1.2f + (Rand() + 1.f))); }
			S->Gust += (S->GustTarget - S->Gust) * 0.00002f;

			// Rush: white noise through two one-pole lowpasses; brighter as he goes faster.
			const float Cut = FMath::Clamp(0.004f + 0.06f * V * V + 0.05f * Turbo, 0.002f, 0.25f);
			const float NL = Rand(), NR = Rand();
			S->Lp1L += (NL - S->Lp1L) * Cut; S->Lp2L += (S->Lp1L - S->Lp2L) * Cut;
			S->Lp1R += (NR - S->Lp1R) * Cut; S->Lp2R += (S->Lp1R - S->Lp2R) * Cut;
			const float RushAmp = (0.05f + 1.6f * FMath::Pow(V, 1.3f)) * (0.75f + 0.5f * S->Gust) / FMath::Sqrt(Cut * 40.f + 0.05f);

			// Whistle: a narrow band near 1.8-3 kHz, only near flat out and in turbo.
			const float WAmp = FMath::Clamp((V - 0.55f) * 2.f, 0.f, 1.f) * 0.10f + Turbo * 0.18f;
			const float F = 0.24f + 0.12f * Turbo;
			S->BpL += F * (NL - S->BpL - 0.3f * S->BpL2); S->BpL2 += F * S->BpL;
			S->BpR += F * (NR - S->BpR - 0.3f * S->BpR2); S->BpR2 += F * S->BpR;

			const float L = S->Lp2L * RushAmp + S->BpL * WAmp;
			const float R = S->Lp2R * RushAmp + S->BpR * WAmp;
			Out[i * 2] = (int16)FMath::Clamp(L * 9000.f, -32000.f, 32000.f);
			Out[i * 2 + 1] = (int16)FMath::Clamp(R * 9000.f, -32000.f, 32000.f);
		}
		W->QueueAudio(reinterpret_cast<const uint8*>(Out.GetData()), Out.Num() * sizeof(int16));
	});

	SetSound(Wave);
	SetVolumeMultiplier(MasterVolume);
	Play();
}

void UDragonWindAudio::SetFlight(float SpeedT, float TurboT, bool bGrounded)
{
	if (!State) return;
	State->Speed.store(bGrounded ? 0.02f : FMath::Clamp(SpeedT, 0.f, 1.f), std::memory_order_relaxed);
	State->Turbo.store(bGrounded ? 0.f : FMath::Clamp(TurboT, 0.f, 1.f), std::memory_order_relaxed);
}

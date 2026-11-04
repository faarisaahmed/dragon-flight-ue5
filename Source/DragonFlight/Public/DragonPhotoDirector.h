#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DragonPhotoDirector.generated.h"

class ACameraActor;

/**
 * Unattended photo mode, for judging the look without anyone at the screen.
 * Started by the game mode when the command line has -AutoShot. It waits for
 * shader compilation and streaming to settle, then visits a list of views
 * (the chase camera, then island vistas at a few times of day), saves a
 * screenshot at each into Saved/Screenshots/Auto/, and quits.
 *
 *   UnrealEditor DragonFlight.uproject -game -AutoShot [-AutoShotRes=1920x1080] [-RenderOffscreen]
 */
UCLASS()
class DRAGONFLIGHT_API ADragonPhotoDirector : public AActor
{
	GENERATED_BODY()

public:
	ADragonPhotoDirector();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Seconds to let a view settle (streaming, Lumen, exposure) before shooting it. */
	UPROPERTY(EditAnywhere, Category = "Photo") float SettleSeconds = 10.f;

private:
	struct FShot
	{
		FString Name;
		bool bChase = false;    // use the player's own camera
		bool bDragonClose = false; // a close portrait of the dragon
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float Hours = 15.5f;
	};

	void PlanShots();
	bool Busy() const;
	void Frame(const FShot& S);

	TArray<FShot> Shots;
	int32 Index = -1;
	float Timer = 0.f;
	float Warmup = 0.f;
	bool bShotRequested = false;
	bool bGrassBuilt = false;
	FString OutDir;
	FString Tag;

	UPROPERTY(Transient) TObjectPtr<ACameraActor> Camera;
};

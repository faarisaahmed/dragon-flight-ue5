#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BuildArchipelagoCommandlet.generated.h"

/**
 * Headless world build:
 *   UnrealEditor-Cmd DragonFlight.uproject -run=BuildArchipelago [-Res=8129] [-NoModels] [-NoRivers] [-NoTextures]
 */
UCLASS()
class UBuildArchipelagoCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBuildArchipelagoCommandlet();
	virtual int32 Main(const FString& Params) override;
};

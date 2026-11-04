#include "BuildArchipelagoCommandlet.h"
#include "DragonArchipelagoBuilder.h"

UBuildArchipelagoCommandlet::UBuildArchipelagoCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UBuildArchipelagoCommandlet::Main(const FString& Params)
{
	FDragonBuildOptions O;
	FParse::Value(*Params, TEXT("Res="), O.Res);
	O.bModels = !FParse::Param(*Params, TEXT("NoModels"));
	O.bRivers = !FParse::Param(*Params, TEXT("NoRivers"));
	O.bTextures = !FParse::Param(*Params, TEXT("NoTextures"));
	O.bWater = !FParse::Param(*Params, TEXT("NoWater"));
	O.bDragonOnly = FParse::Param(*Params, TEXT("DragonOnly"));
	O.bLandscapeNanite = FParse::Param(*Params, TEXT("LandscapeNanite"));
	O.bVillageOnly = FParse::Param(*Params, TEXT("VillageOnly"));
	return FDragonArchipelagoBuilder::Build(O) ? 0 : 1;
}

#include "Modules/ModuleManager.h"
#include "DragonArchipelagoBuilder.h"
#include "ToolMenus.h"
#include "HAL/IConsoleManager.h"

/**
 * Editor hooks: Tools > Dragon Flight > Build Archipelago, and the console
 * command  Dragon.BuildArchipelago [res] [nomodels] [norivers]
 */
class FDragonFlightEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Command = MakeUnique<FAutoConsoleCommand>(
			TEXT("Dragon.BuildArchipelago"),
			TEXT("Build the archipelago map from Data/. Args: [res 4033|8129|16321] [nomodels] [norivers] [notextures]"),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
			{
				FDragonBuildOptions O;
				for (const FString& A : Args)
				{
					if (A.IsNumeric()) O.Res = FCString::Atoi(*A);
					else if (A.Equals(TEXT("nomodels"), ESearchCase::IgnoreCase)) O.bModels = false;
					else if (A.Equals(TEXT("norivers"), ESearchCase::IgnoreCase)) O.bRivers = false;
					else if (A.Equals(TEXT("notextures"), ESearchCase::IgnoreCase)) O.bTextures = false;
				}
				FDragonArchipelagoBuilder::Build(O);
			}));

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
		{
			UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
			FToolMenuSection& Section = Menu->AddSection(TEXT("DragonFlight"), FText::FromString(TEXT("Dragon Flight")));
			Section.AddMenuEntry(TEXT("BuildArchipelago"),
				FText::FromString(TEXT("Build Archipelago")),
				FText::FromString(TEXT("Import textures and models, build the landscape, water, sky and scatter, and save /Game/Maps/Archipelago.")),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]() { FDragonArchipelagoBuilder::Build(FDragonBuildOptions()); })));
			Section.AddMenuEntry(TEXT("RebuildLevelOnly"),
				FText::FromString(TEXT("Rebuild Level Only")),
				FText::FromString(TEXT("Rebuild the map from already-imported assets (fast).")),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]()
				{
					FDragonBuildOptions O; O.bTextures = false; O.bModels = false;
					FDragonArchipelagoBuilder::Build(O);
				})));
		}));
	}

	virtual void ShutdownModule() override
	{
		Command.Reset();
	}

private:
	TUniquePtr<FAutoConsoleCommand> Command;
};

IMPLEMENT_MODULE(FDragonFlightEditorModule, DragonFlightEditor);

#include "DragonReport.h"
#include "DragonFlight.h"
#include "DragonWorldSubsystem.h"
#include "DragonGameMode.h"
#include "Engine/Engine.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SWindow.h"
#include "Misc/Paths.h"
#include "RHI.h"

namespace
{
	const TCHAR* IssueUrl = TEXT("https://github.com/faarisaahmed/dragon-flight-ue5/issues/new");
	// GitHub refuses very long URLs (~8 KB); the body is cut to fit once encoded.
	constexpr int32 MaxEncodedBody = 7000;

	FString Anonymise(FString S)
	{
		// Paths carry the Windows / macOS user name; reports land in a public repo.
		const FString User = FPlatformProcess::UserName(false);
		if (User.Len() >= 2) S.ReplaceInline(*User, TEXT("<user>"), ESearchCase::IgnoreCase);
		return S;
	}

	FString PendingTitle, PendingBody;

	/** Percent-encoding of the UTF-8 bytes, for a URL query value. */
	FString UrlEncode(const FString& In)
	{
		const FTCHARToUTF8 Utf8(*In);
		FString Out;
		Out.Reserve(Utf8.Length() * 3);
		for (int32 i = 0; i < Utf8.Length(); ++i)
		{
			const uint8 C = (uint8)Utf8.Get()[i];
			if ((C < 128 && FChar::IsAlnum((TCHAR)C)) || C == '-' || C == '_' || C == '.' || C == '~') Out.AppendChar((TCHAR)C);
			else Out += FString::Printf(TEXT("%%%02X"), C);
		}
		return Out;
	}

	FString ReportedPath() { return FPaths::ProjectSavedDir() / TEXT("ReportedCrashes.txt"); }

	/** The previous run's log: UE renames it to <Name>-backup-<date>.log when the game starts. */
	FString PreviousLog()
	{
		TArray<FString> Files;
		const FString Dir = FPaths::ProjectLogDir();
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*-backup-*.log")), true, false);
		FString Best;
		FDateTime BestTime = FDateTime::MinValue();
		for (const FString& F : Files)
		{
			const FDateTime T = IFileManager::Get().GetTimeStamp(*(Dir / F));
			if (T > BestTime) { BestTime = T; Best = Dir / F; }
		}
		return Best;
	}
}

FString DragonReport::BuildId()
{
	FString Id;
	if (FFileHelper::LoadFileToString(Id, *(UDragonWorldSubsystem::DataDir() / TEXT("Build.txt"))))
	{
		Id.TrimStartAndEndInline();
		if (!Id.IsEmpty()) return Id.Left(12);
	}
	return TEXT("dev");
}

FString DragonReport::SystemSummary()
{
	const FPlatformMemoryConstants& Mem = FPlatformMemory::GetConstants();
	const UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	FString Os, OsSub;
	FPlatformMisc::GetOSVersions(Os, OsSub);
	FString L;
	L += FString::Printf(TEXT("- Build: `%s` (%s, engine %s)\n"), *BuildId(), LexToString(FApp::GetBuildConfiguration()), *FEngineVersion::Current().ToString(EVersionComponent::Patch));
	L += FString::Printf(TEXT("- OS: %s %s\n"), *Os, *OsSub);
	L += FString::Printf(TEXT("- CPU: %s (%d cores)\n"), *FPlatformMisc::GetCPUBrand().TrimStartAndEnd(), FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	L += FString::Printf(TEXT("- RAM: %.0f GB\n"), Mem.TotalPhysical / (1024.0 * 1024.0 * 1024.0));
	L += FString::Printf(TEXT("- GPU: %s, driver %s, %s\n"), *GRHIAdapterName, *GRHIAdapterUserDriverVersion, GDynamicRHI ? GDynamicRHI->GetName() : TEXT("?"));
	if (S)
	{
		const FIntPoint R = S->GetScreenResolution();
		L += FString::Printf(TEXT("- Settings: graphics %d, resolution %s, window %dx%d, %s\n"), S->GetOverallScalabilityLevel(),
			*ADragonPlayerController::GetRenderResLabel(), R.X, R.Y,
			(GEngine && GEngine->GameViewport && GEngine->GameViewport->IsFullScreenViewport()) ? TEXT("fullscreen") : TEXT("windowed"));
	}
	return Anonymise(L);
}

void DragonReport::OpenIssue(const FString& Title, const FString& Body, const FString& Label)
{
	FString B = Body;
	FString Enc = UrlEncode(B);
	while (Enc.Len() > MaxEncodedBody && B.Len() > 200)
	{
		// Cut from the middle of the log section, keeping the head (system, error) and the end.
		B = B.Left(B.Len() * 0.8f - 40) + TEXT("\n…(cut to fit)…\n```\n");
		Enc = UrlEncode(B);
	}
	const FString Url = FString::Printf(TEXT("%s?title=%s&labels=%s&body=%s"), IssueUrl,
		*UrlEncode(Title), *UrlEncode(Label), *Enc);
	// -ReportSelfTest: write the link instead of opening it (for testing the reports).
	if (FParse::Param(FCommandLine::Get(), TEXT("ReportSelfTest")))
	{
		FFileHelper::SaveStringToFile(Url, *(FPaths::ProjectSavedDir() / TEXT("ReportSelfTest.txt")));
		return;
	}
	FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
	// Get out of the browser's way (a fullscreen game would cover it).
	if (GEngine && GEngine->GameViewport)
		if (TSharedPtr<SWindow> W = GEngine->GameViewport->GetWindow()) W->Minimize();
}

bool DragonReport::HasPendingCrash() { return !PendingBody.IsEmpty(); }

void DragonReport::SendPendingCrash()
{
	if (PendingBody.IsEmpty()) return;
	OpenIssue(PendingTitle, PendingBody, TEXT("crash"));
	PendingTitle.Reset();
	PendingBody.Reset();
}

void DragonReport::CheckPreviousCrash()
{
	static bool bChecked = false;
	if (bChecked) return;
	bChecked = true;

	const FString Log = PreviousLog();
	if (Log.IsEmpty()) return;
	FString Reported;
	FFileHelper::LoadFileToString(Reported, *ReportedPath());
	const FString Key = FPaths::GetCleanFilename(Log);
	if (Reported.Contains(Key)) return;

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *Log) || Lines.Num() == 0) return;
	// Where the crash is written up: the first crash line in the last part of the log (UE's
	// "Critical error" block, a fatal error, a GPU loss, out of memory).
	auto IsCrashLine = [](const FString& L)
	{
		return L.Contains(TEXT("=== Critical error: ===")) || L.Contains(TEXT("Fatal error:")) || L.Contains(TEXT("Unhandled Exception"))
			|| L.Contains(TEXT("GPU crashed")) || L.Contains(TEXT("Device Removed")) || L.Contains(TEXT("Ran out of memory"));
	};
	int32 At = INDEX_NONE;
	for (int32 i = FMath::Max(0, Lines.Num() - 400); i < Lines.Num() && At == INDEX_NONE; ++i)
		if (IsCrashLine(Lines[i])) At = i;
	if (At == INDEX_NONE) return;

	FFileHelper::SaveStringToFile(Reported + Key + TEXT("\n"), *ReportedPath());

	FString Error;
	for (int32 i = At; i < FMath::Min(Lines.Num(), At + 45); ++i) Error += Lines[i] + TEXT("\n");
	FString Tail;
	for (int32 i = FMath::Max(0, At - 30); i < At; ++i) Tail += Lines[i] + TEXT("\n");
	// Title: the message of the first line that says what went wrong (not the banner).
	auto Message = [](const FString& L)
	{
		FString M = L;
		int32 C = M.Find(TEXT("Error:"));
		if (C != INDEX_NONE) M = M.Mid(C + 6);
		else if ((C = M.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromEnd)) != INDEX_NONE) M = M.Mid(C + 1);
		return M.TrimStartAndEnd();
	};
	FString First = Message(Lines[At]);
	if (First.Contains(TEXT("=== Critical error")) && At + 1 < Lines.Num()) First = Message(Lines[At + 1]);
	First = First.Left(90);
	const FString Body = FString::Printf(TEXT("**Crash report** (sent from the game)\n\n%s\n**Error**\n```\n%s```\n**Just before**\n```\n%s```\n"),
		*SystemSummary(), *Anonymise(Error), *Anonymise(Tail));
	PendingTitle = TEXT("Crash: ") + (First.IsEmpty() ? FString(TEXT("game crashed")) : Anonymise(First));
	PendingBody = Body;
	UE_LOG(LogDragon, Display, TEXT("Report: the previous run crashed (%s); F8 sends a report"), *Key);
	if (FParse::Param(FCommandLine::Get(), TEXT("ReportSelfTest"))) SendPendingCrash();
}

void DragonReport::OpenFeedback(const FString& Where)
{
	const FString Body = FString::Printf(TEXT("**What happened / what looks wrong:**\n\n(write here - you can also drag a screenshot into this box)\n\n---\n%s- %s\n"),
		*SystemSummary(), *Where);
	OpenIssue(TEXT("Feedback: "), Body, TEXT("feedback"));
}

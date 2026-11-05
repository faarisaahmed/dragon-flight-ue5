#include "DragonReport.h"
#include "DragonFlight.h"
#include "DragonWorldSubsystem.h"
#include "DragonGameMode.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"
#include "UnrealClient.h"

const TCHAR* DragonReport::Topic = TEXT("dragonflight-b549fcbc08495fc8");

namespace
{
	FString NoticeText;
	double NoticeUntil = 0.0;

	void SetNotice(const FString& S, double Seconds = 8.0)
	{
		NoticeText = S;
		NoticeUntil = FPlatformTime::Seconds() + Seconds;
	}

	FString Anonymise(FString S)
	{
		// Paths carry the Windows / macOS user name.
		const FString User = FPlatformProcess::UserName(false);
		if (User.Len() >= 2) S.ReplaceInline(*User, TEXT("<user>"), ESearchCase::IgnoreCase);
		return S;
	}

	/** HTTP headers must be plain ASCII on one line. */
	FString HeaderSafe(const FString& In)
	{
		FString Out;
		for (TCHAR C : In) Out.AppendChar((C >= 32 && C < 127) ? C : TEXT(' '));
		return Out.Left(200);
	}

	FString Url() { return FString(TEXT("https://ntfy.sh/")) + DragonReport::Topic; }

	bool SelfTest() { return FParse::Param(FCommandLine::Get(), TEXT("ReportSelfTest")); }

	/** Posts a text report (bodies over 4 KB arrive as an attached text file, which is fine). */
	void PostText(const FString& Title, const FString& Tags, const FString& Body, TFunction<void(bool)> Done = nullptr)
	{
		if (SelfTest()) FFileHelper::SaveStringToFile(Title + TEXT("\n\n") + Body, *(FPaths::ProjectSavedDir() / TEXT("ReportSelfTest.txt")));
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> R = FHttpModule::Get().CreateRequest();
		R->SetURL(Url());
		R->SetVerb(TEXT("POST"));
		R->SetHeader(TEXT("Title"), HeaderSafe(Title));
		R->SetHeader(TEXT("Tags"), Tags);
		R->SetHeader(TEXT("Content-Type"), TEXT("text/markdown; charset=utf-8"));
		R->SetContentAsString(Body);
		R->OnProcessRequestComplete().BindLambda([Done](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			const bool bGood = bOk && Resp.IsValid() && Resp->GetResponseCode() == 200;
			UE_LOG(LogDragon, Display, TEXT("Report: sent %s (%d)"), bGood ? TEXT("ok") : TEXT("FAILED"), Resp.IsValid() ? Resp->GetResponseCode() : 0);
			if (Done) Done(bGood);
		});
		R->ProcessRequest();
	}

	void PostFile(const FString& Title, const FString& Tags, const FString& Message, const FString& FileName, const TArray<uint8>& Data)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> R = FHttpModule::Get().CreateRequest();
		R->SetURL(Url());
		R->SetVerb(TEXT("PUT"));
		R->SetHeader(TEXT("Title"), HeaderSafe(Title));
		R->SetHeader(TEXT("Tags"), Tags);
		R->SetHeader(TEXT("Filename"), FileName);
		R->SetHeader(TEXT("Message"), HeaderSafe(Message));
		R->SetContent(Data);
		R->OnProcessRequestComplete().BindLambda([](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			const bool bGood = bOk && Resp.IsValid() && Resp->GetResponseCode() == 200;
			UE_LOG(LogDragon, Display, TEXT("Report: screenshot %s (%d)"), bGood ? TEXT("ok") : TEXT("FAILED"), Resp.IsValid() ? Resp->GetResponseCode() : 0);
			SetNotice(bGood ? TEXT("Screenshot sent to Faaris - tell him what you saw") : TEXT("The screenshot could not be sent (no internet?)"), 6.0);
		});
		R->ProcessRequest();
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

FString DragonReport::Notice()
{
	return FPlatformTime::Seconds() < NoticeUntil ? NoticeText : FString();
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
	// The first crash line in the last part of the log (UE's "Critical error" block, a fatal
	// error, a GPU loss, out of memory).
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

	FString Error;
	for (int32 i = At; i < FMath::Min(Lines.Num(), At + 80); ++i) Error += Lines[i] + TEXT("\n");
	FString Before;
	for (int32 i = FMath::Max(0, At - 150); i < At; ++i) Before += Lines[i] + TEXT("\n");
	const FString Body = FString::Printf(TEXT("**Crash** (reported by the game on its next start)\n\n%s\n**Error**\n```\n%s```\n**Before it**\n```\n%s```\n"),
		*SystemSummary(), *Anonymise(Error), *Anonymise(Before));
	UE_LOG(LogDragon, Display, TEXT("Report: the previous run crashed (%s), sending a report"), *Key);
	PostText(TEXT("Crash: ") + Anonymise(First).Left(90), TEXT("boom"), Body, [](bool bOk)
	{
		SetNotice(bOk ? TEXT("The game crashed last time - a crash report was sent to Faaris") : TEXT("The game crashed last time (the report could not be sent - no internet?)"), 12.0);
	});
}

void DragonReport::SendSnapshot(const FString& Where)
{
	const FString Summary = SystemSummary() + TEXT("- ") + Anonymise(Where) + TEXT("\n");
	PostText(TEXT("F8 snapshot"), TEXT("camera"), FString(TEXT("**F8 snapshot** (screenshot follows)\n\n")) + Summary);
	if (!GEngine || !GEngine->GameViewport) return;

	// The next frame, as pixels: compress to JPEG and send it as an attachment.
	static FDelegateHandle Handle;
	if (Handle.IsValid()) UGameViewportClient::OnScreenshotCaptured().Remove(Handle);
	Handle = UGameViewportClient::OnScreenshotCaptured().AddLambda([Where](int32 W, int32 H, const TArray<FColor>& Pixels)
	{
		UGameViewportClient::OnScreenshotCaptured().Remove(Handle);
		Handle.Reset();
		IImageWrapperModule& IW = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Jpg = IW.CreateImageWrapper(EImageFormat::JPEG);
		if (!Jpg.IsValid() || !Jpg->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), W, H, ERGBFormat::BGRA, 8)) return;
		const TArray64<uint8> Data64 = Jpg->GetCompressed(88);
		TArray<uint8> Data(Data64.GetData(), (int32)Data64.Num());
		if (SelfTest()) FFileHelper::SaveArrayToFile(Data, *(FPaths::ProjectSavedDir() / TEXT("ReportSelfTest.jpg")));
		SetNotice(TEXT("Sending a screenshot to Faaris..."), 4.0);
		// (A "Name: value" message would be read as a header line; keep it plain.)
		PostFile(TEXT("F8 screenshot"), TEXT("camera"), Anonymise(Where.Replace(TEXT("Where: "), TEXT("Screenshot taken "))), FDateTime::Now().ToString(TEXT("shot_%Y%m%d_%H%M%S.jpg")), Data);
	});
	FScreenshotRequest::RequestScreenshot(false);
}

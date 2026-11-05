#pragma once

#include "CoreMinimal.h"

/**
 * Problem reports without copying and pasting: a report becomes a pre-filled GitHub issue on
 * the project repo, opened in the browser, so the tester only has to press "Submit".
 *
 *  - CheckPreviousCrash(): at startup, if the previous run's log ended in a crash, a crash report
 *    is prepared (once per crash) and the HUD offers it: "press F8 to send".
 *  - F8 in game: sends that crash report, or else opens a feedback report with the system,
 *    settings and where you are.
 */
namespace DragonReport
{
	/** GPU, driver, CPU, RAM, OS, engine, build, graphics settings. */
	FString SystemSummary();
	/** Opens github.com/<repo>/issues/new with this title and body (trimmed to fit a URL). */
	void OpenIssue(const FString& Title, const FString& Body, const FString& Label);
	/** Call once at startup. */
	void CheckPreviousCrash();
	/** A crash report from the previous run is waiting to be sent. */
	bool HasPendingCrash();
	/** Opens the waiting crash report (and forgets it). */
	void SendPendingCrash();
	/** In-game report; Where = a line about the player's position. */
	void OpenFeedback(const FString& Where);
	/** The build the game was made from (the installer writes it), or "dev". */
	FString BuildId();
}

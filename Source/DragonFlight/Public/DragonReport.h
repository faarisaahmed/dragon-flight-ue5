#pragma once

#include "CoreMinimal.h"

/**
 * Problem reports that need nothing from the tester: they are posted to a private ntfy.sh topic
 * (a free message drop, no account) and Tools/fetch_reports.py saves them into Reports/ on
 * Faaris's machines.
 *
 *  - CheckPreviousCrash(): at startup, if the previous run's log ended in a crash, the crash
 *    report is sent (once per crash) and the HUD says so.
 *  - SendSnapshot() (F8): a screenshot plus the system, settings and where you are.
 */
namespace DragonReport
{
	/** The ntfy.sh topic reports go to (also in Install.ps1 and Tools/fetch_reports.py). */
	extern const TCHAR* Topic;

	/** GPU, driver, CPU, RAM, OS, engine, build, graphics settings. */
	FString SystemSummary();
	/** Call once at startup. */
	void CheckPreviousCrash();
	/** F8: screenshot + summary; Where = a line about the player's position. */
	void SendSnapshot(const FString& Where);
	/** A short message for the HUD after something was sent ("" when none). */
	FString Notice();
	/** The build the game was made from (the installer writes it), or "dev". */
	FString BuildId();
}

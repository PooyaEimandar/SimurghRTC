// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractDataExample.h"
#include "SimurghRTCSessionTransport.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

void AContractDataExample::RequestSession_Implementation()
{ if (Report) Report->Fail(TEXT("The sample must implement its session creation flow.")); }

bool AContractDataExample::PrepareBackground()
{
    if (!Report || !Session || !Session->IsUsable() || Report->bComplete || Report->bInBackground) return false;
    Report->bGameThread &= IsInGameThread();
    Report->bInBackground = true; ++Report->InactiveEvents;
    Report->BackgroundStarted = FPlatformTime::Seconds();
    Report->BackgroundCompletions = Report->CompletedIds.Num();
    Report->bBackgroundPending = PendingOperation && !PendingOperation->IsFinished();
    RetiredSession = Session; RetiredOperation = PendingOperation;
    if (Report->IsApplicationLifecycle())
    {
        const bool PendingCase = Report->Scenario == TEXT("background-connect");
        if (Report->Round != 0 || Report->InactiveEvents != 1 || Report->bBackgroundPending != PendingCase
            || Report->CompletedRounds != (PendingCase ? 0 : 1)
            || Session->GetState() != (PendingCase ? ESimurghRTCSessionState::Connecting : ESimurghRTCSessionState::Connected))
            Report->Fail(TEXT("Background event arrived outside the intended connection phase."));
    }
    return true; // The C++ flow or Blueprint graph calls the public Shutdown next.
}

void AContractDataExample::RecordBackgroundShutdown()
{
    if (!Report) return;
    Report->bBackgroundShutdown = Session && !Session->IsUsable() && Session->GetState() == ESimurghRTCSessionState::Closed;
    Report->ShutdownSeconds = FPlatformTime::Seconds()-Report->BackgroundStarted;
    if (!Report->bBackgroundShutdown || Report->CompletedIds.Num() != Report->BackgroundCompletions)
        Report->Fail(TEXT("Background shutdown must be immediate and silent."));
    if (Report->bBackgroundPending)
    {
        Report->bBackgroundCancelled = PendingOperation && PendingOperation->IsFinished()
            && PendingOperation->GetResult().Error.Code == ESimurghRTCErrorCode::Cancelled;
        if (!Report->bBackgroundCancelled) Report->Fail(TEXT("Background shutdown did not silently settle pending Connect."));
    }
    if (!Report->ChannelIds.IsEmpty()) CheckStaleChannel();
    // Save from the lifecycle callback itself: the OS may suspend the next game tick.
    WriteLifecycleProgress();
}

bool AContractDataExample::PrepareForeground()
{
    if (!Report || !Report->bInBackground) return false; // Ignore the initial foreground and duplicate notifications.
    Report->bGameThread &= IsInGameThread();
    Report->ForegroundAt = FPlatformTime::Seconds();
    Report->BackgroundSeconds = Report->OSBackgroundAt > 0 ? Report->ForegroundAt-Report->OSBackgroundAt : 0;
    Report->bInBackground = false; ++Report->ActiveEvents;
    if (!Report->bBackgroundShutdown || !Session || Session->IsUsable())
    { Report->Fail(TEXT("Foreground resume requires a retired background session.")); return false; }
    ++Report->Round;
    Report->bTextReceived = Report->bBytesReceived = Report->bRoundRecorded = Report->bAwaitingFailure = false;
    // Shutdown is final. The public CreateSession node/call must supply a new facade.
    Session = nullptr; PendingOperation = nullptr; Channel = {}; Report->SessionId = {};
    SetRoundPayload();
    return LoadConnectionConfig();
}

void AContractDataExample::RecordOSBackground()
{
    if (!Report || !Report->bInBackground) return;
    Report->bGameThread &= IsInGameThread(); ++Report->BackgroundEvents;
    Report->OSBackgroundAt = FPlatformTime::Seconds(); WriteLifecycleProgress();
}
void AContractDataExample::RecordOSForeground()
{
    if (!Report || !Report->bInBackground) return;
    Report->bGameThread &= IsInGameThread(); ++Report->ForegroundEvents;
}

void AContractDataExample::CheckRetiredObjectsCollected()
{
    if (!Report || !Report->IsApplicationLifecycle()) return;
    Report->bRetiredObjectsCollected = RetiredSession.GetEvenIfUnreachable() == nullptr
        && RetiredOperation.GetEvenIfUnreachable() == nullptr;
    if (!Report->bRetiredObjectsCollected) Report->Fail(TEXT("Background session or operation remained retained after collection."));
}

void AContractDataExample::WriteLifecycleProgress() const
{
    if (!Report || !Report->IsApplicationLifecycle()) return;
    FString RunId; FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCExampleRunId="),RunId);
    auto J = MakeShared<FJsonObject>(); const auto& R = *Report;
    J->SetStringField(TEXT("run_id"),RunId); J->SetStringField(TEXT("session_id"),R.SessionId.ToString(EGuidFormats::DigitsWithHyphensLower));
    J->SetStringField(TEXT("error"),R.Error); J->SetNumberField(TEXT("state"),Session ? int32(Session->GetState()) : 0);
    J->SetNumberField(TEXT("round"),R.Round); J->SetNumberField(TEXT("completed_rounds"),R.CompletedRounds);
    J->SetNumberField(TEXT("inactive_events"),R.InactiveEvents); J->SetNumberField(TEXT("active_events"),R.ActiveEvents);
    J->SetNumberField(TEXT("background_events"),R.BackgroundEvents); J->SetNumberField(TEXT("foreground_events"),R.ForegroundEvents);
    J->SetBoolField(TEXT("in_background"),R.bInBackground); J->SetBoolField(TEXT("background_shutdown"),R.bBackgroundShutdown);
    J->SetBoolField(TEXT("background_pending"),R.bBackgroundPending); J->SetBoolField(TEXT("background_cancelled"),R.bBackgroundCancelled);
    J->SetNumberField(TEXT("completions"),R.CompletedIds.Num()); J->SetNumberField(TEXT("late_callbacks"),R.LateCallbacks);
    J->SetNumberField(TEXT("transports_created"),SimurghRTC::GetSessionRuntimeSnapshot().Created);
    J->SetNumberField(TEXT("shutdown_seconds"),R.ShutdownSeconds);
    FString Text; FJsonSerializer::Serialize(J,TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    const auto Directory = FPaths::ProjectSavedDir()/TEXT("SimurghRTC"); IFileManager::Get().MakeDirectory(*Directory,true);
    if (!FFileHelper::SaveStringToFile(Text,*(Directory/(TEXT("data-example-progress-")+RunId+TEXT(".json")))))
        Report->Fail(TEXT("Could not write lifecycle progress."));
}

// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractDataExampleProbe.h"
#include "ContractDataExample.h"
#include "SimurghRTCSessionTransport.h"
#include "SimurghRTCNativeRuntime.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

class FContractDataExampleProbe::FImpl
{
public:
    FString RunId, Implementation, Scenario, Map, Error, TriggerGate;
    TWeakObjectPtr<AContractDataExample> Actor;
    TWeakObjectPtr<USimurghRTCSession> Session;
    TWeakObjectPtr<USimurghRTCOperation> Operation;
    TSharedPtr<FContractDataExampleResult> Result;
    FTSTicker::FDelegateHandle Ticker;
    double Started = FPlatformTime::Seconds();
    int32 Phase = 0, Quiet = 0;
    bool bRequestedDisconnect = false, bPendingAtDestruction = false, bSilentCancelled = false, bCollected = false;
    ~FImpl()
    {
        FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
        if (Session.IsValid()) Session->Shutdown();
        if (Actor.IsValid() && Actor->Session) Actor->Session->Shutdown();
    }
    void Fail(const TCHAR* Why) { if (Error.IsEmpty()) Error = Why; }
    void DestroyOwner(bool Pending)
    {
        auto* Owner = Actor.Get();
        if (!Owner || !Owner->Session || !Owner->PendingOperation) { Fail(TEXT("No sample owner or pending operation.")); return; }
        Session = Owner->Session; Operation = Owner->PendingOperation;
        bPendingAtDestruction = !Operation->IsFinished();
        if (Pending != bPendingAtDestruction) Fail(TEXT("Owner destruction did not reach the intended operation phase."));
        const int32 Completions = Result->CompletedIds.Num();
        Owner->Destroy(); // Must execute the actual C++ override or Blueprint EndPlay graph.
        bSilentCancelled = Operation->IsFinished() && Operation->GetResult().Error.Code == ESimurghRTCErrorCode::Cancelled
            && Result->CompletedIds.Num() == Completions;
        if (Pending && !bSilentCancelled) Fail(TEXT("Pending operation did not settle silently at EndPlay."));
        if (!Result->bEndPlayClosed || Result->EndPlayCalls != 1) Fail(TEXT("EndPlay cleanup failed."));
        Phase = 2;
    }
    bool Tick(float)
    {
        if (FPlatformTime::Seconds() - Started > 60) Fail(TEXT("Packaged data example timed out."));
        if (Phase == 0)
        {
            for (const auto& Context : GEngine->GetWorldContexts())
            {
                UWorld* World = Context.World();
                if (!World || !World->IsGameWorld() || !World->HasBegunPlay()) continue;
                for (TActorIterator<AContractDataExample> It(World); It; ++It)
                {
                    if (Actor.IsValid()) { Fail(TEXT("Expected exactly one data example actor.")); break; }
                    Actor = *It; Map = World->GetOutermost()->GetName(); Result = It->GetExampleResult();
                }
            }
            if (Actor.IsValid() && Result)
            {
                if (Result->Implementation != Implementation || Result->Scenario != Scenario
                    || Map != (Implementation == TEXT("cpp") ? TEXT("/Game/Examples/Direct/Maps/RtcDataCpp") : TEXT("/Game/Examples/Direct/Maps/RtcDataBlueprint")))
                    Fail(TEXT("Wrong packaged map, implementation or scenario."));
                Phase = 1;
            }
        }
        if (Result && !Result->Error.IsEmpty()) Fail(*Result->Error);
        if (!Error.IsEmpty()) return Finish();
        if (Phase == 1 && Actor.IsValid())
        {
            auto* Owner = Actor.Get();
            const bool Pending = Owner->PendingOperation && !Owner->PendingOperation->IsFinished();
            const bool Connecting = Owner->Session && Owner->Session->GetState() == ESimurghRTCSessionState::Connecting;
            const bool GateOpen = !TriggerGate.IsEmpty() && IFileManager::Get().FileExists(*TriggerGate);
            if (Scenario == TEXT("disconnect-pending") && Connecting && Pending && GateOpen && !bRequestedDisconnect)
            { bRequestedDisconnect = true; Owner->RequestDisconnect(); }
            else if (Scenario == TEXT("destroy-connect") && Connecting && Pending && GateOpen) DestroyOwner(true);
            else if (Scenario == TEXT("destroy-open") && Result->StartedIds.Num() == 2 && Result->CompletedIds.Num() == 1 && Pending) DestroyOwner(true);
            else if (Result->bComplete) DestroyOwner(false);
        }
        else if (Phase == 2)
        {
            // Collect on a later tick, after Destroy/EndPlay and any dispatch stack have returned.
            CollectGarbage(RF_NoFlags);
            bCollected = Actor.GetEvenIfUnreachable() == nullptr && Session.GetEvenIfUnreachable() == nullptr
                && Operation.GetEvenIfUnreachable() == nullptr;
            if (!bCollected) Fail(TEXT("Example actor, session or operation remained retained after collection."));
            Phase = 3;
        }
        else if (Phase == 3)
        {
            const auto Runtime = SimurghRTC::GetSessionRuntimeSnapshot();
            if (Runtime.Created == 1 && Runtime.NativeClosed == 1 && Runtime.PendingCleanup == 0 && ++Quiet >= 8) return Finish();
        }
        if (!Error.IsEmpty()) return Finish();
        return true;
    }
    bool Finish()
    {
        const auto Runtime = SimurghRTC::GetSessionRuntimeSnapshot();
        if (Result && !Result->Error.IsEmpty()) Fail(*Result->Error);
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("schema"),1); J->SetStringField(TEXT("run_id"),RunId);
        J->SetStringField(TEXT("implementation"),Implementation); J->SetStringField(TEXT("scenario"),Scenario);
        J->SetStringField(TEXT("map"),Map); J->SetNumberField(TEXT("pid"),FPlatformProcess::GetCurrentProcessId());
        J->SetStringField(TEXT("engine_version"),FEngineVersion::Current().ToString());
        J->SetBoolField(TEXT("editor_build"),WITH_EDITOR != 0); J->SetBoolField(TEXT("server_build"),UE_SERVER != 0);
        J->SetBoolField(TEXT("arm64_build"),PLATFORM_CPU_ARM_FAMILY && sizeof(void*) == 8);
        J->SetBoolField(TEXT("with_janus"),SIMURGHRTC_WITH_JANUS != 0);
        J->SetStringField(TEXT("artifact_manifest_sha256"),SimurghRTC::FNativeRuntime::GetMobileArtifactSHA256());
        J->SetStringField(TEXT("native_backend"),SimurghRTC::FNativeRuntime::GetBackendVersion());
        J->SetBoolField(TEXT("shared_api"),true); J->SetBoolField(TEXT("loopback_only"),true);
        J->SetBoolField(TEXT("network_media_tested"),false);
        J->SetBoolField(TEXT("passed"),Error.IsEmpty() && Phase == 3 && Quiet >= 8); J->SetStringField(TEXT("error"),Error);
        J->SetNumberField(TEXT("transports_created"),Runtime.Created); J->SetNumberField(TEXT("native_closed"),Runtime.NativeClosed);
        J->SetNumberField(TEXT("remote_closed"),Runtime.RemoteClosed); J->SetNumberField(TEXT("pending_cleanup"),Runtime.PendingCleanup);
        J->SetNumberField(TEXT("quiet_ticks"),Quiet); J->SetBoolField(TEXT("pending_at_destruction"),bPendingAtDestruction);
        J->SetBoolField(TEXT("silent_cancelled"),bSilentCancelled); J->SetBoolField(TEXT("objects_collected"),bCollected);
        J->SetBoolField(TEXT("disconnect_requested"),bRequestedDisconnect);
        J->SetNumberField(TEXT("elapsed_seconds"),FPlatformTime::Seconds()-Started);
        if (Result)
        {
            const auto& R = *Result;
            J->SetStringField(TEXT("actor_class"),R.ClassPath); J->SetStringField(TEXT("session_id"),R.SessionId.ToString(EGuidFormats::DigitsWithHyphensLower));
            J->SetNumberField(TEXT("operations"),R.StartedIds.Num()); J->SetNumberField(TEXT("completions"),R.CompletedIds.Num());
            J->SetNumberField(TEXT("sent"),R.Sent); J->SetNumberField(TEXT("received"),R.Received);
            J->SetNumberField(TEXT("channel_events"),R.ChannelEvents); J->SetNumberField(TEXT("open_events"),R.OpenEvents);
            J->SetNumberField(TEXT("end_play_calls"),R.EndPlayCalls); J->SetNumberField(TEXT("late_callbacks"),R.LateCallbacks);
            J->SetNumberField(TEXT("last_failure"),R.LastFailure); J->SetBoolField(TEXT("callbacks_on_game_thread"),R.bGameThread);
            J->SetBoolField(TEXT("channel_info_checked"),R.bInfoChecked); J->SetBoolField(TEXT("end_play_closed"),R.bEndPlayClosed);
            J->SetBoolField(TEXT("exact_payloads"),R.bTextReceived && R.bBytesReceived);
            TArray<TSharedPtr<FJsonValue>> Outcomes, States;
            for (const auto& S : R.Outcomes) Outcomes.Add(MakeShared<FJsonValueString>(S));
            for (const auto S : R.States) States.Add(MakeShared<FJsonValueNumber>(int32(S)));
            J->SetArrayField(TEXT("outcomes"),Outcomes); J->SetArrayField(TEXT("states"),States);
        }
        FString Text; FJsonSerializer::Serialize(J,TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
        const auto Directory = FPaths::ProjectSavedDir()/TEXT("SimurghRTC"); IFileManager::Get().MakeDirectory(*Directory,true);
        const bool Saved = FFileHelper::SaveStringToFile(Text,*(Directory/(TEXT("data-example-")+RunId+TEXT(".json"))));
        UE_LOG(LogTemp,Display,TEXT("SIMURGHRTC_DATA_EXAMPLE_REPORT %s"),*Text);
        FPlatformMisc::RequestExitWithStatus(false, Saved && J->GetBoolField(TEXT("passed")) ? 0 : 6);
        return false;
    }
};
FContractDataExampleProbe::FContractDataExampleProbe() = default;
FContractDataExampleProbe::~FContractDataExampleProbe() = default;
void FContractDataExampleProbe::Startup()
{
    if (!FParse::Param(FCommandLine::Get(),TEXT("SimurghRTCDataExampleProbe"))) return;
    Exit = FCoreDelegates::OnEnginePreExit.AddRaw(this,&FContractDataExampleProbe::Shutdown);
    Init = FCoreDelegates::GetOnPostEngineInit().AddLambda([this]
    {
        Impl = MakeUnique<FImpl>();
        FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCExampleRunId="),Impl->RunId);
        FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCExampleImplementation="),Impl->Implementation);
        FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCExampleScenario="),Impl->Scenario);
        FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCExampleGate="),Impl->TriggerGate);
        FGuid Id;
        if (!FGuid::ParseExact(Impl->RunId,EGuidFormats::DigitsWithHyphens,Id)) Impl->Fail(TEXT("Invalid example run ID."));
        Impl->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(Impl.Get(),&FImpl::Tick));
    });
}
void FContractDataExampleProbe::Shutdown()
{
    FCoreDelegates::GetOnPostEngineInit().Remove(Init); FCoreDelegates::OnEnginePreExit.Remove(Exit);
    Init.Reset(); Exit.Reset(); Impl.Reset();
}

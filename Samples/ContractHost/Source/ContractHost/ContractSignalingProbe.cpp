// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractSignalingProbe.h"
#include "SimurghRTCNativePeer.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#if PLATFORM_IOS
#include <unistd.h>
#endif
DEFINE_LOG_CATEGORY_STATIC(LogSimurghRTCSignalingProbe, Log, All);

using namespace SimurghRTC;
class FContractSignalingProbe::FImpl
{
public:
    FDelegateHandle Init;
    FTSTicker::FDelegateHandle Ticker;
    TStrongObjectPtr<UObject> Owner;
    FNativePeer::FHandle Peers[2];
    FString RunId, Error, Offer, Answer;
    TArray<TSharedPtr<FJsonValue>> Pairs;
    TSharedPtr<FJsonObject> Pair;
    int32 Requested = 8, Index = 0, Phase = 0, Ready = 0;
    int32 Early[2]{}, Local[2]{}, Duplicates = 0, Accepted = 0, Completed = 0;
    bool End[2]{}, EarlyEnd[2]{}, OfferSet = false, AnswerSet = false, bGameThread = true;
    double Started = 0, PairStarted = 0, CloseStarted = 0;

    ~FImpl()
    {
        FCoreDelegates::GetOnPostEngineInit().Remove(Init);
        FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
        for (auto& Peer : Peers) { if (Peer) Peer->Close(); Peer.Reset(); }
        Owner.Reset();
    }
    void Fail(const FString& Message) { if (Error.IsEmpty()) Error = Message; }
    void Send(int32 To, FNativeSignalRequest Request, FNativeSignalCompleted Callback = {})
    {
        if (!Error.IsEmpty()) return;
        FString Failure;
        if (Peers[To]->Signal(MoveTemp(Request), [this, Callback = MoveTemp(Callback)](const FNativeSignalResult& Result) mutable
        {
            bGameThread &= IsInGameThread(); ++Completed;
            if (!Result.IsSuccess()) { Fail(Result.Message); return; }
            if (Callback) Callback(Result);
        }, Failure)) ++Accepted;
        else Fail(Failure);
    }
    void Forward(int32 From, const FNativeTransportEvent& Event)
    {
        bGameThread &= IsInGameThread();
        if (Event.Type == ENativeSignalEvent::FatalError) { Fail(Event.Result.Message); return; }
        if (Phase >= 7 || !Error.IsEmpty()) return;
        if (Event.Type == ENativeSignalEvent::LocalIce)
        {
            ++Local[From];
            FNativeSignalRequest Q; Q.Action = ENativeSignalAction::AddIce; Q.Ice = Event.Ice;
            Send(1-From, Q, [this, From](const FNativeSignalResult& R) { if (R.bQueued) ++Early[From]; });
            Send(1-From, MoveTemp(Q), [this](const FNativeSignalResult&) { ++Duplicates; });
        }
        else if (Event.Type == ENativeSignalEvent::LocalIceComplete)
        {
            FNativeSignalRequest Q; Q.Action = ENativeSignalAction::EndIce;
            Send(1-From, MoveTemp(Q), [this, From](const FNativeSignalResult& R) { End[From] = true; EarlyEnd[From] = R.bQueued; });
        }
    }
    TSharedRef<FJsonObject> Snapshot(const FNativeTransportSnapshot& S)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("connection"), int32(S.Connection));
        J->SetNumberField(TEXT("ice"), S.IceState); J->SetNumberField(TEXT("dtls"), S.DtlsState);
        J->SetNumberField(TEXT("protocol"), S.SelectedProtocol);
        J->SetBoolField(TEXT("sctp_open"), S.bDataTransportOpen);
        J->SetBoolField(TEXT("local_sdp"), S.bLocalDescriptionSet); J->SetBoolField(TEXT("remote_sdp"), S.bRemoteDescriptionSet);
        J->SetBoolField(TEXT("local_ice_complete"), S.bLocalIceComplete); J->SetBoolField(TEXT("remote_ice_complete"), S.bRemoteIceComplete);
        J->SetNumberField(TEXT("queued"), S.RemoteIceQueued); J->SetNumberField(TEXT("applied"), S.RemoteIceApplied);
        J->SetNumberField(TEXT("local_udp"), S.LocalUdpCandidates); J->SetNumberField(TEXT("local_tcp"), S.LocalTcpCandidates);
        J->SetNumberField(TEXT("candidate_errors"), S.CandidateErrors); J->SetNumberField(TEXT("last_candidate_error"), S.LastCandidateError);
        J->SetNumberField(TEXT("stats_samples"), S.StatsSamples); J->SetNumberField(TEXT("stats_age_ms"), S.StatsAgeMs);
        J->SetNumberField(TEXT("pairs"), S.CandidatePairs); J->SetNumberField(TEXT("succeeded_pairs"), S.SucceededPairs); J->SetNumberField(TEXT("failed_pairs"), S.FailedPairs);
        J->SetNumberField(TEXT("stun_requests"), static_cast<double>(S.StunRequestsSent));
        J->SetNumberField(TEXT("stun_responses"), static_cast<double>(S.StunResponsesReceived));
        J->SetNumberField(TEXT("send_discards"), static_cast<double>(S.PacketsDiscardedOnSend));
        return J;
    }
    void BeginClose(bool bConnected)
    {
        Pair = MakeShared<FJsonObject>(); Pair->SetNumberField(TEXT("index"), Index);
        Pair->SetStringField(TEXT("mode"), Index % 2 ? TEXT("media") : TEXT("data"));
        Pair->SetStringField(TEXT("delivery"), Index % 3 == 0 ? TEXT("early_both") : Index % 3 == 1 ? TEXT("early_offer") : TEXT("trickle"));
        Pair->SetBoolField(TEXT("connected"), bConnected);
        Pair->SetNumberField(TEXT("operations_accepted"), Accepted); Pair->SetNumberField(TEXT("operations_completed"), Completed);
        Pair->SetNumberField(TEXT("duplicates"), Duplicates);
        Pair->SetNumberField(TEXT("early_offer_ice"), Early[0]); Pair->SetNumberField(TEXT("early_answer_ice"), Early[1]);
        Pair->SetBoolField(TEXT("early_offer_end"), EarlyEnd[0]); Pair->SetBoolField(TEXT("early_answer_end"), EarlyEnd[1]);
        Pair->SetNumberField(TEXT("seconds"), FPlatformTime::Seconds()-PairStarted);
        TArray<TSharedPtr<FJsonValue>> States;
        for (auto& Peer : Peers)
        {
            if (Peer) { States.Add(MakeShared<FJsonValueObject>(Snapshot(Peer->GetSnapshot().Transport))); Peer->Close(); }
        }
        Pair->SetArrayField(TEXT("peers"), States);
        CloseStarted = FPlatformTime::Seconds(); Phase = 7;
    }
    void Report(bool bClosed)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("schema"), 1); J->SetStringField(TEXT("run_id"), RunId);
        J->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
        J->SetStringField(TEXT("platform"), FPlatformProperties::IniPlatformName());
#if PLATFORM_IOS
        J->SetNumberField(TEXT("pid"), int32(getpid()));
#else
        J->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
#endif
        J->SetBoolField(TEXT("editor_build"), WITH_EDITOR != 0); J->SetBoolField(TEXT("server_build"), UE_SERVER != 0);
        J->SetBoolField(TEXT("arm64_build"), PLATFORM_CPU_ARM_FAMILY && sizeof(void*) == 8);
#if PLATFORM_IOS && WITH_IOS_SIMULATOR
        J->SetBoolField(TEXT("simulator_build"), true);
#else
        J->SetBoolField(TEXT("simulator_build"), false);
#endif
        J->SetBoolField(TEXT("with_janus"), SIMURGHRTC_WITH_JANUS != 0);
        J->SetStringField(TEXT("artifact_manifest_sha256"), FNativeRuntime::GetMobileArtifactSHA256());
        J->SetStringField(TEXT("native_backend"), FNativeRuntime::GetBackendVersion());
        J->SetNumberField(TEXT("requested_pairs"), Requested); J->SetNumberField(TEXT("completed_pairs"), Index);
        J->SetArrayField(TEXT("pairs"), Pairs);
        J->SetBoolField(TEXT("callbacks_on_game_thread"), bGameThread); J->SetBoolField(TEXT("teardown_complete"), bClosed);
        J->SetBoolField(TEXT("network_transport_tested"), true); J->SetBoolField(TEXT("network_media_tested"), false);
        J->SetBoolField(TEXT("loopback_only"), true);
        J->SetBoolField(TEXT("cross_process_tested"), false); J->SetBoolField(TEXT("stun_turn_service_used"), false);
        J->SetBoolField(TEXT("passed"), Error.IsEmpty() && Index == Requested && bClosed && bGameThread);
        J->SetStringField(TEXT("error"), Error); J->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds()-Started);
        FString Json; FJsonSerializer::Serialize(J, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
        const auto Directory = FPaths::ProjectSavedDir() / TEXT("SimurghRTC"); IFileManager::Get().MakeDirectory(*Directory, true);
        const bool bSaved = FFileHelper::SaveStringToFile(Json, *(Directory / (TEXT("signaling-probe-") + RunId + TEXT(".json"))));
#if PLATFORM_ANDROID
        // Logcat limits a line's payload. Chunks are bounded, numbered, and tied to the fresh run UUID.
        const int32 Total = FMath::DivideAndRoundUp(Json.Len(), 640);
        for (int32 I = 0; I < Total; ++I)
            UE_LOG(LogSimurghRTCSignalingProbe, Display, TEXT("SIMURGHRTC_SIGNALING_CHUNK %s %d/%d %s"), *RunId, I+1, Total, *Json.Mid(I*640, 640));
#else
        UE_LOG(LogSimurghRTCSignalingProbe, Display, TEXT("SIMURGHRTC_SIGNALING_REPORT %s"), *Json);
#endif
        if (!bSaved) UE_LOG(LogSimurghRTCSignalingProbe, Error, TEXT("Could not save the signaling report."));
        if (FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCSignalingExit")))
            FPlatformMisc::RequestExitWithStatus(false, J->GetBoolField(TEXT("passed")) && bSaved ? 0 : 5);
    }
    bool Tick(float)
    {
        check(IsInGameThread());
        if (!Error.IsEmpty() && Phase < 7) BeginClose(false);
        if (Phase < 7 && FPlatformTime::Seconds()-PairStarted > 20) { Fail(TEXT("Local connection deadline expired.")); BeginClose(false); }
        FString Failure;
        if (Phase == 0)
        {
            Ready = Accepted = Completed = Duplicates = 0; OfferSet = AnswerSet = false; Offer.Reset(); Answer.Reset();
            for (int32 I=0; I<2; ++I) { Early[I] = Local[I] = 0; End[I] = EarlyEnd[I] = false; }
            PairStarted = FPlatformTime::Seconds();
            for (int32 I=0; I<2; ++I)
            {
                Peers[I] = FNativePeer::Open(Owner.Get(), Index % 2 ? ENativeFactoryMode::BuiltinMedia : ENativeFactoryMode::DataOnly,
                    [this](const FNativePeerSnapshot& S) { bGameThread &= IsInGameThread(); if (S.State == ENativePeerState::Ready) ++Ready; else Fail(S.Error); }, Failure, true);
                if (!Peers[I]) { Fail(Failure); return true; }
                Peers[I]->SetEventHandler([this, I](const FNativeTransportEvent& E) { Forward(I, E); });
            }
            Phase = 1;
        }
        else if (Phase == 1 && Ready == 2)
        { Send(0, {}, [this](const FNativeSignalResult& R) { Offer = R.Sdp; }); Phase = 2; }
        else if (Phase == 2 && !Offer.IsEmpty() && (Index % 3 == 2 || (End[0] && Early[0] > 0)))
        {
            FNativeSignalRequest Q; Q.Action = ENativeSignalAction::SetRemote; Q.SdpType = ENativeSdpType::Offer; Q.Sdp = Offer;
            Send(1, MoveTemp(Q), [this](const FNativeSignalResult&) { OfferSet = true; }); Phase = 3;
        }
        else if (Phase == 3 && OfferSet)
        {
            FNativeSignalRequest Q; Q.Action = ENativeSignalAction::CreateAnswer;
            Send(1, MoveTemp(Q), [this](const FNativeSignalResult& R) { Answer = R.Sdp; }); Phase = 4;
        }
        else if (Phase == 4 && !Answer.IsEmpty() && (Index % 3 != 0 || (End[1] && Early[1] > 0)))
        {
            FNativeSignalRequest Q; Q.Action = ENativeSignalAction::SetRemote; Q.SdpType = ENativeSdpType::Answer; Q.Sdp = Answer;
            Send(0, MoveTemp(Q), [this](const FNativeSignalResult&) { AnswerSet = true; }); Phase = 5;
        }
        else if (Phase == 5 && AnswerSet && End[0] && End[1] && Accepted == Completed)
        {
            bool Good = true;
            for (int32 I=0; I<2; ++I)
            {
                const auto S = Peers[I]->GetSnapshot().Transport;
                Good &= S.Connection == ENativeConnectionState::Connected && S.bDataTransportOpen
                    && S.bRemoteIceComplete && !S.RemoteIceQueued && S.RemoteIceApplied == uint32(Local[1-I])
                    && S.StatsSamples > 0 && S.DtlsState == 3 && S.SelectedProtocol > 0 && S.StatsAgeMs < 1000;
            }
            if (Good) BeginClose(true);
        }
        else if (Phase == 7)
        {
            bool Closed = true;
            for (const auto& Peer : Peers) if (Peer && Peer->GetSnapshot().State != ENativePeerState::Closed) Closed = false;
            if (!Closed && FPlatformTime::Seconds()-CloseStarted < 10) return true;
            if (!Closed) Fail(TEXT("Native teardown did not complete before the deadline."));
            Pair->SetBoolField(TEXT("closed"), Closed); Pairs.Add(MakeShared<FJsonValueObject>(Pair)); Pair.Reset();
            for (auto& Peer : Peers) Peer.Reset();
            if (Error.IsEmpty()) ++Index;
            if (!Error.IsEmpty() || Index == Requested) { Report(Closed); return false; }
            Phase = 0; PairStarted = FPlatformTime::Seconds();
        }
        return true;
    }
};
FContractSignalingProbe::FContractSignalingProbe() = default;
FContractSignalingProbe::~FContractSignalingProbe() = default;
void FContractSignalingProbe::Startup()
{
#if !UE_BUILD_SHIPPING
    if (!FParse::Param(FCommandLine::Get(), TEXT("SimurghRTCSignalingProbe"))) return;
    Impl = MakeUnique<FImpl>();
    FGuid Id; FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCSignalingRunId="), Impl->RunId);
    FParse::Value(FCommandLine::Get(), TEXT("SimurghRTCSignalingPairs="), Impl->Requested);
    if (!FGuid::ParseExact(Impl->RunId, EGuidFormats::DigitsWithHyphens, Id) || Impl->Requested < 2 || Impl->Requested > 32)
    { UE_LOG(LogSimurghRTCSignalingProbe, Error, TEXT("Signaling probe requires a UUID and 2–32 pairs.")); Impl.Reset(); return; }
    Impl->Init = FCoreDelegates::GetOnPostEngineInit().AddLambda([this]
    {
        Impl->Owner.Reset(NewObject<UGameInstance>()); Impl->Started = Impl->PairStarted = FPlatformTime::Seconds();
        Impl->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(Impl.Get(), &FImpl::Tick));
    });
#endif
}
void FContractSignalingProbe::Shutdown() { Impl.Reset(); }

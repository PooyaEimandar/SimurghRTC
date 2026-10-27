// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractSessionProbe.h"
#include "ContractSessionEndpoint.h"
#include "SimurghRTCSessionTransport.h"
#include "SimurghRTCNativeRuntime.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "UObject/StrongObjectPtr.h"

class FContractSessionProbe::FImpl
{
public:
    FString RunId, Scenario, Gate, Token, Endpoint, Error;
    int32 Role = 0, Phase = 0, QuietTicks = 0, IsolationChecks = 0;
    bool bPressure = false, bBlocked = false, bResumed = false, bReadySaved = false;
    double Started = FPlatformTime::Seconds();
    TArray<TStrongObjectPtr<UObject>> Owners;
    TArray<TStrongObjectPtr<UContractSessionEndpoint>> Clients;
    TStrongObjectPtr<UWorld> World;
    TWeakObjectPtr<USimurghRTCSession> CollectedSession;
    FTSTicker::FDelegateHandle TickHandle;
    ~FImpl()
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        for (auto& C : Clients) if (IsValid(C->Session)) C->Session->Shutdown();
        if (World && !Clients.IsEmpty() && !Clients[0]->bTriggered) World->DestroyWorld(false);
    }
    FSimurghRTCSessionConfig Config() const
    {
        FSimurghRTCSessionConfig C; C.Provider = ESimurghRTCProvider::DirectUnreal; C.Endpoint = Endpoint; C.AccessToken = Token;
        C.LocalIdentity = FString::Printf(TEXT("shared-%d"),Role); C.bAllowInsecureDevelopmentSignaling = true;
        C.TimeoutMilliseconds = Scenario == TEXT("timeout") ? 200 : 20000; return C;
    }
    void Fail(const TCHAR* Why) { if (Error.IsEmpty()) Error = Why; }
    FSimurghRTCChannelOptions Options() const { FSimurghRTCChannelOptions O; O.Label = TEXT("sr105-main"); return O; }
    void Create(int32 Count)
    {
        for (int32 I = 0; I < Count; ++I)
        {
            Owners.Emplace(NewObject<UGameInstance>()); Clients.Emplace(NewObject<UContractSessionEndpoint>());
            UObject* Owner = Owners.Last().Get();
            if (Scenario == TEXT("world")) { World.Reset(UWorld::CreateWorld(EWorldType::Game,false)); Owner = World.Get(); }
            auto& C = *Clients.Last(); C.Initialize(Owner);
            C.bShutdownOnCompletion = Scenario == TEXT("reentrant");
            C.Start(USimurghRTCOperation::Connect(C.Session,Config()));
        }
    }
    bool AllSuccess() const { for (const auto& C : Clients) if (!C->Success()) return false; return true; }
    bool AllReceived() const { for (const auto& C : Clients) if (!C->Expected.IsEmpty()) return false; return true; }
    FSimurghRTCDataMessage Large(int32 Seed) const
    { FSimurghRTCDataMessage M; M.Kind = ESimurghRTCMessageKind::Binary; M.Bytes.SetNumUninitialized(65536); for (int32 I=0;I<M.Bytes.Num();++I) M.Bytes[I]=uint8(I+Seed); return M; }
    TSharedRef<FJsonObject> Report(bool Ready = false)
    {
        auto J = MakeShared<FJsonObject>(); J->SetNumberField(TEXT("schema"),1); J->SetStringField(TEXT("run_id"),RunId);
        J->SetStringField(TEXT("scenario"),Scenario); J->SetNumberField(TEXT("role"),Role); J->SetNumberField(TEXT("pid"),FPlatformProcess::GetCurrentProcessId());
        J->SetStringField(TEXT("engine_version"),FEngineVersion::Current().ToString()); J->SetBoolField(TEXT("editor_build"),WITH_EDITOR!=0);
        J->SetBoolField(TEXT("server_build"),UE_SERVER!=0); J->SetBoolField(TEXT("arm64_build"),PLATFORM_CPU_ARM_FAMILY && sizeof(void*)==8);
        J->SetBoolField(TEXT("with_janus"),SIMURGHRTC_WITH_JANUS!=0); J->SetStringField(TEXT("platform"),FPlatformProperties::IniPlatformName());
        J->SetStringField(TEXT("artifact_manifest_sha256"),SimurghRTC::FNativeRuntime::GetMobileArtifactSHA256());
        J->SetStringField(TEXT("native_backend"),SimurghRTC::FNativeRuntime::GetBackendVersion());
        J->SetBoolField(TEXT("shared_api"),true); J->SetBoolField(TEXT("loopback_only"),true); J->SetBoolField(TEXT("network_media_tested"),false);
        J->SetBoolField(TEXT("passed"),Error.IsEmpty()); J->SetStringField(TEXT("error"),Error); J->SetBoolField(TEXT("ready"),Ready);
        J->SetNumberField(TEXT("phase"),Phase); J->SetNumberField(TEXT("isolation_checks"),IsolationChecks);
        J->SetBoolField(TEXT("backpressure_tested"),bPressure); J->SetBoolField(TEXT("backpressure_observed"),bBlocked); J->SetBoolField(TEXT("backpressure_recovered"),bResumed);
        const auto R = SimurghRTC::GetSessionRuntimeSnapshot(); J->SetNumberField(TEXT("transports_created"),R.Created);
        J->SetNumberField(TEXT("native_closed"),R.NativeClosed); J->SetNumberField(TEXT("remote_closed"),R.RemoteClosed); J->SetNumberField(TEXT("pending_cleanup"),R.PendingCleanup);
        J->SetNumberField(TEXT("quiet_ticks"),QuietTicks); J->SetNumberField(TEXT("elapsed_seconds"),FPlatformTime::Seconds()-Started);
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (const auto& C : Clients)
        {
            auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("facade_id"),C->FacadeId.ToString(EGuidFormats::DigitsWithHyphensLower));
            Row->SetNumberField(TEXT("received"),C->Received); Row->SetNumberField(TEXT("unexpected"),C->Unexpected);
            Row->SetNumberField(TEXT("operations"),C->Operations); Row->SetNumberField(TEXT("completions"),C->Completions);
            Row->SetBoolField(TEXT("callbacks_on_game_thread"),C->bGameThread); Row->SetBoolField(TEXT("expected_empty"),C->Expected.IsEmpty());
            Row->SetNumberField(TEXT("last_error"),C->Operation ? int32(C->Operation->GetResult().Error.Code) : -1);
            Row->SetBoolField(TEXT("triggered"),C->bTriggered); Row->SetBoolField(TEXT("session_collected"),C->bCollectSession && !CollectedSession.IsValid());
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        J->SetArrayField(TEXT("clients"),Rows); return J;
    }
    bool Save(bool Ready)
    {
        FString Text; FJsonSerializer::Serialize(Report(Ready),TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
        const auto Dir = FPaths::ProjectSavedDir()/TEXT("SimurghRTC"); IFileManager::Get().MakeDirectory(*Dir,true);
        const bool Saved = FFileHelper::SaveStringToFile(Text,*(Dir/(FString(Ready ? TEXT("session-ready-") : TEXT("session-probe-"))+RunId+TEXT(".json"))));
        UE_LOG(LogTemp,Display,TEXT("%s %s"),Ready ? TEXT("SIMURGHRTC_SESSION_READY") : TEXT("SIMURGHRTC_SESSION_REPORT"),*Text); return Saved;
    }
    void Data()
    {
        auto& A = *Clients[0]; auto& B = *Clients[1];
        if (Phase == 1 && AllSuccess())
        { for (auto& C : Clients) C->Start(USimurghRTCOperation::OpenChannel(C->Session,Options())); Phase=2; }
        else if (Phase == 2 && AllSuccess())
        {
            for (auto& C : Clients) C->Channel = C->Operation->GetResult().Channel;
            if (A.FacadeId == B.FacadeId || A.Channel.Id == B.Channel.Id) { Fail(TEXT("Sessions share identities.")); return; }
            if (!bReadySaved) { bReadySaved=true; if (!Save(true)) Fail(TEXT("Could not save readiness.")); }
            if (!IFileManager::Get().FileExists(*Gate)) return;
            if (A.Session->SendBytes(B.Channel,{}).Code != ESimurghRTCErrorCode::InvalidState
                || B.Session->SendText(A.Channel,TEXT("foreign")).Code != ESimurghRTCErrorCode::InvalidState)
            { Fail(TEXT("Foreign shared channel accepted.")); return; } IsolationChecks += 2;
            auto TooLarge = Large(0); TooLarge.Bytes.Add(0);
            if (A.Session->SendBytes(A.Channel,TooLarge.Bytes).Code != ESimurghRTCErrorCode::InvalidArgument) { Fail(TEXT("Oversized shared message accepted.")); return; }
            ++IsolationChecks; A.SendPair(Role,0); B.SendPair(Role,1); Phase=3;
        }
        else if (Phase == 3 && AllReceived())
        {
            if (bPressure)
            {
                FSimurghRTCChannelInfo Info; A.Session->GetChannelInfo(A.Channel,Info); if (Info.BufferedBytes) return;
                for (int32 I=0;I<4;++I) A.Send(Large(I));
                bBlocked=A.Session->SendBytes(A.Channel,Large(4).Bytes).Code==ESimurghRTCErrorCode::ResourceExhausted;
                if (!bBlocked) { Fail(TEXT("Shared send queue did not apply backpressure.")); return; }
                Phase=4;
            }
            else Phase=5;
        }
        else if (Phase == 4 && AllReceived())
        {
            FSimurghRTCChannelInfo Info; A.Session->GetChannelInfo(A.Channel,Info); if (Info.BufferedBytes) return;
            A.Send(Large(4)); bResumed=Error.IsEmpty() && A.Error.IsEmpty(); Phase=5;
        }
        else if (Phase == 5 && AllReceived())
        { A.OldChannel=A.Channel; A.Start(USimurghRTCOperation::CloseChannel(A.Session,A.Channel)); Phase=6; }
        else if (Phase == 6 && A.Success())
        {
            if (A.Session->SendBytes(A.OldChannel,{}).Code!=ESimurghRTCErrorCode::InvalidState) { Fail(TEXT("Closed handle accepted.")); return; }
            ++IsolationChecks; A.Start(USimurghRTCOperation::Disconnect(A.Session)); Phase=7;
        }
        else if (Phase == 7 && A.Success())
        { A.Start(USimurghRTCOperation::Connect(A.Session,Config())); Phase=8; }
        else if (Phase == 8 && A.Success())
        {
            if (A.Session->GetSessionId()!=A.FacadeId || A.Session->SendBytes(A.OldChannel,{}).Code!=ESimurghRTCErrorCode::InvalidState
                || A.Session->SendBytes(B.Channel,{}).Code!=ESimurghRTCErrorCode::InvalidState)
            { Fail(TEXT("Reconnect reused channel state.")); return; } IsolationChecks+=2;
            A.Start(USimurghRTCOperation::OpenChannel(A.Session,Options())); Phase=9;
        }
        else if (Phase == 9 && A.Success())
        {
            A.Channel=A.Operation->GetResult().Channel;
            if (A.Channel.Id==A.OldChannel.Id) { Fail(TEXT("Reconnect reused channel identity.")); return; }
            A.SendPair(Role,2); Phase=10;
        }
        else if (Phase == 10 && AllReceived())
        { for (auto& C:Clients) C->Start(USimurghRTCOperation::CloseChannel(C->Session,C->Channel)); Phase=11; }
        else if (Phase == 11 && AllSuccess())
        { for (auto& C:Clients) C->Start(USimurghRTCOperation::Disconnect(C->Session)); Phase=12; }
        else if (Phase == 12 && AllSuccess())
        { for (auto& C:Clients) { C->bQuiet=true; C->Session->Shutdown(); } Phase=100; }
    }
    void Lifecycle()
    {
        auto& C=*Clients[0];
        if (Scenario==TEXT("cancel") && Phase==1 && C.Session->GetState()==ESimurghRTCSessionState::Connecting)
        { C.Operation->Cancel(); Phase=2; }
        if (Scenario==TEXT("disconnect-pending") && Phase==1 && C.Session->GetState()==ESimurghRTCSessionState::Connecting)
        { C.Start(USimurghRTCOperation::Disconnect(C.Session)); Phase=2; }
        if ((Scenario==TEXT("owner-loss") || Scenario==TEXT("world") || Scenario==TEXT("session-gc")) && Phase==1 && C.Success())
        {
            CollectedSession=C.Session; C.Start(USimurghRTCOperation::OpenChannel(C.Session,Options())); Phase=2;
            if (Scenario==TEXT("session-gc"))
            {
                // Drop the last application reference outside callback dispatch, then collect
                // while OpenChannel is still pending. Reference release alone is not destruction.
                C.Session=nullptr; C.bCollectSession=C.bTriggered=true;
            }
            else C.Trigger=Scenario;
        }
        if (C.bCollectSession)
        {
            CollectGarbage(RF_NoFlags);
            if (!CollectedSession.IsValid()) C.bQuiet=true;
        }
        bool Done=false;
        if (Scenario==TEXT("cancel") && Phase==2 && C.Done())
        { if (C.Operation->GetResult().Error.Code!=ESimurghRTCErrorCode::Cancelled) Fail(TEXT("Connect cancellation differed.")); Done=true; }
        else if (Scenario==TEXT("disconnect-pending") && Phase==2 && C.Success()) Done=true;
        else if ((Scenario==TEXT("timeout") || Scenario==TEXT("http-timeout")) && C.Done())
        { if (C.Operation->GetResult().Error.Code!=ESimurghRTCErrorCode::Timeout) Fail(TEXT("Expected shared timeout.")); Done=true; }
        else if (Scenario==TEXT("reentrant") && C.bTriggered && C.Done()) Done=true;
        else if (C.bTriggered && C.Done() && (!C.bCollectSession || !CollectedSession.IsValid()))
        {
            if (C.Operation->GetResult().Error.Code!=ESimurghRTCErrorCode::Cancelled || C.Completions!=1)
                Fail(TEXT("Owner/world teardown did not silently settle pending channel operation."));
            Done=true;
        }
        if (Done) { C.bQuiet=true; if (IsValid(C.Session)) C.Session->Shutdown(); Phase=100; }
    }
    bool Tick(float)
    {
        if (Phase==0) { Create(Scenario==TEXT("data") ? 2 : 1); Phase=1; }
        if (Phase<100) { if (Scenario==TEXT("data")) Data(); else Lifecycle(); }
        for (const auto& C:Clients)
        {
            if (!C->Error.IsEmpty()) Error=C->Error;
            if (Scenario==TEXT("data") && C->Done() && !C->Success()) Fail(TEXT("Shared data operation failed."));
        }
        if (FPlatformTime::Seconds()-Started>90) Fail(TEXT("Shared session probe timed out."));
        if (Phase==100)
        {
            const auto R=SimurghRTC::GetSessionRuntimeSnapshot();
            if (R.Created==R.NativeClosed && R.PendingCleanup==0) ++QuietTicks;
        }
        if (!Error.IsEmpty() || QuietTicks>=8)
        { const bool Saved=Save(false); FPlatformMisc::RequestExitWithStatus(false,Error.IsEmpty() && Saved ? 0 : 5); return false; }
        return true;
    }
};
FContractSessionProbe::FContractSessionProbe()=default;
FContractSessionProbe::~FContractSessionProbe() { Shutdown(); }
void FContractSessionProbe::Startup()
{
    if (!FParse::Param(FCommandLine::Get(),TEXT("SimurghRTCSharedSessionProbe"))) return;
    Exit=FCoreDelegates::OnEnginePreExit.AddRaw(this,&FContractSessionProbe::Shutdown);
    Init=FCoreDelegates::GetOnPostEngineInit().AddLambda([this]
    {
        Impl=MakeUnique<FImpl>(); FString TokenFile;
        FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCSessionRunId="),Impl->RunId);
        FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCSessionScenario="),Impl->Scenario);
        FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCSessionEndpoint="),Impl->Endpoint);
        FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCSessionTokenFile="),TokenFile);
        FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCSessionGate="),Impl->Gate);
        FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCSessionRole="),Impl->Role);
        Impl->bPressure=FParse::Param(FCommandLine::Get(),TEXT("SimurghRTCSessionPressure"));
        FGuid Id;
        if (!FGuid::ParseExact(Impl->RunId,EGuidFormats::DigitsWithHyphens,Id)
            || !FFileHelper::LoadFileToString(Impl->Token,*TokenFile)) Impl->Fail(TEXT("Invalid shared fixture input."));
        Impl->TickHandle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(Impl.Get(),&FImpl::Tick));
    });
}
void FContractSessionProbe::Shutdown()
{ FCoreDelegates::GetOnPostEngineInit().Remove(Init); FCoreDelegates::OnEnginePreExit.Remove(Exit); Init.Reset(); Exit.Reset(); Impl.Reset(); }

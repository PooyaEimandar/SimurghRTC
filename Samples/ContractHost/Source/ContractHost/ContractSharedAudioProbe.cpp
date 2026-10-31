// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractSharedAudioProbe.h"
#include "SimurghRTCSessionTransport.h"
#include "SimurghRTCNativeRuntime.h"
#include "Containers/Ticker.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#if CONTRACTHOST_WITH_DIRECT_SERVER
#include "SimurghRTCDirectServer.h"
#include "HttpServerModule.h"
#endif
DEFINE_LOG_CATEGORY_STATIC(LogSharedAudioProbe, Log, All);
using namespace SimurghRTC;
namespace
{
FString Json(TSharedRef<FJsonObject> Object)
{
    FString Text; const auto Writer = TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(Object,Writer); return Text;
}
FSimurghRTCAudioFrame Tone(int64 Sample, int32 Count, double Frequency)
{
    FSimurghRTCAudioFrame F; F.TimestampMicroseconds = Sample * 1000000 / 48000; F.Samples.SetNumUninitialized(Count);
    for (int32 I=0;I<Count;++I) F.Samples[I]=0.25f*FMath::Sin(2*PI*Frequency*(Sample+I)/48000.0);
    return F;
}
bool Good(const FContractAudioMeter& M, bool Silent)
{ return M.Samples >= 48000 && (Silent ? M.Rms()<0.002 : M.Rms()>0.1 && M.Correlation()>0.9); }
}
void FContractAudioMeter::Add(const FSimurghRTCAudioFrame& Frame,double Frequency)
{
    if (Frame.Samples.IsEmpty() || Frame.Channels < 1) return;
    const int32 Count=Frame.Samples.Num()/Frame.Channels; double Sine=0,Cosine=0;
    for(int32 I=0;I<Count;++I)
    {
        const double Value=Frame.Samples[I*Frame.Channels], Phase=2*PI*Frequency*I/Frame.SampleRate;
        Energy+=Value*Value; Sine+=Value*FMath::Sin(Phase); Cosine+=Value*FMath::Cos(Phase);
    }
    Samples+=Count; ToneEnergy+=2*(Sine*Sine+Cosine*Cosine)/Count;
}
void UContractSharedAudioEndpoint::Begin()
{
    Started=FPlatformTime::Seconds(); Session=USimurghRTCSession::CreateSession(this);
    Session->OnTrackChanged.AddDynamic(this,&UContractSharedAudioEndpoint::Track);
    Session->OnAudioFrame.AddDynamic(this,&UContractSharedAudioEndpoint::Audio);
    Session->OnMessage.AddDynamic(this,&UContractSharedAudioEndpoint::Message);
}
void UContractSharedAudioEndpoint::Op(USimurghRTCOperation* Operation,TFunction<void(const FSimurghRTCOperationResult&)> Completed)
{
    Pending=Operation; ++Operations;
    Operation->OnCompletedNative.AddWeakLambda(this,[this,Completed=MoveTemp(Completed)](const FSimurghRTCOperationResult& Result)
    {
        Pending=nullptr;
        if(!Result.IsSuccess()) { Error=FString::Printf(TEXT("Operation failed: %d %s"),int32(Result.Error.Code),*Result.Error.Message); return; }
        Completed(Result);
    });
    Operation->Activate();
}
void UContractSharedAudioEndpoint::Track(const FSimurghRTCTrackInfo& Info,bool bAdded)
{ if(Info.bRemote && Info.Kind==ESimurghRTCTrackKind::Audio) Remote=bAdded?Info.Handle:FSimurghRTCTrackHandle{}; }
void UContractSharedAudioEndpoint::Audio(FSimurghRTCTrackHandle Handle,const FSimurghRTCAudioFrame& Frame)
{
    if(!IsInGameThread() || Handle.Id!=Remote.Id) { Error=TEXT("Wrong audio callback owner/thread."); return; }
    ++Frames;
    if(Phase>=0 && FPlatformTime::Seconds()-PhaseStarted>=1) Meter.Add(Frame,800);
}
void UContractSharedAudioEndpoint::Measure(int32 Number)
{
    Phase=Number; PhaseStarted=FPlatformTime::Seconds(); Meter={};
    const auto E=Session->SendText(Channel,FString::Printf(TEXT("measure:%d"),Number));
    if(E.IsError()) Error=TEXT("Could not start remote measurement.");
}
void UContractSharedAudioEndpoint::Message(const FSimurghRTCDataMessage& Value)
{
    TSharedPtr<FJsonObject> J;
    if(Value.Kind!=ESimurghRTCMessageKind::Text || Value.Channel.Id!=Channel.Id
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Value.Text),J) || !J.IsValid()
        || !J->HasTypedField<EJson::Number>(TEXT("phase")) || int32(J->GetNumberField(TEXT("phase")))!=Phase
        || !J->HasTypedField<EJson::Boolean>(TEXT("passed")) || !J->GetBoolField(TEXT("passed")) || !Good(Meter,false))
    { Error=FString::Printf(TEXT("Tone phase %d failed: local samples=%lld corr=%f rms=%f; remote=%s"),Phase,Meter.Samples,Meter.Correlation(),Meter.Rms(),*Value.Text); return; }
    MinimumCorrelation=FMath::Min(MinimumCorrelation,Meter.Correlation()); Measurements.Add(Meter); ++PhasesPassed;
    Step=Phase==0?7:Phase==1?9:Phase==2?13:16; Phase=-1;
}
void UContractSharedAudioEndpoint::Update()
{
    const double Now=FPlatformTime::Seconds();
    if(Now-Started>70) { Error=TEXT("Shared audio client deadline expired."); return; }
    if(!Error.IsEmpty()) return;
    if(Session->GetState()==ESimurghRTCSessionState::Failed) { Error=TEXT("Shared audio connection failed."); return; }
    for(int32 Batch=0;Batch<10 && Local.IsValid() && Step<19 && Step!=8 && Now>=NextSend && Session->GetState()==ESimurghRTCSessionState::Connected;++Batch)
    {
        // 7 ms + 13 ms proves arbitrary complete sample-frame chunking into 10 ms blocks.
        const int32 Count=SentSamples%960==0?336:624;
        const auto E=Session->PushAudioFrame(Source,Tone(SentSamples,Count,Checks>=1?600:400));
        if(!E.IsError()) { SentSamples+=Count; NextSend=FMath::Max(NextSend+double(Count)/48000.0,Now-0.03); }
        else {if(E.Code!=ESimurghRTCErrorCode::ResourceExhausted && E.Code!=ESimurghRTCErrorCode::InvalidState) Error=E.Message;break;}
    }
    if(Pending) return;
    switch(Step)
    {
    case 0: Op(USimurghRTCOperation::CreateGeneratedSource(Session,ESimurghRTCTrackKind::Audio),[this](const auto& R){Source=R.Source;Step=1;});break;
    case 1:
    {
        FSimurghRTCSessionConfig C; C.Endpoint=Endpoint; C.AccessToken=Token; C.LocalIdentity=TEXT("shared-audio-client");
        C.Purpose=ESimurghRTCPurpose::Call; C.bAllowInsecureDevelopmentSignaling=true; C.TimeoutMilliseconds=20000;
        Op(USimurghRTCOperation::Connect(Session,C),[this](const auto&){Step=2;}); break;
    }
    case 2: Op(USimurghRTCOperation::AttachSource(Session,Source),[this](const auto& R){Local=R.Track;Step=3;NextSend=FPlatformTime::Seconds();});break;
    case 3: {FSimurghRTCChannelOptions O;O.Label=TEXT("audio-control"); Op(USimurghRTCOperation::OpenChannel(Session,O),[this](const auto& R){Channel=R.Channel;Step=4;});break;}
    case 4: if(Remote.IsValid()) Op(USimurghRTCOperation::SetDecodedFrameDelivery(Session,Remote,true),[this](const auto&){Step=5;});break;
    case 5: Measure(0);Step=6;break;
    case 7: Op(USimurghRTCOperation::SetSourceMuted(Session,Source,true),[this](const auto&){Step=8;Measure(1);});break;
    case 9: Op(USimurghRTCOperation::CreateGeneratedSource(Session,ESimurghRTCTrackKind::Audio),[this](const auto& R){OldSource=Source;Source=R.Source;Step=10;});break;
    case 10: Op(USimurghRTCOperation::ReplaceSource(Session,Local,Source),[this](const auto& R)
        {if(R.Track.Id!=Local.Id) Error=TEXT("Replacement changed track identity."); ++Checks;Step=11;});break;
    case 11: Op(USimurghRTCOperation::StopSource(Session,OldSource),[this](const auto&){Step=12;});break;
    case 12: Measure(2);Step=6;break;
    case 13: Op(USimurghRTCOperation::SetDecodedFrameDelivery(Session,Remote,false),[this](const auto&){QuietFrames=Frames;QuietStarted=FPlatformTime::Seconds();Step=14;});break;
    case 14:
        if(Frames!=QuietFrames) Error=TEXT("Decoded callbacks continued after disable.");
        if(Now-QuietStarted>0.4) Op(USimurghRTCOperation::SetDecodedFrameDelivery(Session,Remote,true),[this](const auto&){++Checks;Step=15;});break;
    case 15: Measure(3);Step=6;break;
    case 16: Op(USimurghRTCOperation::DetachTrack(Session,Local),[this](const auto&){Local={};Step=17;});break;
    case 17:
        if(Session->PushAudioFrame(Source,Tone(SentSamples,480,600)).Code!=ESimurghRTCErrorCode::InvalidState) Error=TEXT("Detached source accepted input.");
        Op(USimurghRTCOperation::AttachSource(Session,Source),[this](const auto& R){Local=R.Track;++Checks;Step=18;});break;
    case 18: Op(USimurghRTCOperation::StopSource(Session,Source),[this](const auto&){Local={};Step=19;});break;
    case 19:
        if(Session->PushAudioFrame(Source,Tone(SentSamples,480,600)).Code!=ESimurghRTCErrorCode::InvalidState) Error=TEXT("Stopped source accepted input.");
        Op(USimurghRTCOperation::Disconnect(Session),[this](const auto&){++Checks;QuietFrames=Frames;Step=20;});break;
    case 20: if(Frames!=QuietFrames) Error=TEXT("Audio callback after disconnect.");break;
    }
}

class FContractSharedAudioProbe::FImpl
{
public:
    FDelegateHandle Init; FTSTicker::FDelegateHandle Ticker;
    TStrongObjectPtr<UContractSharedAudioEndpoint> End;
    FString RunId,Token,Endpoint,Error;
    bool bServer=false,bReported=false,bServerClosing=false,bOwnsHttp=false; int32 Port=0,Phase=-1,PhasesPassed=0,QuietTicks=0;
    double Started=0,NextSend=0,PhaseStarted=0,MinimumCorrelation=1,MaximumSilence=0;
    int64 SentSamples=0; FContractAudioMeter Meter; TArray<FContractAudioMeter> Measurements;
#if CONTRACTHOST_WITH_DIRECT_SERVER
    TSharedPtr<FDirectServer> Server; FString SessionId; FSimurghRTCChannelHandle Control;
#endif
    ~FImpl()
    {
        FCoreDelegates::GetOnPostEngineInit().Remove(Init); FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
        if(End && End->Session) End->Session->Shutdown();
#if CONTRACTHOST_WITH_DIRECT_SERVER
        if(Server) Server->Stop();
        if(bOwnsHttp) FHttpServerModule::Get().StopAllListeners();
#endif
    }
    void Begin()
    {
        Started=FPlatformTime::Seconds();
#if CONTRACTHOST_WITH_DIRECT_SERVER
        if(bServer)
        {
            if(!IsRunningDedicatedServer() || FHttpServerModule::IsAvailable())
            {Error=TEXT("Audio Server probe requires a dedicated process with no existing HTTPServer owner.");Report();return;}
            End.Reset(NewObject<UContractSharedAudioEndpoint>());
            FDirectServerConfig C; C.bLoopbackProbe=true;
            C.Admit=[this](const FDirectAdmissionRequest& R){FDirectAdmissionResult A;if(R.BearerToken==Token){A.Principal=TEXT("audio-fixture");A.bAllowCreate=R.Purpose==TEXT("call");}return A;};
            C.OnSessionChanged=[this](const FDirectSessionInfo& I)
            {
                if(I.State==EDirectServerSessionState::Connected && SessionId.IsEmpty())
                {SessionId=I.SessionId;NextSend=FPlatformTime::Seconds();if(Server->MuteAudio(SessionId,false).IsError()) Error=TEXT("Server audio enable failed.");}
            };
            C.OnChannelEvent=[this](const FString& Id,const FNativeChannelEvent& E)
            {
                if(E.Type!=ENativeDataEvent::Message) return;
                if(Id!=SessionId || E.Message.Kind!=ESimurghRTCMessageKind::Text || E.Message.Text!=FString::Printf(TEXT("measure:%d"),PhasesPassed))
                {Error=TEXT("Unexpected audio control message.");return;}
                Control=E.Message.Channel;Phase=PhasesPassed;PhaseStarted=FPlatformTime::Seconds();Meter={};
            };
            TArray<FString> Original, Overrides;
            GConfig->GetArray(TEXT("HTTPServer.Listeners"),TEXT("ListenerOverrides"),Original,GEngineIni);
            Overrides=Original;Overrides.Insert(FString::Printf(TEXT("(Port=%d,BindAddress=127.0.0.1,ReuseAddressAndPort=false)"),Port),0);
            GConfig->SetArray(TEXT("HTTPServer.Listeners"),TEXT("ListenerOverrides"),Overrides,GEngineIni);
            auto& Http=FHttpServerModule::Get();bOwnsHttp=true;Http.StartAllListeners();
            auto Router=Http.GetHttpRouter(Port,true);
            GConfig->SetArray(TEXT("HTTPServer.Listeners"),TEXT("ListenerOverrides"),Original,GEngineIni);
            if(Router) Server=FDirectServer::Bind(End.Get(),Router.ToSharedRef(),MoveTemp(C),Error);
            else Error=TEXT("Could not bind the loopback HTTP listener.");
            if(Server) {UE_LOG(LogSharedAudioProbe,Display,TEXT("SIMURGHRTC_SHARED_AUDIO_READY %s"),*RunId);}
        }
        else
#endif
        {
            if(bServer) Error=TEXT("Server module is excluded.");
            End.Reset(NewObject<UContractSharedAudioEndpoint>());End->Endpoint=Endpoint;End->Token=Token;End->Begin();
        }
        Ticker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this,&FImpl::Tick));
    }
    void Report()
    {
        if(bReported)return;bReported=true;
        auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("schema"),1);J->SetStringField(TEXT("run_id"),RunId);
        J->SetNumberField(TEXT("pid"),FPlatformProcess::GetCurrentProcessId());J->SetBoolField(TEXT("server"),bServer);
        J->SetBoolField(TEXT("server_build"),UE_SERVER!=0);J->SetBoolField(TEXT("editor_build"),WITH_EDITOR!=0);
        J->SetBoolField(TEXT("with_janus"),SIMURGHRTC_WITH_JANUS!=0);J->SetBoolField(TEXT("loopback_only"),true);J->SetBoolField(TEXT("audio_hardware_opened"),false);
        J->SetStringField(TEXT("engine_version"),FEngineVersion::Current().ToString());J->SetStringField(TEXT("artifact_manifest_sha256"),FNativeRuntime::GetMobileArtifactSHA256());
        J->SetStringField(TEXT("error"),Error);J->SetNumberField(TEXT("phases"),bServer?PhasesPassed:End->PhasesPassed);
        J->SetNumberField(TEXT("minimum_correlation"),bServer?MinimumCorrelation:End->MinimumCorrelation);J->SetNumberField(TEXT("maximum_muted_rms"),MaximumSilence);
        J->SetNumberField(TEXT("quiet_ticks"),QuietTicks);J->SetNumberField(TEXT("checks"),bServer?0:End->Checks);
        TArray<TSharedPtr<FJsonValue>> Rows;
        const auto& Values=bServer?Measurements:End->Measurements;
        for(int32 I=0;I<Values.Num();++I)
        {
            auto Row=MakeShared<FJsonObject>();Row->SetNumberField(TEXT("phase"),I);Row->SetNumberField(TEXT("samples"),double(Values[I].Samples));
            Row->SetNumberField(TEXT("correlation"),Values[I].Correlation());Row->SetNumberField(TEXT("rms"),Values[I].Rms());Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        J->SetArrayField(TEXT("measurements"),Rows);
        const auto S=GetSessionRuntimeSnapshot();J->SetNumberField(TEXT("client_native_closed"),S.NativeClosed);J->SetNumberField(TEXT("client_remote_closed"),S.RemoteClosed);J->SetNumberField(TEXT("pending_cleanup"),S.PendingCleanup);
#if CONTRACTHOST_WITH_DIRECT_SERVER
        if(Server){const auto R=Server->GetSnapshot();J->SetNumberField(TEXT("server_live_peers"),R.LivePeers);J->SetNumberField(TEXT("server_closed"),R.Closed);}
#endif
        J->SetBoolField(TEXT("passed"),Error.IsEmpty() && QuietTicks>=8 && (bServer?PhasesPassed:End->PhasesPassed)==4);
        UE_LOG(LogSharedAudioProbe,Display,TEXT("SIMURGHRTC_SHARED_AUDIO_REPORT %s"),*Json(J));
        FPlatformMisc::RequestExitWithStatus(false,J->GetBoolField(TEXT("passed"))?0:1);
    }
    bool Tick(float)
    {
        const double Now=FPlatformTime::Seconds();if(Now-Started>90)Error=TEXT("Shared audio process deadline expired.");
#if CONTRACTHOST_WITH_DIRECT_SERVER
        if(bServer && Server)
        {
            if(!SessionId.IsEmpty())
            {
                FDirectSessionInfo Info;Server->GetSessionInfo(SessionId,Info);
                for(int32 Batch=0;Batch<4 && Info.State==EDirectServerSessionState::Connected && Now>=NextSend;++Batch)
                {
                    const auto E=Server->PushAudio(SessionId,Tone(SentSamples,960,800));
                    if(!E.IsError()){SentSamples+=960;NextSend=FMath::Max(NextSend+0.02,Now-0.03);}
                    else {if(E.Code!=ESimurghRTCErrorCode::ResourceExhausted)Error=TEXT("Server PCM submission failed.");break;}
                }
                FSimurghRTCAudioFrame F;for(int32 I=0;I<20 && Server->PollAudio(SessionId,F);++I)
                    if(Phase>=0 && Now-PhaseStarted>=1) Meter.Add(F,Phase>=2?600:400);
                if(Phase>=0 && Now-PhaseStarted>=2.5)
                {
                    const bool Pass=Good(Meter,Phase==1);auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("phase"),Phase);J->SetBoolField(TEXT("passed"),Pass);
                    J->SetNumberField(TEXT("correlation"),Meter.Correlation());J->SetNumberField(TEXT("rms"),Meter.Rms());J->SetNumberField(TEXT("samples"),double(Meter.Samples));
                    if(Phase==1)MaximumSilence=Meter.Rms();else MinimumCorrelation=FMath::Min(MinimumCorrelation,Meter.Correlation());
                    if(Server->SendText(SessionId,Control,Json(J)).IsError())Error=TEXT("Server audio result send failed.");
                    if(!Pass)Error=TEXT("Server tone/silence measurement failed.");Measurements.Add(Meter);++PhasesPassed;Phase=-1;
                }
                if(Info.bNativeClosed && !bServerClosing) {bServerClosing=true;Server->Stop();}
                if(bServerClosing && Server->GetSnapshot().LivePeers==0)++QuietTicks;
            }
        }
        else
#endif
        {
            End->Update();if(!End->Error.IsEmpty())Error=End->Error;
            if(End->Step==20 && GetSessionRuntimeSnapshot().PendingCleanup==0)++QuietTicks;
        }
        if(!Error.IsEmpty() || QuietTicks>=8){Report();return false;}return true;
    }
};
FContractSharedAudioProbe::FContractSharedAudioProbe()=default;
FContractSharedAudioProbe::~FContractSharedAudioProbe(){Shutdown();}
void FContractSharedAudioProbe::Startup()
{
#if !UE_BUILD_SHIPPING
    if(!FParse::Param(FCommandLine::Get(),TEXT("SimurghRTCSharedAudioProbe")))return;
    Exit=FCoreDelegates::OnEnginePreExit.AddRaw(this,&FContractSharedAudioProbe::Shutdown);
    Impl=MakeUnique<FImpl>();Impl->bServer=FParse::Param(FCommandLine::Get(),TEXT("SimurghRTCSharedAudioServer"));
    FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCSharedAudioRunId="),Impl->RunId);
    FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCSharedAudioEndpoint="),Impl->Endpoint);
    FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCSharedAudioPort="),Impl->Port);
    FString TokenFile;FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCSharedAudioTokenFile="),TokenFile);
    if(!FFileHelper::LoadFileToString(Impl->Token,*TokenFile))Impl->Error=TEXT("Cannot read probe credential.");
    Impl->Init=FCoreDelegates::GetOnPostEngineInit().AddRaw(Impl.Get(),&FImpl::Begin);
#endif
}
void FContractSharedAudioProbe::Shutdown()
{FCoreDelegates::OnEnginePreExit.Remove(Exit);Exit.Reset();Impl.Reset();}

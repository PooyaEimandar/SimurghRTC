// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativeRuntime.h"
#include "HAL/PlatformTLS.h"
#if SIMURGHRTC_WITH_WEBRTC
#include "simurgh_rtc_abi.h"
#endif

namespace SimurghRTC
{
class FNativeRuntime::FImpl
{
public:
    int32 AudioSampleRate = 0, AudioChannels = 0;
#if SIMURGHRTC_WITH_WEBRTC
    SR_Runtime* Handle = nullptr;
    TUniquePtr<SR_SignalEvent> SignalEvent;
    TUniquePtr<SR_DataEvent> DataEvent;
    SR_IceRoute IceRoute{sizeof(SR_IceRoute), SR_ABI_VERSION, 0, 0, 0, 0};
    ~FImpl() { SimurghRTC_DestroyV1(Handle); }
#endif
};
FNativeRuntime::FNativeRuntime() : OwnerThreadId(FPlatformTLS::GetCurrentThreadId()) {}
FNativeRuntime::~FNativeRuntime() { Stop(); }
bool FNativeRuntime::IsSupported() { return SIMURGHRTC_WITH_WEBRTC != 0; }
FString FNativeRuntime::GetBackendVersion()
{
#if SIMURGHRTC_WITH_WEBRTC
    return UTF8_TO_TCHAR(SimurghRTC_NativeVersionV1(3));
#else
    return TEXT("unavailable");
#endif
}
FString FNativeRuntime::GetMobileArtifactSHA256()
{
#ifdef SIMURGHRTC_NATIVE_ARTIFACT_SHA256
    return TEXT(SIMURGHRTC_NATIVE_ARTIFACT_SHA256);
#else
    return {};
#endif
}
bool FNativeRuntime::Start(ENativeFactoryMode Mode, FString& OutError)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId());
    OutError.Reset();
    if (Impl) { OutError = TEXT("Factory is already started; stop it before restarting."); return false; }
    if (Mode != ENativeFactoryMode::DataOnly && Mode != ENativeFactoryMode::BuiltinMedia)
    { OutError = TEXT("Invalid native factory mode."); return false; }
#if SIMURGHRTC_WITH_WEBRTC
    auto Candidate = MakeUnique<FImpl>();
    char Error[1024]{};
    if (SimurghRTC_CreateV1(Mode == ENativeFactoryMode::BuiltinMedia ? 1 : 0, &Candidate->Handle, Error, sizeof(Error)))
    { OutError = UTF8_TO_TCHAR(Error); return false; }
    Impl = MoveTemp(Candidate);
    return true;
#else
    OutError = TEXT("The isolated native backend is not available for this target.");
    return false;
#endif
}
bool FNativeRuntime::CreateProbePeerConnection(FString& OutError, bool bLoopbackOnly, const FNativeIceConfig& Ice)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId());
    OutError.Reset();
    if (!Impl) { OutError = TEXT("Start the factory before creating a probe peer."); return false; }
#if SIMURGHRTC_WITH_WEBRTC
    char Error[1024]{};
    if (bLoopbackOnly && SimurghRTC_EnableLoopbackProbeV1(Impl->Handle))
    { OutError = TEXT("Loopback probe mode must be enabled before peer creation."); return false; }
    const auto IceError = ValidateIceServers(Ice.Servers, Ice.bRelayOnly);
    if (IceError.IsError()) { OutError = IceError.Message; return false; }
    // Keep converted buffers stable until the C ABI copies them.
    TArray<TArray<uint8>> Strings; Strings.Reserve(48);
    const auto Store = [&Strings](const FString& Text) -> const char*
    {
        FTCHARToUTF8 Utf8(*Text, Text.Len()); TArray<uint8> Bytes;
        Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); Bytes.Add(0);
        Strings.Add(MoveTemp(Bytes)); return reinterpret_cast<const char*>(Strings.Last().GetData());
    };
    TArray<SR_IceServer> Servers;
    for (const auto& Server : Ice.Servers) for (const auto& Url : Server.Urls)
    {
        SR_IceServer Entry{}; Entry.url = Store(Url); Entry.url_bytes = Strings.Last().Num() - 1;
        Entry.username = Store(Server.Username); Entry.username_bytes = Strings.Last().Num() - 1;
        Entry.credential = Store(Server.Credential); Entry.credential_bytes = Strings.Last().Num() - 1;
        Servers.Add(Entry);
    }
    SR_IceConfig Config{sizeof(SR_IceConfig), SR_ABI_VERSION, Servers.GetData(), static_cast<uint32>(Servers.Num()), Ice.bRelayOnly ? 1 : 0};
    if (SimurghRTC_ConfigureIceV1(Impl->Handle, &Config))
    { OutError = TEXT("Native ICE configuration was rejected."); return false; }
    if (SimurghRTC_CreatePeerV1(Impl->Handle, Error, sizeof(Error)))
    { OutError = UTF8_TO_TCHAR(Error); return false; }
    return true;
#else
    return false;
#endif
}
FNativeFactoryDiagnostics FNativeRuntime::GetDiagnostics() const
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId());
    FNativeFactoryDiagnostics Result;
#if SIMURGHRTC_WITH_WEBRTC
    if (!Impl) return Result;
    SR_Diagnostics Native{};
    Native.size = sizeof(Native); Native.abi_version = SR_ABI_VERSION;
    if (SimurghRTC_DiagnosticsV1(Impl->Handle, &Native)) return Result;
    Result.bFactoryReady = Native.factory_ready != 0;
    Result.bPeerConnectionReady = Native.peer_ready != 0;
    Result.bDummyAudioDevice = Native.dummy_audio != 0;
    Result.bAudioProcessing = Native.audio_processing != 0;
    FString(UTF8_TO_TCHAR(Native.audio_codecs)).ParseIntoArray(Result.AudioCodecs, TEXT(";"), true);
    FString(UTF8_TO_TCHAR(Native.video_codecs)).ParseIntoArray(Result.VideoCodecs, TEXT(";"), true);
#endif
    return Result;
}
bool FNativeRuntime::Signal(uint64 Id, const FNativeSignalRequest& Request, FNativeSignalResult& OutFailure)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId());
    OutFailure = {}; OutFailure.OperationId = Id;
#if SIMURGHRTC_WITH_WEBRTC
    if (Impl)
    {
        FTCHARToUTF8 Text(*(Request.Action == ENativeSignalAction::AddIce ? Request.Ice.Candidate : Request.Sdp));
        FTCHARToUTF8 Mid(*Request.Ice.Mid), Ufrag(*Request.Ice.UsernameFragment);
        SR_SignalRequest Native{};
        Native.size = sizeof(Native); Native.abi_version = SR_ABI_VERSION; Native.operation_id = Id;
        Native.action = static_cast<int32>(Request.Action); Native.sdp_type = static_cast<int32>(Request.SdpType);
        Native.timeout_ms = Request.TimeoutMs; Native.sdp_mline_index = Request.Ice.MLineIndex;
        Native.text = Text.Get(); Native.text_bytes = Text.Length();
        Native.sdp_mid = Mid.Get(); Native.sdp_mid_bytes = Mid.Length();
        Native.username_fragment = Ufrag.Get(); Native.username_fragment_bytes = Ufrag.Length();
        char Error[1024]{};
        const int Code = SimurghRTC_SignalV1(Impl->Handle, &Native, Error, sizeof(Error));
        if (!Code) return true;
        OutFailure.Error = static_cast<ENativeSignalError>(Code); OutFailure.Message = UTF8_TO_TCHAR(Error);
        return false;
    }
#endif
    OutFailure.Error = ENativeSignalError::InvalidState; OutFailure.Message = TEXT("Native peer is unavailable.");
    return false;
}
bool FNativeRuntime::PollSignal(FNativeTransportEvent& OutEvent)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId());
    OutEvent = {};
#if SIMURGHRTC_WITH_WEBRTC
    if (!Impl) return false;
    if (!Impl->SignalEvent)
    {
        Impl->SignalEvent = MakeUnique<SR_SignalEvent>();
        Impl->SignalEvent->size = sizeof(SR_SignalEvent); Impl->SignalEvent->abi_version = SR_ABI_VERSION;
    }
    auto& Native = *Impl->SignalEvent;
    if (SimurghRTC_PollEventV1(Impl->Handle, &Native)) return false;
    auto& T = OutEvent.Transport;
    T.Connection = static_cast<ENativeConnectionState>(Native.transport.connection);
    T.bLocalDescriptionSet = Native.transport.local_description_set != 0;
    T.bRemoteDescriptionSet = Native.transport.remote_description_set != 0;
    T.bLocalIceComplete = Native.transport.local_ice_complete != 0;
    T.bRemoteIceComplete = Native.transport.remote_ice_complete != 0;
    T.bDataTransportOpen = Native.transport.data_transport_open != 0;
    T.RemoteIceQueued = Native.transport.remote_ice_queued; T.RemoteIceApplied = Native.transport.remote_ice_applied;
    T.IceState = Native.transport.ice_state; T.DtlsState = Native.transport.dtls_state; T.SelectedProtocol = Native.transport.selected_protocol;
    T.LocalUdpCandidates = Native.transport.local_udp_candidates; T.LocalTcpCandidates = Native.transport.local_tcp_candidates;
    T.CandidateErrors = Native.transport.candidate_errors; T.LastCandidateError = Native.transport.last_candidate_error;
    T.StatsSamples = Native.transport.stats_samples; T.StatsAgeMs = Native.transport.stats_age_ms;
    T.CandidatePairs = Native.transport.candidate_pairs; T.SucceededPairs = Native.transport.succeeded_pairs; T.FailedPairs = Native.transport.failed_pairs;
    T.StunRequestsSent = Native.transport.stun_requests_sent; T.StunResponsesReceived = Native.transport.stun_responses_received;
    T.PacketsDiscardedOnSend = Native.transport.packets_discarded_on_send;
    if (Native.transport.stats_samples != Impl->IceRoute.stats_samples)
        SimurghRTC_GetIceRouteV1(Impl->Handle, &Impl->IceRoute);
    T.IceRouteSamples = Impl->IceRoute.stats_samples;
    T.SelectedLocalCandidate = Impl->IceRoute.local_type; T.SelectedRemoteCandidate = Impl->IceRoute.remote_type;
    T.RelayProtocol = Impl->IceRoute.relay_protocol;
    OutEvent.Type = static_cast<ENativeSignalEvent>(Native.type);
    if (OutEvent.Type == ENativeSignalEvent::None) return true;
    auto& Result = OutEvent.Result;
    Result.OperationId = Native.operation_id; Result.Error = static_cast<ENativeSignalError>(Native.error_code);
    Result.Message = UTF8_TO_TCHAR(Native.error); Result.bQueued = Native.queued != 0;
    Result.SdpType = static_cast<ENativeSdpType>(Native.sdp_type);
    if (Result.SdpType != ENativeSdpType::None) Result.Sdp = UTF8_TO_TCHAR(Native.text);
    if (OutEvent.Type == ENativeSignalEvent::LocalIce)
    {
        OutEvent.Ice.Candidate = UTF8_TO_TCHAR(Native.text); OutEvent.Ice.Mid = UTF8_TO_TCHAR(Native.sdp_mid);
        OutEvent.Ice.UsernameFragment = UTF8_TO_TCHAR(Native.username_fragment);
        OutEvent.Ice.MLineIndex = Native.sdp_mline_index;
    }
    return true;
#else
    return false;
#endif
}
namespace
{
#if SIMURGHRTC_WITH_WEBRTC
ESimurghRTCErrorCode DataError(int Code)
{
    switch (Code)
    {
    case SR_OK: return ESimurghRTCErrorCode::None;
    case SR_INVALID_ARGUMENT: return ESimurghRTCErrorCode::InvalidArgument;
    case SR_INVALID_STATE: return ESimurghRTCErrorCode::InvalidState;
    case SR_RESOURCE_EXHAUSTED: return ESimurghRTCErrorCode::ResourceExhausted;
    case SR_TIMED_OUT: return ESimurghRTCErrorCode::Timeout;
    default: return ESimurghRTCErrorCode::InternalError;
    }
}
void DataInfo(const SR_ChannelInfo& Native, FNativeDataResult& Out)
{
    Out.ChannelId = Native.channel_id;
    auto& S = Out.Channel;
    S.State = static_cast<ENativeChannelState>(Native.state);
    S.bRemote = Native.remote != 0;
    S.Info.bOpen = S.State == ENativeChannelState::Open;
    S.Info.Options.Label = UTF8_TO_TCHAR(Native.label);
    S.Info.Options.bOrdered = Native.ordered != 0;
    S.Info.Options.Reliability = static_cast<ESimurghRTCReliability>(Native.reliability);
    S.Info.Options.ReliabilityValue = Native.reliability_value;
    S.Info.MaximumMessageBytes = Native.maximum_message_bytes;
    S.NegotiatedMaximumMessageBytes = Native.negotiated_message_bytes;
    S.Info.BufferedBytes = static_cast<int64>(Native.buffered_bytes);
    S.bWritable = S.Info.bOpen && S.Info.MaximumMessageBytes > 0 && S.Info.BufferedBytes < FNativeDataLimits::ChannelBufferBytes;
}
#endif
}
ESimurghRTCErrorCode FNativeRuntime::Data(const FNativeDataRequest& Request, FNativeDataResult& Out)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId()); Out = {};
#if SIMURGHRTC_WITH_WEBRTC
    if (Impl)
    {
        FTCHARToUTF8 Label(*Request.Options.Label, Request.Options.Label.Len());
        SR_ChannelRequest Q{}; Q.size = sizeof(Q); Q.abi_version = SR_ABI_VERSION;
        Q.action = static_cast<int32>(Request.Action); Q.channel_id = Request.ChannelId;
        Q.ordered = Request.Options.bOrdered; Q.reliability = static_cast<int32>(Request.Options.Reliability);
        Q.reliability_value = Request.Options.ReliabilityValue;
        Q.label = Label.Get(); Q.label_bytes = Label.Length();
        Q.data = Request.Bytes.GetData(); Q.data_bytes = Request.Bytes.Num(); Q.kind = static_cast<int32>(Request.Kind);
        auto Native = MakeUnique<SR_ChannelInfo>();
        Out.Error = DataError(SimurghRTC_ChannelV1(Impl->Handle, &Q, Native.Get()));
        DataInfo(*Native, Out); return Out.Error;
    }
#endif
    return Out.Error = ESimurghRTCErrorCode::InvalidState;
}
bool FNativeRuntime::PollData(FNativeDataResult& Out)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId()); Out = {};
#if SIMURGHRTC_WITH_WEBRTC
    if (!Impl) return false;
    if (!Impl->DataEvent)
    {
        Impl->DataEvent = MakeUnique<SR_DataEvent>();
        Impl->DataEvent->size = sizeof(SR_DataEvent); Impl->DataEvent->abi_version = SR_ABI_VERSION;
    }
    auto& Native = *Impl->DataEvent;
    if (SimurghRTC_PollDataV1(Impl->Handle, &Native)) return false;
    Out.Type = static_cast<ENativeDataEvent>(Native.type);
    if (Out.Type == ENativeDataEvent::None) return true;
    Out.Error = DataError(Native.error_code);
    if (Out.Type == ENativeDataEvent::Error) return true;
    DataInfo(Native.channel, Out);
    Out.Kind = static_cast<ESimurghRTCMessageKind>(Native.kind);
    if (Out.Type == ENativeDataEvent::Message) Out.Bytes.Append(Native.data, Native.data_bytes);
    return true;
#else
    return false;
#endif
}
ENativeSignalError FNativeRuntime::ConfigureAudio(int32 Rate, int32 Channels)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId());
#if SIMURGHRTC_WITH_WEBRTC
    if (Impl)
    {
        SR_AudioConfig Config{sizeof(Config), SR_ABI_VERSION, Rate, Channels};
        const auto Code = static_cast<ENativeSignalError>(SimurghRTC_ConfigureAudioV1(Impl->Handle, &Config));
        if (Code == ENativeSignalError::None) { Impl->AudioSampleRate = Rate; Impl->AudioChannels = Channels; }
        return Code;
    }
#endif
    return ENativeSignalError::InvalidState;
}
ENativeSignalError FNativeRuntime::PushAudio(const FSimurghRTCAudioFrame& Frame)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId());
    if (ValidateAudioFrame(Frame).IsError()) return ENativeSignalError::InvalidArgument;
#if SIMURGHRTC_WITH_WEBRTC
    if (Impl)
    {
        if (!Impl->AudioSampleRate) return ENativeSignalError::InvalidState;
        if (Frame.SampleRate != Impl->AudioSampleRate || Frame.Channels != Impl->AudioChannels
            || Frame.Samples.Num() != Frame.SampleRate / 100 * Frame.Channels) return ENativeSignalError::InvalidArgument;
        SR_AudioInput Input{sizeof(Input), SR_ABI_VERSION, Frame.Samples.GetData(), static_cast<uint32>(Frame.Samples.Num()), Frame.TimestampMicroseconds};
        return static_cast<ENativeSignalError>(SimurghRTC_PushAudioV1(Impl->Handle, &Input));
    }
#endif
    return ENativeSignalError::InvalidState;
}
ENativeSignalError FNativeRuntime::PollAudio(FSimurghRTCAudioFrame& Out)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId()); Out = {};
#if SIMURGHRTC_WITH_WEBRTC
    if (Impl)
    {
        SR_AudioOutput Native{}; Native.size = sizeof(Native); Native.abi_version = SR_ABI_VERSION;
        const auto Code = static_cast<ENativeSignalError>(SimurghRTC_PollAudioV1(Impl->Handle, &Native));
        if (Code == ENativeSignalError::None && Native.sample_count)
        {
            if (Native.sample_count > SR_AUDIO_MAX_SAMPLES) return ENativeSignalError::NativeError;
            Out.SampleRate = Native.sample_rate; Out.Channels = Native.channels; Out.TimestampMicroseconds = Native.timestamp_us;
            Out.Samples.Append(Native.samples, Native.sample_count);
            if (ValidateAudioFrame(Out).IsError()) { Out = {}; return ENativeSignalError::NativeError; }
        }
        return Code;
    }
#endif
    return ENativeSignalError::InvalidState;
}
ENativeSignalError FNativeRuntime::AudioState(FNativeAudioState& Out)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId()); Out = {};
#if SIMURGHRTC_WITH_WEBRTC
    if (Impl)
    {
        SR_AudioState Native{}; Native.size = sizeof(Native); Native.abi_version = SR_ABI_VERSION;
        const auto Code = static_cast<ENativeSignalError>(SimurghRTC_AudioStateV1(Impl->Handle, &Native));
        if (Code == ENativeSignalError::None)
        {
            Out.QueuedInputBlocks = Native.queued_input_blocks; Out.QueuedOutputBlocks = Native.queued_output_blocks;
            Out.AdmittedBlocks = Native.admitted_blocks; Out.ConsumedBlocks = Native.consumed_blocks;
            Out.UnderrunBlocks = Native.underrun_blocks; Out.ReceivedBlocks = Native.received_blocks;
            Out.DroppedOutputBlocks = Native.dropped_output_blocks; Out.bMuted = Native.muted; Out.bRemoteTrack = Native.remote_track;
        }
        return Code;
    }
#endif
    return ENativeSignalError::InvalidState;
}
ENativeSignalError FNativeRuntime::MuteAudio(bool bMuted)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId());
#if SIMURGHRTC_WITH_WEBRTC
    if (Impl) return static_cast<ENativeSignalError>(SimurghRTC_MuteAudioV1(Impl->Handle, bMuted ? 1 : 0));
#endif
    return ENativeSignalError::InvalidState;
}
void FNativeRuntime::Stop() { check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId()); Impl.Reset(); }
}

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
#if SIMURGHRTC_WITH_WEBRTC
    SR_Runtime* Handle = nullptr;
    TUniquePtr<SR_SignalEvent> SignalEvent;
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
bool FNativeRuntime::CreateProbePeerConnection(FString& OutError, bool bLoopbackOnly)
{
    check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId());
    OutError.Reset();
    if (!Impl) { OutError = TEXT("Start the factory before creating a probe peer."); return false; }
#if SIMURGHRTC_WITH_WEBRTC
    char Error[1024]{};
    if (bLoopbackOnly && SimurghRTC_EnableLoopbackProbeV1(Impl->Handle))
    { OutError = TEXT("Loopback probe mode must be enabled before peer creation."); return false; }
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
void FNativeRuntime::Stop() { check(OwnerThreadId == FPlatformTLS::GetCurrentThreadId()); Impl.Reset(); }
}

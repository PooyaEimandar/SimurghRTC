// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativeRuntime.h"
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
    ~FImpl() { check(IsInGameThread()); SimurghRTC_DestroyV1(Handle); }
#endif
};
FNativeRuntime::FNativeRuntime() = default;
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
    check(IsInGameThread());
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
bool FNativeRuntime::CreateProbePeerConnection(FString& OutError)
{
    check(IsInGameThread());
    OutError.Reset();
    if (!Impl) { OutError = TEXT("Start the factory before creating a probe peer."); return false; }
#if SIMURGHRTC_WITH_WEBRTC
    char Error[1024]{};
    if (SimurghRTC_CreatePeerV1(Impl->Handle, Error, sizeof(Error)))
    { OutError = UTF8_TO_TCHAR(Error); return false; }
    return true;
#else
    return false;
#endif
}
FNativeFactoryDiagnostics FNativeRuntime::GetDiagnostics() const
{
    check(IsInGameThread());
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
void FNativeRuntime::Stop() { check(IsInGameThread()); Impl.Reset(); }
}

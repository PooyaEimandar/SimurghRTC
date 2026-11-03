// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCMicrophoneInternal.h"
#include "SimurghRTCMicrophone.h"
#include "SimurghRTCMicrophoneBuffer.h"
#include "Async/Async.h"
#include "AudioCaptureDeviceInterface.h"
#include "Containers/Ticker.h"
#include "Features/IModularFeatures.h"
#include "HAL/PlatformProcess.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Mac/MacSystemIncludes.h"
#include "Apple/PreAppleSystemHeaders.h"
#import <AVFoundation/AVFoundation.h>
#include "Apple/PostAppleSystemHeaders.h"

namespace SimurghRTC
{
namespace
{
struct FPermission { TAtomic<int32> Status{0}; }; // 0 pending, 1 granted, 2 denied, 3 missing usage description
struct FState
{
    FMicrophoneBuffer Buffer;
    TAtomic<bool> Stop{false}, Ready{false}, Opened{false}, Closed{false};
    TAtomic<int32> Failure{0};
    TAtomic<double> LastCallback{0};
};
struct FJob { TSharedPtr<FState, ESPMode::ThreadSafe> State; TFuture<void> Future; };
TArray<FJob> Jobs;
FMicrophoneSnapshot Completed;
FTSTicker::FDelegateHandle Reaper;
Audio::IAudioCaptureFactory* Factory = nullptr;
bool bStopping = true;

void AddSnapshot(FMicrophoneSnapshot& Out, const FState& State)
{
    uint64 Callbacks, Frames, Dropped; int32 Peak; bool bGameThread;
    State.Buffer.Snapshot(Callbacks, Frames, Dropped, Peak, bGameThread);
    Out.Callbacks += Callbacks; Out.InputFrames += Frames; Out.DroppedSamples += Dropped;
    Out.PeakQueuedSamples = FMath::Max(Out.PeakQueuedSamples, Peak); Out.bCallbackOnGameThread |= bGameThread;
    Out.Opened += State.Opened.Load() ? 1 : 0; Out.Closed += State.Closed.Load() ? 1 : 0;
}
void Reap()
{
    for (int32 I = Jobs.Num() - 1; I >= 0; --I) if (Jobs[I].Future.IsReady())
    { Jobs[I].Future.Get(); AddSnapshot(Completed, *Jobs[I].State); Jobs.RemoveAt(I); }
}
FString DeviceId(const Audio::FCaptureDeviceInfo& Info)
{
    // Do not expose the engine's raw device identifier as an application handle.
    FTCHARToUTF8 Text(*Info.DeviceId); FSHAHash Hash; FSHA1::HashBuffer(Text.Get(), Text.Length(), Hash.Hash);
    return TEXT("mac-input-") + Hash.ToString();
}
void RunCapture(TSharedPtr<FState, ESPMode::ThreadSafe> State, FString Selected, Audio::IAudioCaptureFactory* CaptureFactory)
{
    @autoreleasepool
    {
        auto Stream = CaptureFactory->CreateNewAudioCaptureStream();
        Audio::FCaptureDeviceInfo Info{}; Audio::FAudioCaptureDeviceParams Params;
        bool bFound = false;
        if (Stream && !State->Stop.Load())
        {
            if (Selected.IsEmpty()) bFound = Stream->GetCaptureDeviceInfo(Info, INDEX_NONE);
            else
            {
                TArray<Audio::FCaptureDeviceInfo> Devices;
                if (Stream->GetInputDevicesAvailable(Devices)) for (int32 I = 0; I < Devices.Num(); ++I)
                    if (DeviceId(Devices[I]) == Selected) { Info = Devices[I]; Params.DeviceIndex = I; bFound = true; break; }
            }
        }
        if (!bFound || Info.InputChannels <= 0) State->Failure.Store(1);
        else if (Info.PreferredSampleRate != 8000 && Info.PreferredSampleRate != 16000 && Info.PreferredSampleRate != 32000
            && Info.PreferredSampleRate != 44100 && Info.PreferredSampleRate != 48000) State->Failure.Store(2);
        else if (!State->Stop.Load())
        {
            Params.SampleRate = Info.PreferredSampleRate; Params.NumInputChannels = 1;
            Params.PCMAudioEncoding = Audio::EPCMAudioEncoding::FLOATING_POINT_32;
            const bool bOpen = Stream->OpenAudioCaptureStream(Params,
                [State](const void* Data, int32 Frames, int32 Channels, int32 Rate, double, bool)
                {
                    State->LastCallback.Store(FPlatformTime::Seconds());
                    if (!State->Buffer.Push(static_cast<const float*>(Data), Frames, Channels, Rate)) State->Failure.Store(2);
                }, 480);
            if (!bOpen) State->Failure.Store(3);
            else
            {
                State->Opened.Store(true); State->LastCallback.Store(FPlatformTime::Seconds());
                if (!State->Stop.Load() && Stream->StartStream())
                {
                    State->Ready.Store(true);
                    while (!State->Stop.Load() && !State->Failure.Load())
                    {
                        if (!Stream->IsCapturing() || FPlatformTime::Seconds() - State->LastCallback.Load() > 3.0) State->Failure.Store(4);
                        FPlatformProcess::Sleep(0.01f);
                    }
                }
                else if (!State->Stop.Load()) State->Failure.Store(3);
            }
        }
        State->Buffer.Close();
        if (Stream)
        {
            if (Stream->IsCapturing()) Stream->StopStream();
            if (Stream->IsStreamOpen()) Stream->CloseStream();
        }
        if (State->Opened.Load()) State->Closed.Store(!Stream->IsStreamOpen() && !Stream->IsCapturing());
    }
}
class FRequest final : public IMicrophoneRequest
{
    FString Selected;
    TSharedPtr<FPermission, ESPMode::ThreadSafe> Permission;
    TSharedPtr<FState, ESPMode::ThreadSafe> State;
    bool bClosed = false;
public:
    explicit FRequest(const FString& Device) : Selected(Device) {}
    virtual ~FRequest() override { Close(); }
    virtual FSimurghRTCError Advance(bool& bReady) override
    {
        check(IsInGameThread()); bReady = false;
        if (bClosed || bStopping) return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("Microphone request is closed."));
        if (!Factory) return SimurghRTC::MakeError(ESimurghRTCErrorCode::PlatformUnavailable, TEXT("Unreal's AudioCaptureRtAudio module is unavailable."));
        if (!Permission)
        {
            Permission = MakeShared<FPermission, ESPMode::ThreadSafe>();
            const auto Status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
            if (![[[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"] length]) Permission->Status.Store(3);
            else if (Status == AVAuthorizationStatusAuthorized) Permission->Status.Store(1);
            else if (Status == AVAuthorizationStatusDenied || Status == AVAuthorizationStatusRestricted) Permission->Status.Store(2);
            else
            {
                // The OS callback owns only this atomic result. Cancellation cannot open a late device.
                const auto Result = Permission;
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL Granted) { Result->Status.Store(Granted ? 1 : 2); }];
            }
        }
        if (Permission->Status.Load() == 2) return SimurghRTC::MakeError(ESimurghRTCErrorCode::PermissionDenied, TEXT("Microphone access was denied. Allow it in macOS Privacy & Security settings."));
        if (Permission->Status.Load() == 3) return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("The app must include NSMicrophoneUsageDescription before requesting microphone access."));
        if (Permission->Status.Load() != 1) return {};
        if (!State)
        {
            Reap();
            if (Jobs.Num() >= 4) return SimurghRTC::MakeError(ESimurghRTCErrorCode::ResourceExhausted, TEXT("At most four microphone workers can be open or closing."));
            State = MakeShared<FState, ESPMode::ThreadSafe>();
            auto Shared = State; auto Device = Selected; auto* CurrentFactory = Factory;
            Jobs.Add({Shared, Async(EAsyncExecution::Thread, [Shared, Device, CurrentFactory] { RunCapture(Shared, Device, CurrentFactory); })});
        }
        const auto E = Error(); bReady = !E.IsError() && State->Ready.Load(); return E;
    }
    virtual void SetActive(bool Value) override { if (State) State->Buffer.SetActive(Value); }
    virtual bool Poll(FSimurghRTCAudioFrame& Out) override { return State && State->Buffer.Poll(Out); }
    virtual FSimurghRTCError Error() const override
    {
        const int32 Failure = State ? State->Failure.Load() : 0;
        if (!Failure || bClosed) return {};
        const TCHAR* Message = Failure == 1 ? TEXT("The selected microphone is unavailable.")
            : Failure == 2 ? TEXT("Microphone format is unsupported; use mono/stereo input at 8, 16, 32, 44.1 or 48 kHz.")
            : Failure == 3 ? TEXT("The microphone stream could not start.") : TEXT("Microphone callbacks stopped or the device was removed.");
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::PlatformUnavailable, Message);
    }
    virtual void Close() override
    {
        bClosed = true;
        if (State) { State->Buffer.Close(); State->Stop.Store(true); }
    }
};
}
TSharedPtr<IMicrophoneRequest> RequestPlatformMicrophone(const FString& DeviceId) { return MakeShared<FRequest>(DeviceId); }
FSimurghRTCError EnumerateMicrophones(TArray<FSimurghRTCDeviceInfo>& Devices)
{
    check(IsInGameThread()); Devices.Reset();
    if (!Factory || bStopping) return SimurghRTC::MakeError(ESimurghRTCErrorCode::PlatformUnavailable, TEXT("Microphone capture is unavailable."));
    const auto Status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    auto Stream = Factory->CreateNewAudioCaptureStream(); TArray<Audio::FCaptureDeviceInfo> Inputs;
    if (!Stream || !Stream->GetInputDevicesAvailable(Inputs)) return SimurghRTC::MakeError(ESimurghRTCErrorCode::PlatformUnavailable, TEXT("Microphone enumeration failed."));
    for (const auto& Input : Inputs)
    {
        auto& D = Devices.AddDefaulted_GetRef(); D.Id = DeviceId(Input);
        D.Permission = Status == AVAuthorizationStatusAuthorized ? ESimurghRTCSupport::Available
            : Status == AVAuthorizationStatusNotDetermined ? ESimurghRTCSupport::Unknown : ESimurghRTCSupport::Unavailable;
        D.Label = D.Permission == ESimurghRTCSupport::Available ? Input.DeviceName : TEXT("Microphone");
    }
    return {};
}
FMicrophoneSnapshot GetMicrophoneSnapshot()
{
    check(IsInGameThread()); Reap(); auto Out = Completed; Out.Workers = Jobs.Num();
    for (const auto& Job : Jobs) AddSnapshot(Out, *Job.State);
    return Out;
}
void StartMicrophoneService()
{
    check(IsInGameThread()); bStopping = false;
    if (FModuleManager::Get().LoadModule(TEXT("AudioCaptureRtAudio")))
    {
        auto& Features = IModularFeatures::Get(); const FName Name = Audio::IAudioCaptureFactory::GetModularFeatureName();
        if (Features.IsModularFeatureAvailable(Name)) Factory = &Features.GetModularFeature<Audio::IAudioCaptureFactory>(Name);
    }
    SetMicrophoneCaptureAvailable(Factory != nullptr);
    Reaper = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) { Reap(); return true; }), 0.1f);
}
void StopMicrophoneService()
{
    check(IsInGameThread()); bStopping = true; SetMicrophoneCaptureAvailable(false);
    FTSTicker::GetCoreTicker().RemoveTicker(Reaper); Reaper.Reset();
    for (auto& Job : Jobs) { Job.State->Buffer.Close(); Job.State->Stop.Store(true); }
    for (auto& Job : Jobs) Job.Future.Wait();
    Reap(); Factory = nullptr;
}
}

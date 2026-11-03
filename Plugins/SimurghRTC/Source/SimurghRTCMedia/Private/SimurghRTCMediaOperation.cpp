#include "SimurghRTCMediaOperation.h"
#include "SimurghRTCSession.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/AudioComponent.h"
#include "SimurghRTCMicrophoneInternal.h"
namespace SimurghRTC { FSimurghRTCError BindSessionAudioPlayback(USimurghRTCSession&, FSimurghRTCTrackHandle, UAudioComponent*); }

USimurghRTCMediaOperation* USimurghRTCMediaOperation::CreateSource(USimurghRTCSession* Session, const FSimurghRTCSourceOptions& Options)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCMediaOperation>();
    FSimurghRTCError Error;
    if (uint8(Options.Kind) > uint8(ESimurghRTCSourceKind::GeneratedVideo)
        || (Options.Kind == ESimurghRTCSourceKind::RenderTarget && !IsValid(Options.RenderTarget)))
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("CreateSource"), (Options.Kind == ESimurghRTCSourceKind::Microphone || Options.Kind == ESimurghRTCSourceKind::GeneratedAudio) ? ESimurghRTCFeature::Audio : ESimurghRTCFeature::Video, Error);
    Operation->bCreateSource = true; Operation->SourceKind = Options.Kind; Operation->CaptureDeviceId = Options.DeviceId;
    return Operation;
}

USimurghRTCMediaOperation* USimurghRTCMediaOperation::BindVideo(USimurghRTCSession* Session, FSimurghRTCTrackHandle Track, UTextureRenderTarget2D* Target)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCMediaOperation>();
    FSimurghRTCError Error;
    if (!Track.IsValid() || !IsValid(Target))
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("BindVideo"), ESimurghRTCFeature::Video, Error);
    return Operation;
}

USimurghRTCMediaOperation* USimurghRTCMediaOperation::BindAudio(USimurghRTCSession* Session, FSimurghRTCTrackHandle Track, UAudioComponent* Target)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCMediaOperation>();
    FSimurghRTCError Error;
    if (!Track.IsValid() || !IsValid(Target))
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("BindAudio"), ESimurghRTCFeature::Audio, Error);
    Operation->AudioTrack = Track; Operation->AudioTarget = Target; Operation->bBindAudio = true;
    return Operation;
}

FSimurghRTCError USimurghRTCMediaOperation::ExecuteExtension(USimurghRTCSession& Session, FSimurghRTCOperationResult& Out)
{
    if (bCreateSource)
    {
        if (SourceKind == ESimurghRTCSourceKind::GeneratedAudio) return Session.AddAudioSource({}, Out.Source);
        if (SourceKind != ESimurghRTCSourceKind::Microphone) return Super::ExecuteExtension(Session, Out);
        if (Session.GetProvider() != ESimurghRTCProvider::DirectUnreal)
            return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("Microphone sources are integrated with Direct only."));
        if (!Microphone) Microphone = SimurghRTC::RequestMicrophone(CaptureDeviceId);
        if (!Microphone) return SimurghRTC::MakeError(ESimurghRTCErrorCode::PlatformUnavailable, TEXT("Microphone capture is currently implemented on Mac only."));
        bool bReady = false; const auto Error = Microphone->Advance(bReady);
        bCapturePending = !Error.IsError() && !bReady;
        if (Error.IsError() || !bReady) return Error;
        const auto Added = Session.AddAudioSource(Microphone, Out.Source);
        if (!Added.IsError()) Microphone.Reset(); // The session now owns capture and closes it with its media.
        return Added;
    }
    if (!bBindAudio) return Super::ExecuteExtension(Session, Out);
    const auto Error = SimurghRTC::BindSessionAudioPlayback(Session, AudioTrack, AudioTarget.Get());
    if (!Error.IsError()) Out.Track = AudioTrack;
    AudioTarget.Reset();
    return Error;
}

void USimurghRTCMediaOperation::CleanupExtension()
{
    bCapturePending = false;
    if (Microphone) Microphone->Close();
    Microphone.Reset(); AudioTarget.Reset();
}


FSimurghRTCError USimurghRTCMediaOperation::GetDevices(ESimurghRTCSourceKind Kind, TArray<FSimurghRTCDeviceInfo>& Devices)
{
    check(IsInGameThread());
    Devices.Reset();
    if (Kind != ESimurghRTCSourceKind::Microphone && Kind != ESimurghRTCSourceKind::Camera)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Device discovery accepts microphone or camera."));
    if (Kind == ESimurghRTCSourceKind::Microphone) return SimurghRTC::EnumerateMicrophones(Devices);
    return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("Camera discovery is not implemented yet."));
}

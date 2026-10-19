#include "SimurghRTCMediaOperation.h"
#include "SimurghRTCSession.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/AudioComponent.h"

USimurghRTCMediaOperation* USimurghRTCMediaOperation::CreateSource(USimurghRTCSession* Session, const FSimurghRTCSourceOptions& Options)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCMediaOperation>();
    FSimurghRTCError Error;
    if (uint8(Options.Kind) > uint8(ESimurghRTCSourceKind::GeneratedVideo)
        || (Options.Kind == ESimurghRTCSourceKind::RenderTarget && !IsValid(Options.RenderTarget)))
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("CreateSource"), (Options.Kind == ESimurghRTCSourceKind::Microphone || Options.Kind == ESimurghRTCSourceKind::GeneratedAudio) ? ESimurghRTCFeature::Audio : ESimurghRTCFeature::Video, Error);
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
    return Operation;
}


FSimurghRTCError USimurghRTCMediaOperation::GetDevices(ESimurghRTCSourceKind Kind, TArray<FSimurghRTCDeviceInfo>& Devices)
{
    check(IsInGameThread());
    Devices.Reset();
    if (Kind != ESimurghRTCSourceKind::Microphone && Kind != ESimurghRTCSourceKind::Camera)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Device discovery accepts microphone or camera."));
    return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("Device discovery is not implemented in M0."));
}

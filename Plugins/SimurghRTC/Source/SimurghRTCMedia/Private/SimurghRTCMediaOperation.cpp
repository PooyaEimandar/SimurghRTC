#include "SimurghRTCMediaOperation.h"
#include "SimurghRTCSession.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/AudioComponent.h"

USimurghRTCMediaOperation* USimurghRTCMediaOperation::CreateSource(USimurghRTCSession* Session, const FSimurghRTCSourceOptions& Options)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCMediaOperation>();
    FSimurghRTCError Error;
    if (!StaticEnum<ESimurghRTCSourceKind>()->IsValidEnumValue(static_cast<int64>(Options.Kind)) || (Options.Kind == ESimurghRTCSourceKind::RenderTarget && !IsValid(Options.RenderTarget)))
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("CreateSource"), (Options.Kind == ESimurghRTCSourceKind::Microphone || Options.Kind == ESimurghRTCSourceKind::GeneratedAudio) ? ESimurghRTCFeature::Audio : ESimurghRTCFeature::Video, Error);
    return Operation;
}

USimurghRTCMediaOperation* USimurghRTCMediaOperation::AttachSource(USimurghRTCSession* Session, FSimurghRTCSourceHandle Source)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCMediaOperation>();
    FSimurghRTCError Error;
    if (!Source.IsValid())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("AttachSource"), ESimurghRTCFeature::MediaSources, Error);
    return Operation;
}

USimurghRTCMediaOperation* USimurghRTCMediaOperation::ReplaceSource(USimurghRTCSession* Session, FSimurghRTCTrackHandle Track, FSimurghRTCSourceHandle Source)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCMediaOperation>();
    FSimurghRTCError Error;
    if (!Track.IsValid() || !Source.IsValid())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("ReplaceSource"), ESimurghRTCFeature::MediaSources, Error);
    return Operation;
}

USimurghRTCMediaOperation* USimurghRTCMediaOperation::SetSourceMuted(USimurghRTCSession* Session, FSimurghRTCSourceHandle Source, bool bMuted)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCMediaOperation>();
    FSimurghRTCError Error;
    if (!Source.IsValid())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("SetSourceMuted"), ESimurghRTCFeature::MediaSources, Error);
    return Operation;
}

USimurghRTCMediaOperation* USimurghRTCMediaOperation::StopSource(USimurghRTCSession* Session, FSimurghRTCSourceHandle Source)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCMediaOperation>();
    FSimurghRTCError Error;
    if (!Source.IsValid())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("StopSource"), ESimurghRTCFeature::MediaSources, Error);
    return Operation;
}

USimurghRTCMediaOperation* USimurghRTCMediaOperation::DetachTrack(USimurghRTCSession* Session, FSimurghRTCTrackHandle Track)
{
    check(IsInGameThread());
    auto* Operation = NewObject<USimurghRTCMediaOperation>();
    FSimurghRTCError Error;
    if (!Track.IsValid())
        Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid media source, track or sink."));
    Operation->Initialize(Session, TEXT("DetachTrack"), ESimurghRTCFeature::MediaSources, Error);
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


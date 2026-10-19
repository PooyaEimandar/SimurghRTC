#include "SimurghRTCTypes.h"
#include "Containers/StringConv.h"

bool FSimurghRTCCapability::IsAvailable() const
{
    return Compiled == ESimurghRTCSupport::Available && Platform == ESimurghRTCSupport::Available
        && Service == ESimurghRTCSupport::Available && Negotiated == ESimurghRTCSupport::Available;
}

FSimurghRTCError SimurghRTC::MakeError(ESimurghRTCErrorCode Code, const FString& Message)
{
    FSimurghRTCError Error;
    Error.Code = Code;
    Error.Message = Message;
    return Error;
}

bool SimurghRTC::IsProviderCompiled(ESimurghRTCProvider Provider)
{
    switch (Provider)
    {
    case ESimurghRTCProvider::DirectUnreal: return SIMURGHRTC_WITH_DIRECT != 0;
    case ESimurghRTCProvider::Janus: return SIMURGHRTC_WITH_JANUS != 0;
    default: return false;
    }
}

FSimurghRTCError SimurghRTC::ValidateSessionConfig(const FSimurghRTCSessionConfig& Config)
{
    // UEnum::IsValidEnumValue also accepts Unreal's generated MAX sentinel.
    if (uint8(Config.Provider) > uint8(ESimurghRTCProvider::Janus)
        || uint8(Config.Purpose) > uint8(ESimurghRTCPurpose::Receive))
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid provider or session purpose."));
    if (!IsProviderCompiled(Config.Provider))
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::BuildExcluded, TEXT("The selected provider is excluded by the build profile."));
    if (Config.TimeoutMilliseconds < 1 || Config.TimeoutMilliseconds > 300000)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Timeout must be between 1 and 300000 milliseconds."));
    const bool Secure = Config.Endpoint.StartsWith(TEXT("https://"))
        || (Config.Provider == ESimurghRTCProvider::Janus && Config.Endpoint.StartsWith(TEXT("wss://")));
    const bool Insecure = Config.Endpoint.StartsWith(TEXT("http://"))
        || (Config.Provider == ESimurghRTCProvider::Janus && Config.Endpoint.StartsWith(TEXT("ws://")));
    int32 Separator = Config.Endpoint.Find(TEXT("://"));
    if ((!Secure && !(Insecure && Config.bAllowInsecureDevelopmentSignaling))
        || Separator == INDEX_NONE || Config.Endpoint.Len() <= Separator + 3)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Supply a supported signaling URL; insecure schemes require explicit development opt-in."));
    const FString Remainder = Config.Endpoint.Mid(Separator + 3);
    int32 AuthorityEnd = Remainder.Len();
    for (int32 Index = 0; Index < Remainder.Len(); ++Index)
        if (Remainder[Index] == '/' || Remainder[Index] == '?' || Remainder[Index] == '#') { AuthorityEnd = Index; break; }
    const FString Authority = Remainder.Left(AuthorityEnd);
    bool InvalidCharacter = false;
    for (TCHAR Character : Config.Endpoint)
        InvalidCharacter |= Character == 0 || FChar::IsWhitespace(Character) || Character < 32;
    if (Authority.IsEmpty() || Authority.Contains(TEXT("@")) || Config.Endpoint.Contains(TEXT("#")) || InvalidCharacter)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Signaling URL requires a host and must not contain credentials, fragments or whitespace."));
    return {};
}

FSimurghRTCError SimurghRTC::ValidateChannelOptions(const FSimurghRTCChannelOptions& Options)
{
    const FTCHARToUTF8 Label(*Options.Label, Options.Label.Len());
    bool HasNull = false;
    for (int32 Index = 0; Index < Options.Label.Len(); ++Index)
        HasNull |= Options.Label[Index] == 0;
    if (Label.Length() > 65535 || HasNull)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Channel label exceeds its UTF-8 limit or contains a null character."));
    if (uint8(Options.Reliability) > uint8(ESimurghRTCReliability::MaxLifetime)
        || Options.ReliabilityValue < 0 || Options.ReliabilityValue > 65535
        || (Options.Reliability == ESimurghRTCReliability::Reliable && Options.ReliabilityValue != 0))
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid channel reliability mode or bound."));
    return {};
}

FSimurghRTCError SimurghRTC::ValidateAudioFrame(const FSimurghRTCAudioFrame& Frame)
{
    const bool Rate = Frame.SampleRate == 8000 || Frame.SampleRate == 16000 || Frame.SampleRate == 32000
        || Frame.SampleRate == 44100 || Frame.SampleRate == 48000;
    if (!Rate || Frame.Channels < 1 || Frame.Channels > 2 || Frame.TimestampMicroseconds < 0
        || Frame.Samples.IsEmpty() || Frame.Samples.Num() > Frame.SampleRate / 10 * Frame.Channels
        || Frame.Samples.Num() % Frame.Channels != 0)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("PCM requires a supported rate, 1-2 channels and at most 100 ms of complete frames."));
    for (float Sample : Frame.Samples)
        if (!FMath::IsFinite(Sample) || Sample < -1.0f || Sample > 1.0f)
            return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("PCM samples must be finite and within [-1, 1]."));
    return {};
}

FSimurghRTCError SimurghRTC::ValidateVideoFrame(const FSimurghRTCVideoFrame& Frame)
{
    if (Frame.Width < 1 || Frame.Width > 3840 || Frame.Height < 1 || Frame.Height > 2160
        || Frame.TimestampMicroseconds < 0 || int64(Frame.Width) * Frame.Height * 4 != Frame.Bgra.Num())
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Video requires tightly packed BGRA8, positive dimensions up to 3840 x 2160 and a nonnegative timestamp."));
    return {};
}

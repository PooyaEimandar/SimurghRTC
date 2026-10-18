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
    if (!StaticEnum<ESimurghRTCProvider>()->IsValidEnumValue(static_cast<int64>(Config.Provider))
        || !StaticEnum<ESimurghRTCPurpose>()->IsValidEnumValue(static_cast<int64>(Config.Purpose)))
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
    if (!StaticEnum<ESimurghRTCReliability>()->IsValidEnumValue(static_cast<int64>(Options.Reliability))
        || Options.ReliabilityValue < 0 || Options.ReliabilityValue > 65535
        || (Options.Reliability == ESimurghRTCReliability::Reliable && Options.ReliabilityValue != 0))
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidArgument, TEXT("Invalid channel reliability mode or bound."));
    return {};
}

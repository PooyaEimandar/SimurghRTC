// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCTypes.generated.h"

UENUM(BlueprintType)
enum class ESimurghRTCProvider : uint8
{
    DirectUnreal, Janus
};

UENUM(BlueprintType)
enum class ESimurghRTCPurpose : uint8
{
    Echo, Call, TextRoom, Receive
};

UENUM(BlueprintType)
enum class ESimurghRTCSessionState : uint8
{
    Idle, Connecting, Connected, Closing, Closed, Failed
};

UENUM(BlueprintType)
enum class ESimurghRTCErrorCode : uint8
{
    None, InvalidArgument, InvalidState, BuildExcluded, PlatformUnavailable, ServiceUnsupported, NotNegotiated, Unauthorized, Timeout, Cancelled, NotImplemented, InternalError
};

UENUM(BlueprintType)
enum class ESimurghRTCSupport : uint8
{
    Unknown, Available, Unavailable
};

UENUM(BlueprintType)
enum class ESimurghRTCFeature : uint8
{
    DataChannels, Audio, Video, TextRooms, IncomingCalls, JanusRooms, MediaSources
};

UENUM(BlueprintType)
enum class ESimurghRTCReliability : uint8
{
    Reliable, MaxRetransmits, MaxLifetime
};

UENUM(BlueprintType)
enum class ESimurghRTCMessageKind : uint8
{
    Text, Binary
};

UENUM(BlueprintType)
enum class ESimurghRTCTrackKind : uint8
{
    Audio, Video
};

/** Plugin-owned identity; never a provider handle or wire-protocol ID. */
USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCChannelHandle
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FGuid Id;
    bool IsValid() const { return Id.IsValid(); }
};

/** Plugin-owned identity; never a provider handle or wire-protocol ID. */
USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCSourceHandle
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FGuid Id;
    bool IsValid() const { return Id.IsValid(); }
};

/** Plugin-owned identity; never a provider handle or wire-protocol ID. */
USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCTrackHandle
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FGuid Id;
    bool IsValid() const { return Id.IsValid(); }
};

USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCError
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") ESimurghRTCErrorCode Code = ESimurghRTCErrorCode::None;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FString Message;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FGuid SessionId;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FGuid OperationId;
    bool IsError() const { return Code != ESimurghRTCErrorCode::None; }
};

USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCIceServer
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC") TArray<FString> Urls;
    UPROPERTY(Transient, BlueprintReadWrite, Category="SimurghRTC") FString Username;
    UPROPERTY(Transient, BlueprintReadWrite, Category="SimurghRTC") FString Credential;
};

USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCSessionConfig
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC") ESimurghRTCProvider Provider = ESimurghRTCProvider::DirectUnreal;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC") ESimurghRTCPurpose Purpose = ESimurghRTCPurpose::Echo;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC") FString Endpoint;
    // Opaque string preserves identity precision; authentication is separate.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC") FString LocalIdentity;
    UPROPERTY(Transient, BlueprintReadWrite, Category="SimurghRTC") FString AccessToken;
    UPROPERTY(Transient, BlueprintReadWrite, Category="SimurghRTC") TArray<FSimurghRTCIceServer> IceServers;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC", meta=(ClampMin="1", ClampMax="300000")) int32 TimeoutMilliseconds = 30000;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC") bool bAllowInsecureDevelopmentSignaling = false;
};

USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCChannelOptions
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC") FString Label;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC") bool bOrdered = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC") ESimurghRTCReliability Reliability = ESimurghRTCReliability::Reliable;
    // Retransmission count or milliseconds, depending on Reliability. Must be zero for Reliable.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC", meta=(ClampMin="0", ClampMax="65535")) int32 ReliabilityValue = 0;
};

USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCCapability
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") ESimurghRTCSupport Compiled = ESimurghRTCSupport::Unknown;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") ESimurghRTCSupport Platform = ESimurghRTCSupport::Unknown;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") ESimurghRTCSupport Service = ESimurghRTCSupport::Unknown;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") ESimurghRTCSupport Negotiated = ESimurghRTCSupport::Unknown;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FSimurghRTCError Reason;
    bool IsAvailable() const;
};

USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCOperationResult
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FSimurghRTCError Error;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FSimurghRTCChannelHandle Channel;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FSimurghRTCSourceHandle Source;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FSimurghRTCTrackHandle Track;
    bool IsSuccess() const { return !Error.IsError(); }
};

USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCChannelInfo
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FSimurghRTCChannelHandle Handle;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FSimurghRTCChannelOptions Options;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") bool bOpen = false;
    // Zero means unknown/unavailable, not unlimited.
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") int64 MaximumMessageBytes = 0;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") int64 BufferedBytes = 0;
};

USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCDataMessage
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FSimurghRTCChannelHandle Channel;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") ESimurghRTCMessageKind Kind = ESimurghRTCMessageKind::Text;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FString Text;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") TArray<uint8> Bytes;
};

USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCTrackInfo
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FSimurghRTCTrackHandle Handle;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") ESimurghRTCTrackKind Kind = ESimurghRTCTrackKind::Audio;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FString ParticipantId;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") bool bMuted = false;
};

USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCParticipant
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FString Id;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FString DisplayName;
};

USTRUCT(BlueprintType)
struct SIMURGHRTCCORE_API FSimurghRTCStatistics
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") bool bAvailable = false;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") int64 BytesSent = 0;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") int64 BytesReceived = 0;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") double RoundTripMilliseconds = 0;
};

namespace SimurghRTC
{
    SIMURGHRTCCORE_API FSimurghRTCError MakeError(ESimurghRTCErrorCode Code, const FString& Message);
    SIMURGHRTCCORE_API FSimurghRTCError ValidateSessionConfig(const FSimurghRTCSessionConfig& Config);
    SIMURGHRTCCORE_API FSimurghRTCError ValidateChannelOptions(const FSimurghRTCChannelOptions& Options);
    SIMURGHRTCCORE_API bool IsProviderCompiled(ESimurghRTCProvider Provider);
}

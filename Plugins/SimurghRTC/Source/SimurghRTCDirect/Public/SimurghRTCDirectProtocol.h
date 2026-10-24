// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include <initializer_list>
#include "SimurghRTCNativeSignaling.h"

namespace SimurghRTC::Direct
{
inline constexpr int32 Version = 1;
inline constexpr int32 MaxMessageBytes = 256 * 1024;
inline constexpr int32 MaxSdpBytes = 128 * 1024;
inline constexpr int32 MaxIce = 32;
inline constexpr int32 MaxEvents = 64;
inline constexpr int32 MaxReplay = 128;
inline constexpr int32 MaxReplayBytes = 2 * 1024 * 1024;
inline constexpr const TCHAR* Path = TEXT("/simurghrtc/v1/signal");

struct FRequest
{
    FString Type, Id, Session, Digest;
    int32 Sequence = 0;
    TSharedPtr<FJsonObject> Body;
};
SIMURGHRTCDIRECT_API bool ParseJson(const TArray<uint8>& Bytes, TSharedPtr<FJsonObject>& Out, FString& Error);
SIMURGHRTCDIRECT_API bool ParseRequest(const TArray<uint8>& Bytes, FRequest& Out, FString& Error);
SIMURGHRTCDIRECT_API bool Fields(const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> Names);
SIMURGHRTCDIRECT_API bool Integer(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, int32& Out, int32 Max = MAX_int32);
SIMURGHRTCDIRECT_API bool Text(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, FString& Out, int32 Max, bool bEmpty = false);
SIMURGHRTCDIRECT_API bool SessionId(const FString& Value);
SIMURGHRTCDIRECT_API bool Sdp(const FString& Value);
SIMURGHRTCDIRECT_API bool Ice(const TSharedPtr<FJsonObject>& Body, FNativeSignalRequest& Out);
SIMURGHRTCDIRECT_API FString Write(const TSharedRef<FJsonObject>& Object);
SIMURGHRTCDIRECT_API TArray<uint8> Bytes(const FString& Value);
SIMURGHRTCDIRECT_API TSharedRef<FJsonObject> Envelope(const FRequest& Request, const FString& Session, const TCHAR* Status, TSharedRef<FJsonObject> Body);
SIMURGHRTCDIRECT_API TSharedRef<FJsonObject> IceBody(const FNativeIceCandidate& Ice);
SIMURGHRTCDIRECT_API int32 HttpStatus(const FString& Code);
}

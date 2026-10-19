// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "SimurghRTCOperation.h"
#include "SimurghRTCMediaOperation.generated.h"
class UTextureRenderTarget2D;
class UAudioComponent;

UENUM(BlueprintType)
enum class ESimurghRTCSourceKind : uint8 { Microphone, Camera, RenderTarget, Screen, GeneratedAudio, GeneratedVideo };

USTRUCT(BlueprintType)
struct SIMURGHRTCMEDIA_API FSimurghRTCSourceOptions
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC") ESimurghRTCSourceKind Kind = ESimurghRTCSourceKind::Microphone;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimurghRTC") FString DeviceId;
    UPROPERTY(Transient, BlueprintReadWrite, Category="SimurghRTC") TObjectPtr<UTextureRenderTarget2D> RenderTarget = nullptr;
};

USTRUCT(BlueprintType)
struct SIMURGHRTCMEDIA_API FSimurghRTCDeviceInfo
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FString Id;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") FString Label;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") ESimurghRTCSourceKind Kind = ESimurghRTCSourceKind::Microphone;
    UPROPERTY(BlueprintReadOnly, Category="SimurghRTC") ESimurghRTCSupport Permission = ESimurghRTCSupport::Unknown;
};

UCLASS(BlueprintType)
class SIMURGHRTCMEDIA_API USimurghRTCMediaOperation : public USimurghRTCOperation
{
    GENERATED_BODY()
public:
    // Snapshot only; discovery never prompts for permission. CreateSource requests access.
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Media")
    static FSimurghRTCError GetDevices(ESimurghRTCSourceKind Kind, TArray<FSimurghRTCDeviceInfo>& Devices);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Media", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCMediaOperation* CreateSource(USimurghRTCSession* Session, const FSimurghRTCSourceOptions& Options);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Media", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCMediaOperation* BindVideo(USimurghRTCSession* Session, FSimurghRTCTrackHandle Track, UTextureRenderTarget2D* Target);
    UFUNCTION(BlueprintCallable, Category="SimurghRTC|Media", meta=(BlueprintInternalUseOnly="true"))
    static USimurghRTCMediaOperation* BindAudio(USimurghRTCSession* Session, FSimurghRTCTrackHandle Track, UAudioComponent* Target);
};

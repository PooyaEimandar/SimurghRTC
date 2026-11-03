// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SimurghRTCTypes.h"
namespace SimurghRTC
{
// Optional client capture adapter. All entry points run on the game thread.
class ISessionAudioSource
{
public:
    virtual ~ISessionAudioSource() = default;
    virtual void SetActive(bool bActive) = 0;
    virtual bool Poll(FSimurghRTCAudioFrame& Out) = 0;
    virtual FSimurghRTCError Error() const = 0;
    virtual void Close() = 0;
};
SIMURGHRTCCORE_API void SetMicrophoneCaptureAvailable(bool bAvailable);
SIMURGHRTCCORE_API bool IsMicrophoneCaptureAvailable();
}

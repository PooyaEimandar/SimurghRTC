// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SimurghRTCTypes.h"

namespace SimurghRTC
{
// Internal client adapter. All methods run on the game thread; Core owns no audio device.
class ISessionAudioSink
{
public:
    virtual ~ISessionAudioSink() = default;
    virtual bool IsAlive() const = 0;
    virtual void Push(const FSimurghRTCAudioFrame& Frame) = 0;
    virtual void Close() = 0;
};
}

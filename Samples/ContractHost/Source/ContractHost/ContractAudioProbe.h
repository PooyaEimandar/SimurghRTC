// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
class FContractAudioProbe
{
public:
    FContractAudioProbe();
    ~FContractAudioProbe();
    void Startup();
    void Shutdown();
private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};

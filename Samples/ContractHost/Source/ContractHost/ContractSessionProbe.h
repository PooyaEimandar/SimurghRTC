// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
class FContractSessionProbe
{
public:
    FContractSessionProbe();
    ~FContractSessionProbe();
    void Startup();
    void Shutdown();
private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
    FDelegateHandle Init, Exit;
};

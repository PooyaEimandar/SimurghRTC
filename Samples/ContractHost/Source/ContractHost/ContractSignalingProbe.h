// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"

/** Explicit, non-Shipping local connection probe, shared by clients and dedicated Server. */
class FContractSignalingProbe
{
public:
    FContractSignalingProbe();
    ~FContractSignalingProbe();
    void Startup();
    void Shutdown();
private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};

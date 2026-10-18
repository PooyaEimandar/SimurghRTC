// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "Containers/Ticker.h"

/** Opt-in sample probe. Never runs merely because the plugin is loaded. */
class FContractHostClientProbe
{
public:
    void Startup();
    void Shutdown();
private:
    bool Run(float);
    FDelegateHandle InitHandle;
    FTSTicker::FDelegateHandle TickHandle;
};

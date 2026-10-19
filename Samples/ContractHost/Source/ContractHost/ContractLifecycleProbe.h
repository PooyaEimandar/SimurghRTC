// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/StrongObjectPtr.h"
#include "ContractLifecycleExample.h"
#include "ContractExtensionExample.h"

class FContractLifecycleProbe
{
public:
    void Startup();
    void Shutdown();
private:
    bool Tick(float);
    void Finish(bool bPassed, const FString& Error);
    FDelegateHandle InitHandle;
    FDelegateHandle ExitHandle;
    FTSTicker::FDelegateHandle TickHandle;
    TStrongObjectPtr<AContractLifecycleExample> Native;
    TStrongObjectPtr<AContractLifecycleExample> Blueprint;
    TStrongObjectPtr<AContractExtensionExample> Extension;
    bool bProfileChecked = false;
    double Started = 0;
    int32 QuietTicks = 0;
    bool bEndPlayChecked = false;
};

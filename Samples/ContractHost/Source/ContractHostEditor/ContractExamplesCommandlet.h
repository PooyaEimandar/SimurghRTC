// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Commandlets/Commandlet.h"
#include "ContractExamplesCommandlet.generated.h"

/** Rebuild the inspectable, portable Blueprint example from public nodes. */
UCLASS()
class UContractExamplesCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UContractExamplesCommandlet();
    virtual int32 Main(const FString& Params) override;
};

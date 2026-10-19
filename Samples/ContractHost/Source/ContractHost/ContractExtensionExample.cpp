// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractExtensionExample.h"

void AContractExtensionExample::RecordExtensionResult(const FSimurghRTCOperationResult& Result, bool bFailureBranch)
{
    ++CompletionCount;
    bExpectedFailure = bFailureBranch && Result.Error.Code == ESimurghRTCErrorCode::InvalidArgument
        && Result.Error.OperationId.IsValid() && !Result.Error.SessionId.IsValid()
        && !Result.Channel.IsValid() && !Result.Source.IsValid() && !Result.Track.IsValid();
}

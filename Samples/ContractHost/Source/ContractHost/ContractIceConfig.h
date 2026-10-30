// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SimurghRTCTypes.h"

// Optional private runtime file for packaged probes. Never contains signaling payloads.
bool LoadContractIceConfig(TArray<FSimurghRTCIceServer>& Servers, bool& bRelayOnly);

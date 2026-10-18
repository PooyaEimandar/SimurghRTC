// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "Modules/ModuleManager.h"

// Factory instances are explicitly owned by callers; loading this module starts no threads or sockets.
IMPLEMENT_MODULE(FDefaultModuleImpl, SimurghRTCNative)

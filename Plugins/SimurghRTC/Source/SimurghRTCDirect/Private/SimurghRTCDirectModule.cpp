// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"

namespace SimurghRTC { void ShutdownDirectClients(); }
class FSimurghRTCDirectModule final : public IModuleInterface
{
    FDelegateHandle Exit;
public:
    bool SupportsDynamicReloading() override { return false; }
    void StartupModule() override
    { Exit = FCoreDelegates::OnEnginePreExit.AddStatic(&SimurghRTC::ShutdownDirectClients); }
    void ShutdownModule() override
    { FCoreDelegates::OnEnginePreExit.Remove(Exit); SimurghRTC::ShutdownDirectClients(); }
};
IMPLEMENT_MODULE(FSimurghRTCDirectModule, SimurghRTCDirect)

// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"
#include "SimurghRTCSessionTransport.h"

namespace SimurghRTC { void ShutdownDirectClients(); void RegisterDirectSessionTransport(); }
class FSimurghRTCDirectModule final : public IModuleInterface
{
    FDelegateHandle Exit;
public:
    bool SupportsDynamicReloading() override { return false; }
    void StartupModule() override
    { SimurghRTC::RegisterDirectSessionTransport(); Exit = FCoreDelegates::OnEnginePreExit.AddStatic(&SimurghRTC::ShutdownDirectClients); }
    void ShutdownModule() override
    {
        FCoreDelegates::OnEnginePreExit.Remove(Exit); SimurghRTC::ShutdownSharedSessions();
        SimurghRTC::ShutdownSessionTransports(); SimurghRTC::ShutdownDirectClients();
    }
};
IMPLEMENT_MODULE(FSimurghRTCDirectModule, SimurghRTCDirect)

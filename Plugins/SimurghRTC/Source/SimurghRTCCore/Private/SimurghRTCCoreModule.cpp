// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "Modules/ModuleManager.h"
#include "SimurghRTCSessionTransport.h"
#include "Misc/CoreDelegates.h"

class FSimurghRTCCoreModule final : public IModuleInterface
{
    FDelegateHandle Exit;
    static void Stop() { SimurghRTC::ShutdownSharedSessions(); SimurghRTC::ShutdownSessionTransports(); }
public:
    bool SupportsDynamicReloading() override { return false; }
    void StartupModule() override { Exit = FCoreDelegates::OnEnginePreExit.AddStatic(&Stop); }
    void ShutdownModule() override { FCoreDelegates::OnEnginePreExit.Remove(Exit); Stop(); }
};
IMPLEMENT_MODULE(FSimurghRTCCoreModule, SimurghRTCCore)

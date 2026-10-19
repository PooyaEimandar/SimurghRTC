#include "Modules/ModuleManager.h"
#include "ContractHostClientProbe.h"
#include "ContractLifecycleProbe.h"

class FContractHostModule final : public FDefaultGameModuleImpl
{
    FContractHostClientProbe Probe;
    FContractLifecycleProbe Lifecycle;
public:
    void StartupModule() override { Probe.Startup(); Lifecycle.Startup(); }
    void ShutdownModule() override { Lifecycle.Shutdown(); Probe.Shutdown(); }
};
IMPLEMENT_PRIMARY_GAME_MODULE(FContractHostModule, ContractHost, "ContractHost");

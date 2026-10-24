#include "Modules/ModuleManager.h"
#include "ContractHostClientProbe.h"
#include "ContractLifecycleProbe.h"
#include "ContractSignalingProbe.h"

class FContractHostModule final : public FDefaultGameModuleImpl
{
    FContractHostClientProbe Probe;
    FContractLifecycleProbe Lifecycle;
    FContractSignalingProbe Signaling;
public:
    void StartupModule() override { Probe.Startup(); Lifecycle.Startup(); Signaling.Startup(); }
    void ShutdownModule() override { Signaling.Shutdown(); Lifecycle.Shutdown(); Probe.Shutdown(); }
};
IMPLEMENT_PRIMARY_GAME_MODULE(FContractHostModule, ContractHost, "ContractHost");

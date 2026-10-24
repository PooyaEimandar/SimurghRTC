#include "Modules/ModuleManager.h"
#include "ContractHostClientProbe.h"
#include "ContractLifecycleProbe.h"
#include "ContractSignalingProbe.h"
#include "ContractDirectProbe.h"

class FContractHostModule final : public FDefaultGameModuleImpl
{
    FContractHostClientProbe Probe;
    FContractLifecycleProbe Lifecycle;
    FContractSignalingProbe Signaling;
    FContractDirectProbe Direct;
public:
    void StartupModule() override { Probe.Startup(); Lifecycle.Startup(); Signaling.Startup(); Direct.Startup(); }
    void ShutdownModule() override { Direct.Shutdown(); Signaling.Shutdown(); Lifecycle.Shutdown(); Probe.Shutdown(); }
};
IMPLEMENT_PRIMARY_GAME_MODULE(FContractHostModule, ContractHost, "ContractHost");

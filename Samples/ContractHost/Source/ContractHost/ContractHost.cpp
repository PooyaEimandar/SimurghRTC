#include "Modules/ModuleManager.h"
#include "ContractHostClientProbe.h"
#include "ContractLifecycleProbe.h"
#include "ContractSignalingProbe.h"
#include "ContractDirectProbe.h"
#include "ContractSessionProbe.h"
#include "ContractDataExampleProbe.h"

class FContractHostModule final : public FDefaultGameModuleImpl
{
    FContractHostClientProbe Probe;
    FContractLifecycleProbe Lifecycle;
    FContractSignalingProbe Signaling;
    FContractDirectProbe Direct;
    FContractSessionProbe Session;
    FContractDataExampleProbe DataExample;
public:
    void StartupModule() override { Probe.Startup(); Lifecycle.Startup(); Signaling.Startup(); Direct.Startup(); Session.Startup(); DataExample.Startup(); }
    void ShutdownModule() override { DataExample.Shutdown(); Session.Shutdown(); Direct.Shutdown(); Signaling.Shutdown(); Lifecycle.Shutdown(); Probe.Shutdown(); }
};
IMPLEMENT_PRIMARY_GAME_MODULE(FContractHostModule, ContractHost, "ContractHost");

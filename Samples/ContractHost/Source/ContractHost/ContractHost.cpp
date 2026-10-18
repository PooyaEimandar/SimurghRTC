#include "Modules/ModuleManager.h"
#include "ContractHostClientProbe.h"

class FContractHostModule final : public FDefaultGameModuleImpl
{
    FContractHostClientProbe Probe;
public:
    void StartupModule() override { Probe.Startup(); }
    void ShutdownModule() override { Probe.Shutdown(); }
};
IMPLEMENT_PRIMARY_GAME_MODULE(FContractHostModule, ContractHost, "ContractHost");

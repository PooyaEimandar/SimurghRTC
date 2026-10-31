#include "Modules/ModuleManager.h"
#include "ContractHostClientProbe.h"
#include "ContractLifecycleProbe.h"
#include "ContractSignalingProbe.h"
#include "ContractDirectProbe.h"
#include "ContractSessionProbe.h"
#include "ContractDataExampleProbe.h"
#include "ContractAudioProbe.h"

class FContractHostModule final : public FDefaultGameModuleImpl
{
    FContractHostClientProbe Probe;
    FContractLifecycleProbe Lifecycle;
    FContractSignalingProbe Signaling;
    FContractDirectProbe Direct;
    FContractSessionProbe Session;
    FContractDataExampleProbe DataExample;
    FContractAudioProbe Audio;
public:
    void StartupModule() override { Probe.Startup(); Lifecycle.Startup(); Signaling.Startup(); Direct.Startup(); Session.Startup(); DataExample.Startup(); Audio.Startup(); }
    void ShutdownModule() override { Audio.Shutdown(); DataExample.Shutdown(); Session.Shutdown(); Direct.Shutdown(); Signaling.Shutdown(); Lifecycle.Shutdown(); Probe.Shutdown(); }
};
IMPLEMENT_PRIMARY_GAME_MODULE(FContractHostModule, ContractHost, "ContractHost");

// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "SimurghRTCNativePeerService.h"

class FSimurghRTCNativeModule final : public IModuleInterface
{
public:
    // Loading starts no native threads or sockets; the first accepted peer starts the worker.
    void StartupModule() override
    {
        Service = MakeShared<SimurghRTC::FNativePeerService, ESPMode::ThreadSafe>();
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
        { Service->Tick(); return true; }));
        WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda([this](UWorld* World, bool, bool)
        { Service->CloseWorld(World); });
    }

    void ShutdownModule() override
    {
        FTSTicker::RemoveTicker(TickHandle);
        FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
        Service->Shutdown();
        Service.Reset();
    }

    TSharedPtr<SimurghRTC::FNativePeerService, ESPMode::ThreadSafe> Service;
private:
    FTSTicker::FDelegateHandle TickHandle;
    FDelegateHandle WorldCleanupHandle;
};

SimurghRTC::FNativePeer::FHandle SimurghRTC::FNativePeer::Open(UObject* LifetimeOwner, ENativeFactoryMode Mode,
    FNativePeerOpened OnOpened, FString& OutError, bool bLoopbackProbe, const FNativeIceConfig& Ice)
{
    check(IsInGameThread());
    auto& Module = FModuleManager::LoadModuleChecked<FSimurghRTCNativeModule>(TEXT("SimurghRTCNative"));
    return Module.Service->Open(LifetimeOwner, Mode, MoveTemp(OnOpened), OutError, bLoopbackProbe, Ice);
}

IMPLEMENT_MODULE(FSimurghRTCNativeModule, SimurghRTCNative)

// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCSessionTransport.h"
#include "Containers/Ticker.h"
#include "Modules/ModuleManager.h"

namespace SimurghRTC
{
namespace
{
FSessionTransportFactory Factories[2];
TArray<TSharedPtr<ISessionTransport>> Retiring;
FTSTicker::FDelegateHandle Cleanup;
FSessionRuntimeSnapshot Counts;
bool bStopping = false;
void LoadProvider(ESimurghRTCProvider Provider)
{
    if (!bStopping && IsProviderCompiled(Provider))
        FModuleManager::Get().LoadModule(Provider == ESimurghRTCProvider::DirectUnreal ? TEXT("SimurghRTCDirect") : TEXT("SimurghRTCJanus"));
}
}
FSessionTransportFactory SetSessionTransportFactory(ESimurghRTCProvider Provider, FSessionTransportFactory Factory)
{
    check(IsInGameThread()); check(uint8(Provider) < 2);
    auto Previous = MoveTemp(Factories[uint8(Provider)]); Factories[uint8(Provider)] = MoveTemp(Factory); return Previous;
}
bool HasSessionTransport(ESimurghRTCProvider Provider)
{
    check(IsInGameThread()); LoadProvider(Provider);
    return !bStopping && uint8(Provider) < 2 && bool(Factories[uint8(Provider)]);
}
TSharedPtr<ISessionTransport> CreateSessionTransport(UObject* Owner, const FSimurghRTCSessionConfig& Config, FSimurghRTCError& Error)
{
    check(IsInGameThread()); Error = {};
    if (!HasSessionTransport(Config.Provider))
    { Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("The selected session provider is not implemented.")); return {}; }
    const auto Factory = Factories[uint8(Config.Provider)]; auto Result = Factory(Owner,Config,Error);
    if (Result) ++Counts.Created;
    else if (!Error.IsError()) Error = SimurghRTC::MakeError(ESimurghRTCErrorCode::InternalError,TEXT("The session provider could not start."));
    return Result;
}
void RetireSessionTransport(TSharedPtr<ISessionTransport> Transport)
{
    check(IsInGameThread()); if (!Transport) return;
    Transport->SetHandler({}); Transport->Close();
    Retiring.Add(MoveTemp(Transport));
    if (!Cleanup.IsValid()) Cleanup = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
    {
        for (int32 I = Retiring.Num()-1; I >= 0; --I)
        {
            const auto S = Retiring[I]->Snapshot();
            if (S.bNativeClosed && (S.State == ESimurghRTCSessionState::Closed || S.State == ESimurghRTCSessionState::Failed))
            { ++Counts.NativeClosed; if (S.bRemoteClosed) ++Counts.RemoteClosed; Retiring.RemoveAt(I); }
        }
        if (!Retiring.IsEmpty()) return true;
        Cleanup.Reset(); return false;
    }));
}
FSessionRuntimeSnapshot GetSessionRuntimeSnapshot()
{ check(IsInGameThread()); auto Result = Counts; Result.PendingCleanup = Retiring.Num(); return Result; }
void ShutdownSessionTransports()
{
    check(IsInGameThread()); bStopping = true;
    FTSTicker::GetCoreTicker().RemoveTicker(Cleanup); Cleanup.Reset();
    for (const auto& T : Retiring) { T->SetHandler({}); T->Close(); }
    Retiring.Reset(); Factories[0] = {}; Factories[1] = {};
}
}

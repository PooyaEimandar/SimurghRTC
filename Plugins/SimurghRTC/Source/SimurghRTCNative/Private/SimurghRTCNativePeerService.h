// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "SimurghRTCNativePeer.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "SimurghRTCNativeDataService.h"

class FEvent;
class UWorld;

namespace SimurghRTC
{
struct FNativePeerSharedState
{
    FCriticalSection Mutex;
    FNativePeerSnapshot Snapshot;
    uint64 Id = 0;
    TArray<FNativeTransportEvent> Events;
    TSharedRef<FNativeDataShared, ESPMode::ThreadSafe> Data = MakeShared<FNativeDataShared, ESPMode::ThreadSafe>();
};

/** Game-thread service; only Run and its native runtime map execute on the worker. */
class FNativePeerService final : public FRunnable, public TSharedFromThis<FNativePeerService, ESPMode::ThreadSafe>
{
public:
    static constexpr int32 MaxPeers = 64;

    FNativePeerService();
    ~FNativePeerService();
    FNativePeer::FHandle Open(UObject* Owner, ENativeFactoryMode Mode, FNativePeerOpened OnOpened, FString& OutError, bool bLoopbackProbe = false);
    void Close(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State);
    bool Signal(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State,
        FNativeSignalRequest Request, FNativeSignalCompleted OnComplete, FString& OutError);
    void SetEventHandler(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State, FNativeTransportChanged OnEvent);
    FNativeDataGame* Data(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State);
    void SubmitData(const TSharedRef<FNativePeerSharedState, ESPMode::ThreadSafe>& State, FNativeDataCommand Command);
    void Tick();
    void CloseWorld(UWorld* World);
    // Only module shutdown/tests wait for native teardown. Normal Close never waits.
    void Shutdown();
    int32 GetPendingPeerCount() const;

#if WITH_DEV_AUTOMATION_TESTS
    // Deterministic tests can hold the worker while exercising game-thread cancellation.
    bool HoldWorkerForTesting(FEvent& Entered, FEvent& Release);
#endif

private:
    struct FEntry
    {
        TSharedPtr<FNativePeerSharedState, ESPMode::ThreadSafe> State;
        TWeakPtr<FNativePeer, ESPMode::ThreadSafe> Peer;
        TWeakObjectPtr<UObject> Owner;
        TWeakObjectPtr<UWorld> World;
        FNativePeerOpened OnOpened;
        TMap<uint64, FNativeSignalCompleted> Operations;
        TSharedPtr<FNativeTransportChanged> OnEvent;
        uint64 NextOperation = 1;
        uint64 PendingSdp = 0;
        TUniquePtr<FNativeDataGame> Data;
    };
    enum class ECommand : uint8 { Open, Close, Shutdown, Signal, Data
#if WITH_DEV_AUTOMATION_TESTS
        , TestGate
#endif
    };
    struct FCommand
    {
        ECommand Type = ECommand::Shutdown;
        TSharedPtr<FNativePeerSharedState, ESPMode::ThreadSafe> State;
        ENativeFactoryMode Mode = ENativeFactoryMode::DataOnly;
        bool bLoopbackProbe = false;
        FNativeSignalRequest Request;
        uint64 OperationId = 0;
        double Deadline = 0;
        FNativeDataCommand Data;
#if WITH_DEV_AUTOMATION_TESTS
        FEvent* Entered = nullptr;
        FEvent* Release = nullptr;
#endif
    };

    bool StartWorker(FString& OutError);
    void Enqueue(FCommand Command);
    uint32 Run() override;

    FEvent* Wake = nullptr;
    TUniquePtr<FRunnableThread> Thread;
    TQueue<FCommand, EQueueMode::Spsc> Commands;
    TArray<TSharedRef<FEntry>> Entries;
    uint64 NextId = 1;
    bool bShuttingDown = false;
};
}

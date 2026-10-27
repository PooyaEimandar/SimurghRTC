// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractSessionEndpoint.h"
#include "Engine/World.h"
void UContractSessionEndpoint::Initialize(UObject* Owner)
{
    LifetimeOwner = Owner; Session = USimurghRTCSession::CreateSession(Owner); FacadeId = Session->GetSessionId();
    Session->OnStateChanged.AddDynamic(this,&UContractSessionEndpoint::StateChanged);
    Session->OnChannelChanged.AddDynamic(this,&UContractSessionEndpoint::ChannelChanged);
    Session->OnMessage.AddDynamic(this,&UContractSessionEndpoint::Message);
}
void UContractSessionEndpoint::Start(USimurghRTCOperation* Op)
{
    Operation = Op; ++Operations;
    TWeakObjectPtr<UContractSessionEndpoint> Weak(this);
    Op->OnCompletedNative.AddLambda([Weak](const FSimurghRTCOperationResult& R)
    {
        if (auto* Self = Weak.Get())
        {
            Self->bGameThread &= IsInGameThread(); ++Self->Completions;
            if (Self->bQuiet || R.Error.SessionId != Self->FacadeId || !R.Error.OperationId.IsValid()
                || Self->OperationIds.Contains(R.Error.OperationId)) { ++Self->Unexpected; Self->Fail(TEXT("Unexpected completion/correlation.")); }
            Self->OperationIds.Add(R.Error.OperationId);
            if (Self->bShutdownOnCompletion && R.IsSuccess()) { Self->bQuiet = true; Self->Session->Shutdown(); Self->bTriggered = true; }
        }
    });
    Op->Activate();
}
void UContractSessionEndpoint::StateChanged(ESimurghRTCSessionState)
{
    bGameThread &= IsInGameThread(); ++StateEvents;
    if (bQuiet) { ++Unexpected; Fail(TEXT("State callback after teardown.")); }
}
void UContractSessionEndpoint::ChannelChanged(const FSimurghRTCChannelInfo&)
{
    bGameThread &= IsInGameThread(); ++ChannelEvents;
    if (bQuiet) { ++Unexpected; Fail(TEXT("Channel callback after teardown.")); return; }
    if (!Trigger.IsEmpty() && !bTriggered)
    {
        bTriggered = bQuiet = true;
        if (Trigger == TEXT("owner-loss")) { if (LifetimeOwner.IsValid()) LifetimeOwner->MarkAsGarbage(); }
        else if (Trigger == TEXT("world"))
        { if (auto* World = Cast<UWorld>(LifetimeOwner.Get())) World->DestroyWorld(false); }
    }
}
void UContractSessionEndpoint::Message(const FSimurghRTCDataMessage& Data)
{
    bGameThread &= IsInGameThread();
    if (bQuiet || Expected.IsEmpty()) { ++Unexpected; Fail(TEXT("Unexpected message after completion/teardown.")); return; }
    const auto& Want = Expected[0];
    if (Data.Channel.Id != Channel.Id || Data.Kind != Want.Kind || Data.Bytes != Want.Bytes || Data.Text.Len() != Want.Text.Len()
        || FMemory::Memcmp(*Data.Text,*Want.Text,Data.Text.Len()*sizeof(TCHAR)) != 0)
    { ++Unexpected; Fail(TEXT("Message leaked across sessions or changed in transit.")); return; }
    Expected.RemoveAt(0); ++Received;
}
void UContractSessionEndpoint::Send(FSimurghRTCDataMessage Data)
{
    const auto Result = Data.Kind == ESimurghRTCMessageKind::Text ? Session->SendText(Channel,Data.Text) : Session->SendBytes(Channel,Data.Bytes);
    if (Result.IsError() || Result.SessionId != FacadeId || Result.OperationId.IsValid())
    {
        FSimurghRTCChannelInfo Info; const auto Query = Session->GetChannelInfo(Channel,Info);
        if (Error.IsEmpty()) Error = FString::Printf(TEXT("Shared send failed: code=%d state=%d query=%d open=%d limit=%lld buffered=%lld correlation=%d; %s"),
            int32(Result.Code),int32(Session->GetState()),int32(Query.Code),Info.bOpen,static_cast<long long>(Info.MaximumMessageBytes),
            static_cast<long long>(Info.BufferedBytes),Result.SessionId == FacadeId && !Result.OperationId.IsValid(),*Result.Message);
        return;
    }
    Expected.Add(Data); Data.Text = TEXT("changed after send"); for (auto& Byte : Data.Bytes) Byte ^= 255;
}
void UContractSessionEndpoint::SendPair(int32 Role, int32 Index)
{
    FSimurghRTCDataMessage Text; Text.Kind = ESimurghRTCMessageKind::Text;
    Text.Text = FString::Printf(TEXT("sr105|%d|%d|%s|سلام 🌍"),Role,Index,*FacadeId.ToString());
    Text.Text.AppendChar(0); Text.Text += TEXT("end"); Send(Text);
    FSimurghRTCDataMessage Bytes; Bytes.Kind = ESimurghRTCMessageKind::Binary; Bytes.Bytes.SetNumUninitialized(257);
    for (int32 I = 0; I < Bytes.Bytes.Num(); ++I) Bytes.Bytes[I] = uint8(I*17 + Role*31 + Index);
    Send(Bytes);
}

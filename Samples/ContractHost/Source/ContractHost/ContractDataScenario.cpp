// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractDataScenario.h"
#if SIMURGHRTC_WITH_DIRECT
#include "Dom/JsonObject.h"
using namespace SimurghRTC;
namespace
{
FSimurghRTCDataMessage Binary(int32 Size, int32 Seed)
{
    FSimurghRTCDataMessage M; M.Kind = ESimurghRTCMessageKind::Binary; M.Bytes.SetNumUninitialized(Size);
    for (int32 I = 0; I < Size; ++I) M.Bytes[I] = uint8(I * 17 + Seed);
    return M;
}
FSimurghRTCDataMessage PayloadAt(int32 Index)
{
    FSimurghRTCDataMessage M;
    if (Index == 0)
    {
        M.Kind = ESimurghRTCMessageKind::Text;
        const TCHAR Text[] = TEXT("hello\0سلام 🌍"); M.Text = FString(UE_ARRAY_COUNT(Text) - 1, Text);
    }
    else if (Index == 1) M.Kind = ESimurghRTCMessageKind::Text;
    else if (Index == 2) M = Binary(0, 0);
    else if (Index == 3) M = Binary(FNativeDataLimits::MessageBytes, 9);
    else if (Index == 4) M = Binary(257, 31);
    else { M.Kind = ESimurghRTCMessageKind::Text; M.Text = FString::ChrN(FNativeDataLimits::MessageBytes, 'x'); }
    return M;
}
bool Same(const FSimurghRTCDataMessage& A, const FSimurghRTCDataMessage& B)
{ return A.Kind == B.Kind && A.Text.Len() == B.Text.Len() && FMemory::Memcmp(*A.Text, *B.Text, A.Text.Len() * sizeof(TCHAR)) == 0 && A.Bytes == B.Bytes; }
}
FSimurghRTCChannelOptions FContractDataScenario::Options(int32 Mode)
{
    FSimurghRTCChannelOptions O; O.Label = FString::Printf(TEXT("sr103-%d"), Mode); O.bOrdered = Mode % 2 == 0;
    O.Reliability = static_cast<ESimurghRTCReliability>(Mode / 2);
    O.ReliabilityValue = Mode < 2 ? 0 : Mode < 4 ? (Mode == 2 ? 0 : 2) : 10000;
    return O;
}
FContractDataScenario::FContractDataScenario(TSharedRef<FDirectClient> InClient) : Client(InClient)
{ Client->SetChannelHandler([this](const FNativeChannelEvent& E) { Event(E); }); }
FContractDataScenario::~FContractDataScenario() { Client->SetChannelHandler({}); }
void FContractDataScenario::Fail(const TCHAR* Why) { if (Error.IsEmpty()) Error = Why; }
void FContractDataScenario::Event(const FNativeChannelEvent& E)
{
    bGameThread &= IsInGameThread();
    if (E.Type == ENativeDataEvent::Error) { Fail(TEXT("Channel error callback.")); return; }
    if (E.Channel.Info.Handle.Id != Channel.Id) { Fail(TEXT("Unexpected or foreign channel callback.")); return; }
    if (E.Type == ENativeDataEvent::Changed)
    {
        if (Phase == 5 && !E.Channel.bWritable && E.Channel.State == ENativeChannelState::Open) bBlocked = true;
        if (bBlocked && E.Channel.bWritable) bResumed = true;
        return;
    }
    if (E.Type != ENativeDataEvent::Message) return;
    if (Expected.IsEmpty() || !Same(Expected[0], E.Message)) { Fail(TEXT("Round-trip bytes, text, kind or order differed.")); return; }
    Expected.RemoveAt(0); ++Received;
    if (E.Message.Kind == ESimurghRTCMessageKind::Text) ++TextMessages; else ++BinaryMessages;
}
bool FContractDataScenario::Send(FSimurghRTCDataMessage Message)
{
    const auto R = Message.Kind == ESimurghRTCMessageKind::Text ? Client->SendText(Channel, Message.Text) : Client->SendBytes(Channel, Message.Bytes);
    if (R.IsError()) { Fail(TEXT("Expected send was rejected.")); return false; }
    Expected.Add(Message);
    // Mutation after return must not alter the queued copy.
    for (auto& Byte : Message.Bytes) Byte ^= 255;
    Message.Text = TEXT("mutated after admission");
    return true;
}
bool FContractDataScenario::Tick()
{
    if (!Error.IsEmpty()) return true;
    if (Mode == 6) return true;
    if (Phase == 0)
    {
        if (Client->OpenChannel(Options(Mode), Channel).IsError()) { Fail(TEXT("OpenChannel failed.")); return true; }
        UE_LOG(LogTemp, Display, TEXT("SIMURGHRTC_DATA mode=%d opening"), Mode);
        Phase = 1; return false;
    }
    FNativeChannelSnapshot S;
    if (Client->GetChannelInfo(Channel, S).IsError()) { Fail(TEXT("GetChannelInfo failed.")); return true; }
    if (Phase == 1 && S.State == ENativeChannelState::Open && S.bWritable)
    {
        const auto O = Options(Mode);
        if (S.Info.Options.Label != O.Label || S.Info.Options.bOrdered != O.bOrdered || S.Info.Options.Reliability != O.Reliability
            || S.Info.Options.ReliabilityValue != O.ReliabilityValue || S.Info.MaximumMessageBytes != 65536 || S.bRemote)
        { Fail(TEXT("Negotiated channel options/limits differed.")); return true; }
        auto TooLarge = Binary(65537, 0);
        if (Client->SendBytes(Channel, TooLarge.Bytes).Code != ESimurghRTCErrorCode::InvalidArgument
            || Client->SendText(Channel, FString::ChrN(65537, 'x')).Code != ESimurghRTCErrorCode::InvalidArgument)
        { Fail(TEXT("Oversized message was accepted.")); return true; }
        FSimurghRTCChannelHandle Foreign; Foreign.Id = FGuid::NewGuid();
        if (Client->SendBytes(Foreign, {}).Code != ESimurghRTCErrorCode::InvalidState)
        { Fail(TEXT("Foreign handle was accepted.")); return true; }
        Rejected += 3; Phase = 2;
    }
    if (Phase == 2 && Expected.IsEmpty())
    {
        if (Payload < 6) { Send(PayloadAt(Payload++)); return false; }
        Phase = Mode == 0 ? 3 : 7;
    }
    if (Phase == 3 && S.Info.BufferedBytes == 0)
    {
        for (int32 I = 0; I < 32; ++I) if (!Send(Binary(4, I))) return true;
        Phase = 4; return false;
    }
    if (Phase == 4 && Expected.IsEmpty() && S.Info.BufferedBytes == 0)
    {
        Phase = 5;
        for (int32 I = 0; I < 4; ++I) if (!Send(Binary(65536, 100 + I))) return true;
        if (Client->SendBytes(Channel, Binary(65536, 104).Bytes).Code != ESimurghRTCErrorCode::ResourceExhausted)
        { Fail(TEXT("Send queue did not reject its fifth maximum-size message.")); return true; }
        ++Rejected; return false;
    }
    if (Phase == 5 && Expected.IsEmpty() && bBlocked && bResumed && S.bWritable && S.Info.BufferedBytes == 0)
    { Send(Binary(65536, 104)); Phase = 6; return false; }
    if (Phase == 6 && Expected.IsEmpty()) Phase = 7;
    if (Phase == 7 && Expected.IsEmpty())
    {
        if (Client->CloseChannel(Channel).IsError() || Client->CloseChannel(Channel).IsError()
            || Client->SendBytes(Channel, {}).Code != ESimurghRTCErrorCode::InvalidState)
        { Fail(TEXT("Close was not idempotent or send after close was accepted.")); return true; }
        ++Rejected; Phase = 8;
    }
    if (Phase == 8 && S.State == ENativeChannelState::Closed)
    { UE_LOG(LogTemp, Display, TEXT("SIMURGHRTC_DATA mode=%d closed messages=%d"), Mode, Received); ++Closed; ++Mode; Payload = 0; Phase = 0; }
    return false;
}
TSharedRef<FJsonObject> FContractDataScenario::Report() const
{
    auto J = MakeShared<FJsonObject>(); J->SetNumberField(TEXT("phase"), Phase); J->SetNumberField(TEXT("modes"), Mode); J->SetNumberField(TEXT("closed_channels"), Closed);
    J->SetNumberField(TEXT("messages"), Received); J->SetNumberField(TEXT("text_messages"), TextMessages); J->SetNumberField(TEXT("binary_messages"), BinaryMessages);
    J->SetNumberField(TEXT("rejected_sends"), Rejected); J->SetNumberField(TEXT("ordered_burst"), 32);
    J->SetBoolField(TEXT("backpressure_observed"), bBlocked); J->SetBoolField(TEXT("writable_resumed"), bResumed);
    J->SetBoolField(TEXT("callbacks_on_game_thread"), bGameThread); J->SetBoolField(TEXT("all_echoes_received"), Expected.IsEmpty());
    return J;
}
#endif

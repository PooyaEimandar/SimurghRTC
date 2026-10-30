// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractChannelValidation.h"
#include "ContractDataExample.h"
#include "Dom/JsonObject.h"
namespace
{
FSimurghRTCDataMessage Binary(int32 Size, int32 Seed)
{
    FSimurghRTCDataMessage M; M.Kind = ESimurghRTCMessageKind::Binary; M.Bytes.SetNumUninitialized(Size);
    for (int32 I = 0; I < Size; ++I) M.Bytes[I] = uint8(I * 17 + Seed);
    return M;
}
FSimurghRTCDataMessage Text(const FString& Value)
{ FSimurghRTCDataMessage M; M.Kind = ESimurghRTCMessageKind::Text; M.Text = Value; return M; }
bool Same(const FSimurghRTCDataMessage& A, const FSimurghRTCDataMessage& B)
{
    return A.Kind == B.Kind && A.Text.Len() == B.Text.Len() && A.Bytes == B.Bytes
        && FMemory::Memcmp(*A.Text,*B.Text,A.Text.Len()*sizeof(TCHAR)) == 0;
}
}
void FContractChannelValidation::Opened(AContractDataExample& A)
{
    FSimurghRTCChannelInfo Info; const auto Error = A.Session->GetChannelInfo(A.Channel,Info);
    bOptionsChecked = !Error.IsError() && Info.bOpen && Info.Handle.Id == A.Channel.Id && Info.MaximumMessageBytes == Limit
        && Info.Options.Label == A.Options.Label && Info.Options.bOrdered == A.Options.bOrdered
        && Info.Options.Reliability == A.Options.Reliability && Info.Options.ReliabilityValue == A.Options.ReliabilityValue;
    if (!bOptionsChecked) A.GetExampleResult()->Fail(TEXT("Negotiated options or message limit differ."));
    Phase = 1;
}
void FContractChannelValidation::Send(AContractDataExample& A, FSimurghRTCDataMessage M, ESimurghRTCErrorCode Code)
{
    if (!A.GetExampleResult()->Error.IsEmpty()) return;
    Sending = MoveTemp(M); ExpectedCode = Code; bSending = true;
    A.bValidationText = Sending.Kind == ESimurghRTCMessageKind::Text;
    A.TextToSend = Sending.Text; A.BytesToSend = Sending.Bytes;
    A.RequestValidationSend();
    if (bSending) A.GetExampleResult()->Fail(TEXT("The sample did not execute its public send node/call."));
    // Mutation after the public call must not alter the provider's admitted copy.
    A.TextToSend = TEXT("mutated after admission");
    for (auto& Byte : A.BytesToSend) Byte ^= 255;
}
void FContractChannelValidation::SendResult(AContractDataExample& A, const FSimurghRTCError& Error)
{
    auto R = A.GetExampleResult(); R->bGameThread &= IsInGameThread();
    if (!bSending || Error.Code != ExpectedCode || Error.SessionId != A.Session->GetSessionId())
    { R->Fail(TEXT("Send admission/error correlation differed.")); return; }
    bSending = false;
    if (Error.IsError()) ++Rejected;
    else { Expected.Add(Sending); ++R->Sent; }
}
void FContractChannelValidation::Message(AContractDataExample& A, const FSimurghRTCDataMessage& M)
{
    auto R = A.GetExampleResult(); R->Callback(); ++R->Received;
    // Unordered channels must preserve the set, without imposing delivery order.
    const int32 Index = A.Options.bOrdered ? (Expected.IsEmpty() || !Same(Expected[0],M) ? INDEX_NONE : 0)
        : Expected.IndexOfByPredicate([&](const auto& E) { return Same(E,M); });
    if (M.Channel.Id != A.Channel.Id || Index == INDEX_NONE)
    { R->Fail(TEXT("Channel validation received a mismatched, duplicate or out-of-order echo.")); return; }
    Expected.RemoveAt(Index); ++Received;
    if (M.Kind == ESimurghRTCMessageKind::Text) { ++TextMessages; R->bTextReceived = true; }
    else { ++BinaryMessages; R->bBytesReceived = true; }
}
void FContractChannelValidation::Changed(const FSimurghRTCChannelInfo& Info)
{ if (Info.bOpen) { ++BufferEvents; PeakBuffered = FMath::Max(PeakBuffered,Info.BufferedBytes); } }
void FContractChannelValidation::Tick(AContractDataExample& A)
{
    if (Phase == 0 || Phase == 9 || !A.GetExampleResult()->Error.IsEmpty()) return;
    FSimurghRTCChannelInfo Info;
    if (A.Session->GetChannelInfo(A.Channel,Info).IsError() || !Info.bOpen || Info.BufferedBytes < 0 || Info.BufferedBytes > 262144)
    { A.GetExampleResult()->Fail(TEXT("Invalid channel or unbounded buffered amount.")); return; }
    PeakBuffered = FMath::Max(PeakBuffered,Info.BufferedBytes);
    if (Phase == 1)
    {
        Send(A,Binary(Limit+1,0),ESimurghRTCErrorCode::InvalidArgument);
        Send(A,Text(FString::ChrN(Limit+1,'x')),ESimurghRTCErrorCode::InvalidArgument);
        // U+00E9 occupies two UTF-8 bytes: check the wire-byte boundary, not TCHAR count.
        Send(A,Text(FString::ChrN(Limit/2+1,TCHAR(0x00e9))),ESimurghRTCErrorCode::InvalidArgument);
        Phase = 2;
    }
    if (Phase == 2 && Expected.IsEmpty())
    {
        switch (Payload++)
        {
        case 0: { const TCHAR Value[] = TEXT("hello\0سلام 🌍"); Send(A,Text(FString(UE_ARRAY_COUNT(Value)-1,Value))); return; }
        case 1: Send(A,Text(TEXT(""))); return;
        case 2: Send(A,Binary(0,0)); return;
        case 3: Send(A,Binary(Limit,9)); return;
        case 4: Send(A,Text(FString::ChrN(Limit,'x'))); return;
        case 5: Send(A,Text(FString::ChrN(Limit/2,TCHAR(0x00e9)))); return;
        default: Phase = 3;
        }
    }
    if (Phase == 3 && Expected.IsEmpty() && Info.BufferedBytes == 0)
    {
        for (int32 I = 0; I < 32; ++I) Send(A,Binary(4,I));
        Phase = 4; return;
    }
    if (Phase == 4 && Expected.IsEmpty() && Info.BufferedBytes == 0)
    {
        if (Mode >= 2) { Phase = 8; }
        else
        {
            const int32 Count = 262144/Limit;
            for (int32 I = 0; I < Count; ++I) Send(A,Binary(Limit,100+I));
            Send(A,Binary(Limit,100+Count),ESimurghRTCErrorCode::ResourceExhausted); ++ByteQueueRejections;
            A.Session->GetChannelInfo(A.Channel,Info); PeakBuffered = FMath::Max(PeakBuffered,Info.BufferedBytes);
            if (Info.BufferedBytes != 262144) A.GetExampleResult()->Fail(TEXT("Byte queue saturation was not observable."));
            Phase = 5; return;
        }
    }
    if (Phase == 5 && Expected.IsEmpty() && Info.BufferedBytes == 0)
    {
        Send(A,Binary(Limit,100+262144/Limit)); bByteQueueRecovered = true;
        Phase = 6; return;
    }
    if (Phase == 6 && Expected.IsEmpty() && Info.BufferedBytes == 0)
    {
        for (int32 I = 0; I < 64; ++I) Send(A,Binary(0,0));
        Send(A,Binary(0,0),ESimurghRTCErrorCode::ResourceExhausted); ++SlotQueueRejections;
        Phase = 7; return;
    }
    if (Phase == 7 && Expected.IsEmpty() && Info.BufferedBytes == 0)
    {
        Send(A,Binary(0,0)); bSlotQueueRecovered = true; Phase = 8; return;
    }
    if (Phase == 8 && Expected.IsEmpty() && Info.BufferedBytes == 0)
    { Phase = 9; ++A.GetExampleResult()->CompletedRounds; A.RequestCloseChannel(); }
}
TSharedRef<FJsonObject> FContractChannelValidation::Report() const
{
    auto J = MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("mode"),Mode); J->SetNumberField(TEXT("phase"),Phase);
    J->SetNumberField(TEXT("maximum_message_bytes"),Limit); J->SetNumberField(TEXT("messages"),Received);
    J->SetNumberField(TEXT("text_messages"),TextMessages); J->SetNumberField(TEXT("binary_messages"),BinaryMessages);
    J->SetNumberField(TEXT("rejected_sends"),Rejected); J->SetNumberField(TEXT("burst_messages"),32);
    J->SetNumberField(TEXT("byte_queue_rejections"),ByteQueueRejections); J->SetNumberField(TEXT("slot_queue_rejections"),SlotQueueRejections);
    J->SetNumberField(TEXT("peak_buffered_bytes"),PeakBuffered); J->SetNumberField(TEXT("buffer_events"),BufferEvents);
    J->SetBoolField(TEXT("byte_queue_recovered"),bByteQueueRecovered); J->SetBoolField(TEXT("slot_queue_recovered"),bSlotQueueRecovered);
    J->SetBoolField(TEXT("options_checked"),bOptionsChecked); J->SetBoolField(TEXT("all_echoes_received"),Expected.IsEmpty());
    return J;
}

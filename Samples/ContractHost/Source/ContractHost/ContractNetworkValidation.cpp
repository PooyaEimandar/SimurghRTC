// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractNetworkValidation.h"
#include "ContractDataExample.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

FContractNetworkValidation::FContractNetworkValidation(int32 InMode) : Mode(InMode)
{ FParse::Value(FCommandLine::Get(),TEXT("SimurghRTCExampleGate="),GatePath); }
FSimurghRTCChannelOptions FContractNetworkValidation::Options(int32 Mode, int32 Index)
{
    FSimurghRTCChannelOptions O;
    O.Label = FString::Printf(TEXT("sr107-network-%d-%d"),Mode,Index);
    O.bOrdered = Mode % 2 == 0;
    O.Reliability = Mode == 6 ? ESimurghRTCReliability::Reliable : static_cast<ESimurghRTCReliability>(Mode/2);
    O.ReliabilityValue = Mode == 4 || Mode == 5 ? 100 : 0;
    return O;
}
bool FContractNetworkValidation::ParseOptions(const FString& Label, FSimurghRTCChannelOptions& Out)
{
    for (int32 M = 0; M <= 6; ++M) for (int32 I = 0; I < (M == 6 ? 5 : 1); ++I)
    { const auto O = Options(M,I); if (Label == O.Label) { Out = O; return true; } }
    return false;
}
bool FContractNetworkValidation::Gate(bool Release) const
{ return !GatePath.IsEmpty() && IFileManager::Get().FileExists(*(GatePath+(Release ? TEXT(".release") : TEXT("")))); }
void FContractNetworkValidation::Opened(AContractDataExample& A)
{
    FSimurghRTCChannelInfo Info;
    const auto O = Options(Mode,Channels.Num());
    if (A.Session->GetChannelInfo(A.Channel,Info).IsError() || !Info.bOpen || Info.MaximumMessageBytes != 65536
        || Info.Options.Label != O.Label || Info.Options.bOrdered != O.bOrdered || Info.Options.Reliability != O.Reliability
        || Info.Options.ReliabilityValue != O.ReliabilityValue)
        A.GetExampleResult()->Fail(TEXT("Impairment probe negotiated different channel options."));
    Channels.Add(A.Channel); bOpening = false;
    if (Channels.Num() == (Mode == 6 ? 5 : 1)) Phase = 1;
}
void FContractNetworkValidation::Send(AContractDataExample& A, int32 Index, int32 Seed, int32 Size, bool Expired, bool Reject, bool Text)
{
    if (!A.GetExampleResult()->Error.IsEmpty()) return;
    A.Channel = Channels[Index]; A.Options = Options(Mode,Index);
    Sending = {}; Sending.Channel = A.Channel;
    Sending.Kind = Text ? ESimurghRTCMessageKind::Text : ESimurghRTCMessageKind::Binary;
    if (Text) Sending.Text = FString::Printf(TEXT("network fence %d — سلام 🌍"),Seed);
    else { Sending.Bytes.SetNumUninitialized(Size); for (int32 I = 0; I < Size; ++I) Sending.Bytes[I] = uint8(I*17+Seed); }
    bSending = true; bExpired = Expired; bReject = Reject;
    A.bValidationText = Text; A.TextToSend = Sending.Text; A.BytesToSend = Sending.Bytes;
    A.RequestValidationSend();
    if (bSending) A.GetExampleResult()->Fail(TEXT("Impairment probe did not execute its public send node/call."));
    A.TextToSend = TEXT("mutated after admission"); for (auto& Byte : A.BytesToSend) Byte ^= 255;
}
void FContractNetworkValidation::SendResult(AContractDataExample& A, const FSimurghRTCError& Error)
{
    auto R = A.GetExampleResult();
    if (!bSending || Error.SessionId != A.Session->GetSessionId()
        || Error.Code != (bReject ? ESimurghRTCErrorCode::ResourceExhausted : ESimurghRTCErrorCode::None))
    { R->Fail(TEXT("Impairment probe send admission differed.")); return; }
    bSending = false;
    if (bReject) ++Rejected;
    else { ++R->Sent; if (bExpired) ++ExpiredSent; else Expected.Add(Sending); }
}
void FContractNetworkValidation::Message(AContractDataExample& A, const FSimurghRTCDataMessage& M)
{
    auto R = A.GetExampleResult(); R->Callback(); ++R->Received;
    auto Same = [&](const FSimurghRTCDataMessage& E) { return E.Channel.Id == M.Channel.Id && E.Kind == M.Kind && E.Text == M.Text && E.Bytes == M.Bytes; };
    const int32 Index = Expected.IndexOfByPredicate(Same);
    const int32 First = Expected.IndexOfByPredicate([&](const auto& E) { return E.Channel.Id == M.Channel.Id; });
    if (Index == INDEX_NONE || (Mode % 2 == 0 && Index != First))
    { R->Fail(TEXT("Expired, duplicate, misrouted or out-of-order message arrived.")); return; }
    Expected.RemoveAt(Index); ++Received;
    if (M.Kind == ESimurghRTCMessageKind::Text) R->bTextReceived = true; else R->bBytesReceived = true;
}
void FContractNetworkValidation::Changed(AContractDataExample& A, const FSimurghRTCChannelInfo& Info)
{
    FSimurghRTCChannelOptions O;
    if (!ParseOptions(Info.Options.Label,O)) A.GetExampleResult()->Fail(TEXT("Unexpected channel event during impairment probe."));
    if (Info.bOpen) ++BufferEvents;
}
bool FContractNetworkValidation::Closed(AContractDataExample& A)
{
    if (!bClosing || A.Channel.Id != Channels[ClosedCount].Id) A.GetExampleResult()->Fail(TEXT("Wrong channel close completion."));
    bClosing = false; ++ClosedCount;
    if (ClosedCount == Channels.Num()) { Phase = 9; ++A.GetExampleResult()->CompletedRounds; return true; }
    return false;
}
void FContractNetworkValidation::Tick(AContractDataExample& A)
{
    auto R = A.GetExampleResult(); if (!R->Error.IsEmpty() || Phase == 9 || !A.Session) return;
    if (Phase == 0)
    {
        if (Mode == 6 && !Channels.IsEmpty() && !bOpening)
        { bOpening = true; A.Options = Options(Mode,Channels.Num()); A.RequestOpenChannel(); }
        return;
    }
    if (Phase == 5)
    {
        if (!bClosing) { bClosing = true; A.Channel = Channels[ClosedCount]; A.RequestCloseChannel(); }
        return;
    }
    int64 Buffered = 0;
    for (const auto& Handle : Channels)
    {
        FSimurghRTCChannelInfo Info;
        if (A.Session->GetChannelInfo(Handle,Info).IsError() || !Info.bOpen || Info.BufferedBytes < 0 || Info.BufferedBytes > 262144)
        { R->Fail(TEXT("Invalid channel state or unbounded per-channel queue.")); return; }
        Buffered += Info.BufferedBytes;
    }
    PeakBuffered = FMath::Max(PeakBuffered,Buffered);
    if (Buffered > 1048576) R->Fail(TEXT("Unbounded aggregate channel queue."));
    if (Phase == 1 && Gate())
    {
        if (Mode == 6)
        {
            for (int32 C = 0; C < 4; ++C) for (int32 I = 0; I < 4; ++I) Send(A,C,20+C*4+I,65536);
            FSimurghRTCChannelInfo Info; A.Session->GetChannelInfo(Channels[4],Info); EmptyChannelAtRejection = Info.BufferedBytes;
            Send(A,4,90,65536,false,true);
            int64 Total = 0;
            for (const auto& H : Channels) { A.Session->GetChannelInfo(H,Info); Total += Info.BufferedBytes; }
            PeakBuffered = FMath::Max(PeakBuffered,Total);
            if (Total != 1048576 || EmptyChannelAtRejection != 0) R->Fail(TEXT("Peer queue was not full while the fifth channel was empty."));
        }
        else if (Mode < 2)
        { for (int32 I = 0; I < 32; ++I) Send(A,0,I,1024); Send(A,0,200,0,false,false,true); }
        else for (int32 I = 0; I < 8; ++I) Send(A,0,100+I,1024,true);
        Phase = 2; return;
    }
    if (Phase == 2)
    {
        if (Mode == 6 && Expected.IsEmpty() && Buffered == 0)
        {
            Send(A,4,90,65536); // Retry the exact rejected payload once the shared queue drains.
            for (int32 C = 0; C < 5; ++C) Send(A,C,200+C,0,false,false,true);
            bRecovered = true; Phase = 3; return;
        }
        if (Mode < 2 && Expected.IsEmpty() && Buffered == 0) Phase = 3;
        if (Mode >= 2 && Mode <= 5 && Gate(true))
        { Send(A,0,220,1024); Send(A,0,221,0,false,false,true); Phase = 3; return; }
    }
    if (Phase == 3 && Expected.IsEmpty() && Buffered == 0 && (Mode == 6 || Gate(true)))
    { Phase = 4; QuietAt = FPlatformTime::Seconds(); }
    // Observe longer than the local SCTP retransmission backoff, so a late old
    // message cannot be mistaken for an expired message merely because it is delayed.
    if (Phase == 4 && FPlatformTime::Seconds()-QuietAt >= (Mode >= 2 && Mode <= 5 ? 10 : 2)) Phase = 5;
}
TSharedRef<FJsonObject> FContractNetworkValidation::Report() const
{
    auto J = MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("quiet_seconds"),QuietAt > 0 ? FPlatformTime::Seconds()-QuietAt : 0);
    J->SetNumberField(TEXT("mode"),Mode); J->SetNumberField(TEXT("phase"),Phase);
    J->SetNumberField(TEXT("channels"),Channels.Num()); J->SetNumberField(TEXT("channels_closed"),ClosedCount);
    J->SetNumberField(TEXT("messages"),Received); J->SetNumberField(TEXT("expired_admissions"),ExpiredSent);
    J->SetNumberField(TEXT("rejected_sends"),Rejected); J->SetNumberField(TEXT("peak_buffered_bytes"),PeakBuffered);
    J->SetNumberField(TEXT("empty_channel_at_rejection"),EmptyChannelAtRejection); J->SetNumberField(TEXT("buffer_events"),BufferEvents);
    J->SetBoolField(TEXT("aggregate_queue_recovered"),bRecovered); J->SetBoolField(TEXT("all_expected_received"),Expected.IsEmpty());
    return J;
}

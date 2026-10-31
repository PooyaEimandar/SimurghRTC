// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "ContractSendDeadlineValidation.h"
#include "ContractDataExample.h"
#include "Dom/JsonObject.h"

void FContractSendDeadlineValidation::Opened(AContractDataExample& A)
{
    auto R = A.GetExampleResult(); R->bAwaitingFailure = bExpire;
    AdmittedAt = FPlatformTime::Seconds(); Phase = 1;
    for (int32 I = 0; I < 2; ++I)
    {
        FSimurghRTCDataMessage M; M.Channel = A.Channel;
        M.Kind = I == 0 ? ESimurghRTCMessageKind::Text : ESimurghRTCMessageKind::Binary;
        if (I == 0) M.Text = TEXT("queued deadline — سلام 🌍"); else M.Bytes = {0,1,0,127,128,255};
        Expected.Add(M); A.bValidationText = I == 0; A.TextToSend = M.Text; A.BytesToSend = M.Bytes;
        bSending = true; A.RequestValidationSend();
        if (bSending) R->Fail(TEXT("Deadline probe did not run its public send flow."));
        A.TextToSend = TEXT("changed after admission"); A.BytesToSend = {42};
    }
}
void FContractSendDeadlineValidation::SendResult(AContractDataExample& A, const FSimurghRTCError& Error)
{
    auto R = A.GetExampleResult();
    if (!bSending || Error.IsError() || Error.SessionId != A.Session->GetSessionId()) R->Fail(TEXT("Deadline send was not admitted."));
    bSending = false; ++R->Sent;
}
void FContractSendDeadlineValidation::Message(AContractDataExample& A, const FSimurghRTCDataMessage& M)
{
    auto R = A.GetExampleResult(); R->Callback(); ++R->Received;
    if (bExpire || Expected.IsEmpty()) { R->Fail(TEXT("Expired or duplicate queued payload arrived.")); return; }
    const auto E = Expected[0];
    if (M.Channel.Id != E.Channel.Id || M.Kind != E.Kind || M.Text != E.Text || M.Bytes != E.Bytes)
        R->Fail(TEXT("Released queued payload differs or arrived out of order."));
    Expected.RemoveAt(0); ++Received;
    if (M.Kind == ESimurghRTCMessageKind::Text) R->bTextReceived = true; else R->bBytesReceived = true;
}
void FContractSendDeadlineValidation::Tick(AContractDataExample& A)
{
    if (Phase == 0 || Phase == 3) return;
    auto R = A.GetExampleResult(); const double Now = FPlatformTime::Seconds();
    if (Phase == 1)
    {
        if (A.Session->GetState() == ESimurghRTCSessionState::Failed)
        {
            SettledAfter = Now-AdmittedAt;
            if (!bExpire || A.Session->GetConnectionError().Code != ESimurghRTCErrorCode::Timeout
                || !R->bFailureObserved || Received != 0 || SettledAfter < 30 || SettledAfter > 40)
                R->Fail(TEXT("Queued send failed outside its real deadline or with the wrong error."));
            FSimurghRTCChannelInfo Info;
            if (A.Session->GetChannelInfo(A.Channel,Info).Code != ESimurghRTCErrorCode::InvalidState
                || A.Session->SendBytes(A.Channel,{99}).Code != ESimurghRTCErrorCode::InvalidState)
                R->Fail(TEXT("Timed-out channel remained usable."));
            Phase = 2; QuietAt = Now;
        }
        else
        {
            // The worker retires its peer before the game-thread failure event arrives.
            // At the deadline, wait for that event instead of querying the retired peer.
            if (bExpire && Now-AdmittedAt >= 30)
            {
                if (Now-AdmittedAt >= 40) R->Fail(TEXT("Queued send deadline did not publish its failure."));
                return;
            }
            FSimurghRTCChannelInfo Info;
            const auto InfoError = A.Session->GetChannelInfo(A.Channel,Info);
            if (bExpire && InfoError.Code == ESimurghRTCErrorCode::InvalidState && FPlatformTime::Seconds()-AdmittedAt >= 30) return;
            if (InfoError.IsError() || !Info.bOpen || Info.BufferedBytes < 0 || Info.BufferedBytes > 262144)
                R->Fail(TEXT("Invalid queue while waiting for its deadline."));
            PeakBuffered = FMath::Max(PeakBuffered,Info.BufferedBytes);
            if (!bExpire && Expected.IsEmpty() && Info.BufferedBytes == 0)
            {
                SettledAfter = Now-AdmittedAt;
                if (SettledAfter < 3 || SettledAfter >= 30) R->Fail(TEXT("Release control did not wait below the send deadline."));
                Phase = 2; QuietAt = Now;
            }
        }
    }
    if (Phase == 2 && Now-QuietAt >= 3)
    {
        Phase = 3; ++R->CompletedRounds;
        if (bExpire) A.RequestDisconnect(); else A.RequestCloseChannel();
    }
}
TSharedRef<FJsonObject> FContractSendDeadlineValidation::Report() const
{
    auto J = MakeShared<FJsonObject>(); J->SetBoolField(TEXT("expire"),bExpire); J->SetNumberField(TEXT("phase"),Phase);
    J->SetNumberField(TEXT("admitted"),2); J->SetNumberField(TEXT("received"),Received);
    J->SetNumberField(TEXT("settled_after_seconds"),SettledAfter); J->SetNumberField(TEXT("peak_buffered_bytes"),PeakBuffered);
    J->SetNumberField(TEXT("waiting_seconds"),AdmittedAt > 0 ? FPlatformTime::Seconds()-AdmittedAt : 0);
    J->SetNumberField(TEXT("quiet_seconds"),QuietAt > 0 ? FPlatformTime::Seconds()-QuietAt : 0); return J;
}

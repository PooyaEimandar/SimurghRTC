// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCSession.h"
#include "SimurghRTCSessionTransport.h"

void USimurghRTCSession::ClearMedia(bool bNotify)
{
    const auto Local = LocalAudio, Remote = RemoteAudio;
    AudioSources.Reset(); LocalAudio = {}; RemoteAudio = {}; bDeliverAudio = false; bRemoteAudioDetached = false;
    if (bNotify && IsUsable() && Local.Handle.IsValid()) OnTrackChanged.Broadcast(Local, false);
    if (bNotify && IsUsable() && Remote.Handle.IsValid()) OnTrackChanged.Broadcast(Remote, false);
}

void USimurghRTCSession::TickAudio()
{
    const auto Current = Transport;
    const auto Generation = TransportGeneration;
    if (!Current || !Current->HasAudio()) return;
    auto Live = [&] { return IsUsable() && State == ESimurghRTCSessionState::Connected && Generation == TransportGeneration; };
    if (!bRemoteAudioDetached && !RemoteAudio.Handle.IsValid() && Current->HasRemoteAudio())
    {
        RemoteAudio.Handle.Id = FGuid::NewGuid(); RemoteAudio.bRemote = true;
        const auto Info = RemoteAudio; OnTrackChanged.Broadcast(Info, true);
    }
    for (int32 Count = 0; Live() && Count < 20; ++Count)
    {
        FSimurghRTCAudioFrame Frame;
        if (!Current->PollAudio(Frame)) break;
        if (bDeliverAudio && RemoteAudio.Handle.IsValid()) OnAudioFrame.Broadcast(RemoteAudio.Handle, Frame);
    }
}

FSimurghRTCError USimurghRTCSession::ExecuteAudioOperation(FName Name, ESimurghRTCTrackKind Kind,
    FSimurghRTCSourceHandle Source, FSimurghRTCTrackHandle Track, bool bEnabled, FSimurghRTCOperationResult& Out)
{
    using namespace SimurghRTC;
    auto Invalid = [] { return SimurghRTC::MakeError(ESimurghRTCErrorCode::InvalidState, TEXT("The source or track does not belong to this live session.")); };
    if (Provider != ESimurghRTCProvider::DirectUnreal)
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("Generated audio is currently integrated with Direct only."));
    if (Name == TEXT("CreateGeneratedSource"))
    {
        if (Kind != ESimurghRTCTrackKind::Audio)
            return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("Generated video transport is not integrated yet."));
        if (State == ESimurghRTCSessionState::Closing) return Invalid();
        if (AudioSources.Num() >= 8) return SimurghRTC::MakeError(ESimurghRTCErrorCode::ResourceExhausted, TEXT("A session supports at most eight generated audio sources."));
        Out.Source.Id = FGuid::NewGuid(); AudioSources.Add(Out.Source.Id); return {};
    }
    if (Name == TEXT("StopSource") || Name == TEXT("SetSourceMuted"))
    {
        auto* Found = AudioSources.Find(Source.Id);
        if (!Found) return Invalid();
        if (!LocalAudio.Handle.IsValid() || LocalAudio.Source.Id != Source.Id)
        {
            if (Name == TEXT("StopSource")) AudioSources.Remove(Source.Id);
            else Found->bMuted = bEnabled;
            Out.Source = Source; return {};
        }
    }
    if (!Transport || !Transport->HasAudio())
        return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotNegotiated, TEXT("Connect a Direct Call-purpose session before attaching audio."));
    if (Name == TEXT("SetDecodedFrameDelivery"))
    {
        if (!RemoteAudio.Handle.IsValid() || Track.Id != RemoteAudio.Handle.Id) return Invalid();
        // Drop queued frames at either transition; the next callback uses fresh polling.
        FSimurghRTCAudioFrame Discard;
        for (int32 I = 0; I < 20 && Transport->PollAudio(Discard); ++I) {}
        bDeliverAudio = bEnabled; Out.Track = Track; return {};
    }
    if (Name == TEXT("DetachTrack"))
    {
        if (RemoteAudio.Handle.IsValid() && Track.Id == RemoteAudio.Handle.Id)
        {
            const auto Removed = RemoteAudio; RemoteAudio = {}; bDeliverAudio = false; bRemoteAudioDetached = true;
            Out.Track = Track; OnTrackChanged.Broadcast(Removed, false); return {};
        }
        if (!LocalAudio.Handle.IsValid() || Track.Id != LocalAudio.Handle.Id) return Invalid();
        const auto Error = Transport->MuteAudio(true); if (Error.IsError()) return Error;
        const auto Removed = LocalAudio; LocalAudio = {}; Out.Track = Track;
        OnTrackChanged.Broadcast(Removed, false); return {};
    }
    auto* Found = AudioSources.Find(Source.Id);
    if (!Found) return Invalid();
    if (Name == TEXT("AttachSource") || Name == TEXT("ReplaceSource"))
    {
        const bool Replace = Name == TEXT("ReplaceSource");
        if (Replace ? (!LocalAudio.Handle.IsValid() || Track.Id != LocalAudio.Handle.Id) : LocalAudio.Handle.IsValid()) return Invalid();
        const auto Error = Transport->MuteAudio(Found->bMuted); if (Error.IsError()) return Error;
        if (!Replace) LocalAudio.Handle.Id = FGuid::NewGuid();
        LocalAudio.Source = Source; LocalAudio.bMuted = Found->bMuted;
        Out.Source = Source; Out.Track = LocalAudio.Handle;
        const auto Info = LocalAudio; OnTrackChanged.Broadcast(Info, true); return {};
    }
    if (Name == TEXT("SetSourceMuted"))
    {
        if (LocalAudio.Source.Id == Source.Id)
        {
            const auto Error = Transport->MuteAudio(bEnabled); if (Error.IsError()) return Error;
            LocalAudio.bMuted = bEnabled;
        }
        Found->bMuted = bEnabled; Out.Source = Source;
        if (LocalAudio.Source.Id == Source.Id) { const auto Info = LocalAudio; OnTrackChanged.Broadcast(Info, true); }
        return {};
    }
    if (Name == TEXT("StopSource"))
    {
        if (LocalAudio.Source.Id == Source.Id)
        {
            const auto Error = Transport->MuteAudio(true); if (Error.IsError()) return Error;
        }
        AudioSources.Remove(Source.Id); Out.Source = Source;
        if (LocalAudio.Source.Id == Source.Id)
        {
            const auto Removed = LocalAudio; LocalAudio = {};
            OnTrackChanged.Broadcast(Removed, false);
        }
        return {};
    }
    return SimurghRTC::MakeError(ESimurghRTCErrorCode::NotImplemented, TEXT("This media operation is not implemented."));
}

// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativeRuntime.h"

#if SIMURGHRTC_WITH_WEBRTC
THIRD_PARTY_INCLUDES_START
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/thread.h"
#include "UE58/SimurghRTCHeadlessAudioDevice.h"
THIRD_PARTY_INCLUDES_END

#if !WEBRTC_5414
#error SimurghRTC UE 5.8.3 adapter requires matching WebRTC 5414 headers and library.
#endif
#endif

namespace SimurghRTC
{
#if SIMURGHRTC_WITH_WEBRTC
namespace
{
// Game-thread leases pair our SSL initialization with the final runtime's shutdown.
int32 SSLUsers = 0;

class FProbeObserver final : public webrtc::PeerConnectionObserver
{
public:
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    void OnIceCandidate(const webrtc::IceCandidateInterface*) override {}
};
}
#endif

class FNativeRuntime::FImpl
{
public:
    FNativeFactoryDiagnostics Diagnostics;
#if SIMURGHRTC_WITH_WEBRTC
    bool bSSLAcquired = false;
    bool bNetworkStarted = false;
    bool bWorkerStarted = false;
    bool bSignalingStarted = false;
    std::unique_ptr<rtc::Thread> Network;
    std::unique_ptr<rtc::Thread> Worker;
    std::unique_ptr<rtc::Thread> Signaling;
    rtc::scoped_refptr<webrtc::AudioDeviceModule> AudioDevice;
    rtc::scoped_refptr<webrtc::AudioProcessing> AudioProcessing;
    rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> Factory;
    FProbeObserver Observer; // Outlives Peer, including asynchronous callbacks drained on close.
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> Peer;

    ~FImpl()
    {
        check(IsInGameThread());
        if (bSignalingStarted)
            Signaling->BlockingCall([this]
            {
                if (Peer) Peer->Close();
                Peer = nullptr;
                Factory = nullptr;
            });
        if (bWorkerStarted)
            Worker->BlockingCall([this]
            {
                AudioProcessing = nullptr;
                if (AudioDevice) AudioDevice->Terminate();
                AudioDevice = nullptr;
            });
        // Dependencies stay alive until peers, factory and audio processing have released them.
        if (bSignalingStarted) Signaling->Stop();
        if (bWorkerStarted) Worker->Stop();
        if (bNetworkStarted) Network->Stop();
        Signaling.reset();
        Worker.reset();
        Network.reset();
        if (bSSLAcquired && --SSLUsers == 0) rtc::CleanupSSL();
    }
#endif
};

FNativeRuntime::FNativeRuntime() = default;
FNativeRuntime::~FNativeRuntime() { Stop(); }

bool FNativeRuntime::IsSupported() { return SIMURGHRTC_WITH_WEBRTC != 0; }

bool FNativeRuntime::Start(ENativeFactoryMode Mode, FString& OutError)
{
    check(IsInGameThread());
    OutError.Reset();
    if (Impl) { OutError = TEXT("Factory is already started; stop it before restarting."); return false; }
    if (Mode != ENativeFactoryMode::DataOnly && Mode != ENativeFactoryMode::BuiltinMedia)
    { OutError = TEXT("Invalid native factory mode."); return false; }
#if SIMURGHRTC_WITH_WEBRTC
    auto Candidate = MakeUnique<FImpl>();
    if (SSLUsers == 0 && !rtc::InitializeSSL())
    { OutError = TEXT("WebRTC SSL initialization failed."); return false; }
    ++SSLUsers;
    Candidate->bSSLAcquired = true;
    Candidate->Network = rtc::Thread::CreateWithSocketServer();
    Candidate->Worker = rtc::Thread::Create();
    Candidate->Signaling = rtc::Thread::Create();
    Candidate->Network->SetName("SimurghRTC Network", nullptr);
    Candidate->Worker->SetName("SimurghRTC Worker", nullptr);
    Candidate->Signaling->SetName("SimurghRTC Signaling", nullptr);
    if (!(Candidate->bNetworkStarted = Candidate->Network->Start())
        || !(Candidate->bWorkerStarted = Candidate->Worker->Start())
        || !(Candidate->bSignalingStarted = Candidate->Signaling->Start()))
    { OutError = TEXT("Failed to start WebRTC threads."); return false; }

    if (Mode == ENativeFactoryMode::BuiltinMedia)
    {
        Candidate->Worker->BlockingCall([&]
        {
            Candidate->AudioDevice = new rtc::RefCountedObject<FSimurghRTCHeadlessAudioDevice>();
            Candidate->AudioProcessing = webrtc::AudioProcessingBuilder().Create();
        });
        if (!Candidate->AudioDevice || !Candidate->AudioProcessing)
        { OutError = TEXT("Dummy audio device or audio processing initialization failed."); return false; }
    }

    Candidate->Signaling->BlockingCall([&]
    {
        if (Mode == ENativeFactoryMode::BuiltinMedia)
        {
            Candidate->Factory = webrtc::CreatePeerConnectionFactory(
                Candidate->Network.get(), Candidate->Worker.get(), Candidate->Signaling.get(),
                Candidate->AudioDevice, webrtc::CreateBuiltinAudioEncoderFactory(),
                webrtc::CreateBuiltinAudioDecoderFactory(), webrtc::CreateBuiltinVideoEncoderFactory(),
                webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, Candidate->AudioProcessing);
        }
        else
        {
            webrtc::PeerConnectionFactoryDependencies Dependencies;
            Dependencies.network_thread = Candidate->Network.get();
            Dependencies.worker_thread = Candidate->Worker.get();
            Dependencies.signaling_thread = Candidate->Signaling.get();
            Candidate->Factory = webrtc::CreateModularPeerConnectionFactory(std::move(Dependencies));
        }
        if (Candidate->Factory)
        {
            webrtc::PeerConnectionFactoryInterface::Options Options;
            Options.disable_network_monitor = true;
            Candidate->Factory->SetOptions(Options); // Encryption remains enabled.
            // M109 assumes a media engine when querying RTP capabilities. A modular
            // data-only factory intentionally has none, so never call that API on it.
            if (Mode == ENativeFactoryMode::BuiltinMedia)
            {
                for (const auto& Codec : Candidate->Factory->GetRtpSenderCapabilities(cricket::MEDIA_TYPE_AUDIO).codecs)
                    Candidate->Diagnostics.AudioCodecs.AddUnique(UTF8_TO_TCHAR(Codec.name.c_str()));
                for (const auto& Codec : Candidate->Factory->GetRtpSenderCapabilities(cricket::MEDIA_TYPE_VIDEO).codecs)
                    Candidate->Diagnostics.VideoCodecs.AddUnique(UTF8_TO_TCHAR(Codec.name.c_str()));
            }
        }
    });
    if (!Candidate->Factory) { OutError = TEXT("PeerConnection factory creation failed."); return false; }
    Candidate->Diagnostics.bFactoryReady = true;
    Candidate->Diagnostics.bDummyAudioDevice = Candidate->AudioDevice != nullptr;
    Candidate->Diagnostics.bAudioProcessing = Candidate->AudioProcessing != nullptr;
    Impl = MoveTemp(Candidate);
    return true;
#else
    OutError = TEXT("No native adapter is linked for this target. Android/iOS require separately built artifacts.");
    return false;
#endif
}

bool FNativeRuntime::CreateProbePeerConnection(FString& OutError)
{
    check(IsInGameThread());
    OutError.Reset();
    if (!Impl || !Impl->Diagnostics.bFactoryReady)
    { OutError = TEXT("Start the factory before creating a probe peer."); return false; }
    if (Impl->Diagnostics.bPeerConnectionReady)
    { OutError = TEXT("This probe already owns a peer connection."); return false; }
#if SIMURGHRTC_WITH_WEBRTC
    Impl->Signaling->BlockingCall([&]
    {
        webrtc::PeerConnectionInterface::RTCConfiguration Config;
        Config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        webrtc::PeerConnectionDependencies Dependencies(&Impl->Observer);
        auto Result = Impl->Factory->CreatePeerConnectionOrError(Config, std::move(Dependencies));
        if (Result.ok()) Impl->Peer = Result.MoveValue();
        else OutError = UTF8_TO_TCHAR(Result.error().message());
    });
    Impl->Diagnostics.bPeerConnectionReady = Impl->Peer != nullptr;
    return Impl->Diagnostics.bPeerConnectionReady;
#else
    return false;
#endif
}

FNativeFactoryDiagnostics FNativeRuntime::GetDiagnostics() const
{
    check(IsInGameThread());
    return Impl ? Impl->Diagnostics : FNativeFactoryDiagnostics{};
}

void FNativeRuntime::Stop()
{
    check(IsInGameThread());
    Impl.Reset();
}
}

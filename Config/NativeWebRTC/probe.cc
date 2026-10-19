// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "api/create_peerconnection_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "modules/audio_device/include/audio_device_default.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

class Observer final : public webrtc::PeerConnectionObserver {
 public:
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
  void OnIceCandidate(const webrtc::IceCandidate*) override {}
};
class DummyAudio : public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModule> {};

// Only C scalars cross this experimental boundary. No ownership escapes.
extern "C" __attribute__((visibility("default"))) int SimurghRTC_NativeProbeV1(int media) {
  if (media != 0 && media != 1) return 1;
  if (!webrtc::InitializeSSL()) return 2;
  int result = 0;
  {
    auto network = webrtc::Thread::CreateWithSocketServer();
    auto worker = webrtc::Thread::Create();
    auto signaling = webrtc::Thread::Create();
    if (!network->Start() || !worker->Start() || !signaling->Start()) {
      result = 3;
    } else {
      webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio;
      webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
      webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
      webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer;
      Observer observer;
      if (media) worker->BlockingCall([&] {
        audio = webrtc::make_ref_counted<DummyAudio>();
        apm = webrtc::BuiltinAudioProcessingBuilder().Build(webrtc::CreateEnvironment());
      });
      signaling->BlockingCall([&] {
        if (media) {
          factory = webrtc::CreatePeerConnectionFactory(network.get(), worker.get(), signaling.get(), audio,
            webrtc::CreateBuiltinAudioEncoderFactory(), webrtc::CreateBuiltinAudioDecoderFactory(),
            webrtc::CreateBuiltinVideoEncoderFactory(), webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, apm);
        } else {
          webrtc::PeerConnectionFactoryDependencies deps;
          deps.network_thread = network.get(); deps.worker_thread = worker.get(); deps.signaling_thread = signaling.get();
          factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
        }
        if (!factory) { result = 4; return; }
        webrtc::PeerConnectionFactoryInterface::Options options;
        options.disable_network_monitor = true;
        factory->SetOptions(options);
        webrtc::PeerConnectionInterface::RTCConfiguration config;
        webrtc::PeerConnectionDependencies deps(&observer);
        auto created = factory->CreatePeerConnectionOrError(config, std::move(deps));
        if (!created.ok()) { result = 5; return; }
        peer = created.MoveValue();
        if (media && (factory->GetRtpSenderCapabilities(webrtc::MediaType::AUDIO).codecs.empty()
                   || factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs.empty())) result = 6;
      });
      signaling->BlockingCall([&] { if (peer) peer->Close(); peer = nullptr; factory = nullptr; });
      worker->BlockingCall([&] { apm = nullptr; if (audio) audio->Terminate(); audio = nullptr; });
    }
    signaling->Stop(); worker->Stop(); network->Stop();
  }
  webrtc::CleanupSSL();
  return result;
}

#include "third_party/boringssl/src/include/openssl/crypto.h"
#include "third_party/opus/src/include/opus.h"
#include "third_party/libvpx/source/libvpx/vpx/vpx_codec.h"
extern "C" __attribute__((visibility("default"))) const char* SimurghRTC_NativeVersionV1(int component) {
  if (component == 0) return OpenSSL_version(OPENSSL_VERSION);
  if (component == 1) return opus_get_version_string();
  if (component == 2) return vpx_codec_version_str();
  return nullptr;
}

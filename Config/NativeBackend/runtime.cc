// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "simurgh_rtc_abi.h"
#include "signaling.h"
#include <cstdio>
#include <mutex>
#include <thread>
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
#include "third_party/boringssl/src/include/openssl/crypto.h"
#include "third_party/opus/src/include/opus.h"
#include "third_party/libvpx/source/libvpx/vpx/vpx_codec.h"
namespace {
std::mutex ssl_mutex;
unsigned ssl_users = 0;
int Fail(char* error, uint32_t capacity, const char* text) {
  if (error && capacity) std::snprintf(error, capacity, "%s", text);
  return 1;
}
class DummyAudio : public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModule> {};
}
struct SR_Runtime {
  std::thread::id owner = std::this_thread::get_id();
  bool ssl = false, network_started = false, worker_started = false, signaling_started = false;
  std::unique_ptr<webrtc::Thread> network, worker, signaling;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio;
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
  std::shared_ptr<sr::Signaling> observer = std::make_shared<sr::Signaling>();
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer;
  SR_Diagnostics diagnostics{};
  ~SR_Runtime() {
    if (signaling_started) signaling->BlockingCall([&] {
      observer->Shutdown();
      if (peer) peer->Close();
      peer = nullptr; factory = nullptr;
    });
    if (worker_started) worker->BlockingCall([&] {
      apm = nullptr; if (audio) audio->Terminate(); audio = nullptr;
    });
    if (signaling_started) signaling->Stop();
    if (worker_started) worker->Stop();
    if (network_started) network->Stop();
    signaling.reset(); worker.reset(); network.reset();
    if (ssl) {
      std::lock_guard<std::mutex> guard(ssl_mutex);
      if (--ssl_users == 0) webrtc::CleanupSSL();
    }
  }
};
extern "C" int SimurghRTC_CreateV1(int media, SR_Runtime** out, char* error, uint32_t capacity) {
  if (error && capacity) error[0] = 0;
  if (!out) return Fail(error, capacity, "Missing runtime output.");
  *out = nullptr;
  if (media != 0 && media != 1) return Fail(error, capacity, "Invalid factory mode.");
  auto runtime = std::make_unique<SR_Runtime>();
  auto& r = *runtime;
  {
    std::lock_guard<std::mutex> guard(ssl_mutex);
    if (!ssl_users && !webrtc::InitializeSSL()) return Fail(error, capacity, "SSL initialization failed.");
    ++ssl_users; r.ssl = true;
  }
  r.network = webrtc::Thread::CreateWithSocketServer();
  r.worker = webrtc::Thread::Create(); r.signaling = webrtc::Thread::Create();
  r.network->SetName("SimurghRTC Network", nullptr);
  r.worker->SetName("SimurghRTC Worker", nullptr);
  r.signaling->SetName("SimurghRTC Signaling", nullptr);
  if (!(r.network_started = r.network->Start()) || !(r.worker_started = r.worker->Start())
      || !(r.signaling_started = r.signaling->Start())) return Fail(error, capacity, "Cannot start WebRTC threads.");
  if (media) r.worker->BlockingCall([&] {
    r.audio = webrtc::make_ref_counted<DummyAudio>();
    r.apm = webrtc::BuiltinAudioProcessingBuilder().Build(webrtc::CreateEnvironment());
  });
  if (media && (!r.audio || !r.apm)) return Fail(error, capacity, "Audio device/APM creation failed.");
  r.signaling->BlockingCall([&] {
    if (media) r.factory = webrtc::CreatePeerConnectionFactory(r.network.get(), r.worker.get(), r.signaling.get(), r.audio,
      webrtc::CreateBuiltinAudioEncoderFactory(), webrtc::CreateBuiltinAudioDecoderFactory(),
      webrtc::CreateBuiltinVideoEncoderFactory(), webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, r.apm);
    else {
      webrtc::PeerConnectionFactoryDependencies deps;
      deps.network_thread = r.network.get(); deps.worker_thread = r.worker.get(); deps.signaling_thread = r.signaling.get();
      r.factory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
    }
    if (!r.factory) return;
    webrtc::PeerConnectionFactoryInterface::Options options;
    options.disable_network_monitor = true;
    r.factory->SetOptions(options);
    if (media) {
      std::string audio, video;
      for (const auto& codec : r.factory->GetRtpSenderCapabilities(webrtc::MediaType::AUDIO).codecs) audio += codec.name + ";";
      for (const auto& codec : r.factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs) video += codec.name + ";";
      std::snprintf(r.diagnostics.audio_codecs, sizeof(r.diagnostics.audio_codecs), "%s", audio.c_str());
      std::snprintf(r.diagnostics.video_codecs, sizeof(r.diagnostics.video_codecs), "%s", video.c_str());
    }
  });
  if (!r.factory) return Fail(error, capacity, "PeerConnection factory creation failed.");
  r.diagnostics.size = sizeof(SR_Diagnostics); r.diagnostics.abi_version = SR_ABI_VERSION;
  r.diagnostics.factory_ready = 1; r.diagnostics.dummy_audio = !!r.audio; r.diagnostics.audio_processing = !!r.apm;
  *out = runtime.release(); return 0;
}
extern "C" int SimurghRTC_EnableLoopbackProbeV1(SR_Runtime* r) {
  if (!r || r->owner != std::this_thread::get_id() || !r->factory || r->peer) return 1;
  r->signaling->BlockingCall([&] {
    webrtc::PeerConnectionFactoryInterface::Options options;
    options.disable_network_monitor = true;
    options.network_ignore_mask = ~webrtc::ADAPTER_TYPE_LOOPBACK;
    r->factory->SetOptions(options);
    r->observer->EnableLoopbackProbe();
  });
  return 0;
}
extern "C" int SimurghRTC_CreatePeerV1(SR_Runtime* r, char* error, uint32_t capacity) {
  if (error && capacity) error[0] = 0;
  if (!r || r->owner != std::this_thread::get_id() || !r->factory) return Fail(error, capacity, "Invalid runtime/thread.");
  if (r->peer) return Fail(error, capacity, "Runtime already owns a peer.");
  r->signaling->BlockingCall([&] {
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    webrtc::PeerConnectionDependencies deps(r->observer.get());
    auto created = r->factory->CreatePeerConnectionOrError(config, std::move(deps));
    if (created.ok()) { r->peer = created.MoveValue(); r->observer->Attach(r->peer.get()); }
    else Fail(error, capacity, created.error().message());
  });
  r->diagnostics.peer_ready = !!r->peer;
  return r->peer ? 0 : 1;
}
extern "C" int SimurghRTC_DiagnosticsV1(SR_Runtime* r, SR_Diagnostics* out) {
  if (!r || !out || r->owner != std::this_thread::get_id()
      || out->size != sizeof(*out) || out->abi_version != SR_ABI_VERSION) return 1;
  *out = r->diagnostics; return 0;
}
extern "C" void SimurghRTC_DestroyV1(SR_Runtime* r) { delete r; }
extern "C" int SimurghRTC_SignalV1(SR_Runtime* r, const SR_SignalRequest* request, char* error, uint32_t capacity) {
  if (!r || r->owner != std::this_thread::get_id() || !r->peer || !request
      || request->size != sizeof(*request) || request->abi_version != SR_ABI_VERSION) {
    Fail(error, capacity, "Invalid signaling runtime, thread or ABI."); return SR_INVALID_ARGUMENT;
  }
  return r->signaling->BlockingCall([&] { return r->observer->Submit(*request, error, capacity); });
}
extern "C" int SimurghRTC_PollEventV1(SR_Runtime* r, SR_SignalEvent* event) {
  if (!r || r->owner != std::this_thread::get_id() || !r->peer || !event
      || event->size != sizeof(*event) || event->abi_version != SR_ABI_VERSION) return SR_INVALID_ARGUMENT;
  r->signaling->BlockingCall([&] { r->observer->Poll(*event); }); return SR_OK;
}
extern "C" const char* SimurghRTC_NativeVersionV1(int component) {
  if (component == 0) return OpenSSL_version(OPENSSL_VERSION);
  if (component == 1) return opus_get_version_string();
  if (component == 2) return vpx_codec_version_str();
  if (component == 3) return "M155-5bd86a2f17a35dfc986b87d058f511e45805bd1d";
  return nullptr;
}
extern "C" int SimurghRTC_ChannelV1(SR_Runtime* r, const SR_ChannelRequest* request, SR_ChannelInfo* out) {
  if (!r || r->owner != std::this_thread::get_id() || !r->peer || !request || !out
      || request->size != sizeof(*request) || request->abi_version != SR_ABI_VERSION) return SR_INVALID_ARGUMENT;
  return r->signaling->BlockingCall([&] { return r->observer->Channel(*request, *out); });
}
extern "C" int SimurghRTC_PollDataV1(SR_Runtime* r, SR_DataEvent* event) {
  if (!r || r->owner != std::this_thread::get_id() || !r->peer || !event
      || event->size != sizeof(*event) || event->abi_version != SR_ABI_VERSION) return SR_INVALID_ARGUMENT;
  r->signaling->BlockingCall([&] { r->observer->PollData(*event); }); return SR_OK;
}

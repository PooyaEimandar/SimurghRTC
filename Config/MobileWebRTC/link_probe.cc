// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
// Link acceptance only. Device startup is validated by the packaged Unreal probe.
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

extern "C" __attribute__((visibility("default"))) bool SimurghRTCLinkProbe() {
  auto network = rtc::Thread::CreateWithSocketServer();
  auto worker = rtc::Thread::Create();
  auto signaling = rtc::Thread::Create();
  auto apm = webrtc::AudioProcessingBuilder().Create();
  // Keep the factory, codecs, APM and SSL entry points in the link graph.
  // This function is not a runtime probe: it intentionally does not start threads.
  auto factory = webrtc::CreatePeerConnectionFactory(
      network.get(), worker.get(), signaling.get(), nullptr,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      webrtc::CreateBuiltinVideoEncoderFactory(),
      webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, apm);
  return factory != nullptr && rtc::InitializeSSL() && rtc::CleanupSSL();
}

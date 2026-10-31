// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "simurgh_rtc_abi.h"
#include <array>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <vector>
#include "api/notifier.h"
#include "api/media_stream_interface.h"
#include "common_audio/resampler/include/push_resampler.h"
#include "modules/audio_device/include/audio_device_default.h"

namespace sr {
// One generated source per peer for the first audio transport slice. The
// application clock supplies timestamps; the native clock consumes 10 ms blocks.
class AudioSource : public webrtc::Notifier<webrtc::AudioSourceInterface> {
 public:
  AudioSource(int rate, int channels);
  SourceState state() const override { return kLive; }
  bool remote() const override { return false; }
  const webrtc::AudioOptions options() const override;
  void AddSink(webrtc::AudioTrackSinkInterface* sink) override;
  void RemoveSink(webrtc::AudioTrackSinkInterface* sink) override;
  int Push(const SR_AudioInput& input);
  void Pump();
  void Mute(bool muted);
  void Stop();
  void Snapshot(SR_AudioState& out);
 private:
  const int rate_, channels_;
  std::mutex mutex_;
  bool stopped_ = false, muted_ = false;
  int64_t last_timestamp_ = -1;
  uint64_t admitted_ = 0, consumed_ = 0, underruns_ = 0;
  std::deque<std::array<int16_t, SR_AUDIO_MAX_SAMPLES>> queue_;
  std::vector<webrtc::AudioTrackSinkInterface*> sinks_;
  webrtc::PushResampler<int16_t> resampler_;
};

class AudioReceiver final : public webrtc::AudioTrackSinkInterface {
 public:
  void OnData(const void*, int, int, size_t, size_t, std::optional<int64_t>, const webrtc::RtpPacketInfos&) override;
  void Poll(SR_AudioOutput& out);
  void Snapshot(SR_AudioState& out);
  void Stop();
 private:
  std::mutex mutex_;
  bool stopped_ = false;
  uint64_t received_ = 0, dropped_ = 0;
  std::deque<SR_AudioOutput> queue_;
};

// Drives NetEq without opening hardware. This clock is not Unreal speaker
// playback and must not be advertised as a physical AEC reference.
class AudioDevice : public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModule> {
 public:
  ~AudioDevice() override;
  int32_t RegisterAudioCallback(webrtc::AudioTransport* callback) override;
  int32_t PlayoutIsAvailable(bool* available) override { *available = true; return 0; }
  int32_t RecordingIsAvailable(bool* available) override { *available = false; return 0; }
  int32_t StartPlayout() override;
  int32_t StopPlayout() override;
  bool Playing() const override;
  int32_t Terminate() override;
  bool Start(webrtc::scoped_refptr<AudioSource> source);
  void Stop();
 private:
  void Run();
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false, playing_ = false;
  webrtc::AudioTransport* callback_ = nullptr;
  webrtc::scoped_refptr<AudioSource> source_;
  std::thread thread_;
};
}

// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "audio.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace sr {
AudioSource::AudioSource(int rate, int channels) : rate_(rate), channels_(channels), resampler_(rate / 100, 480, channels) {}
const webrtc::AudioOptions AudioSource::options() const {
  webrtc::AudioOptions value;
  value.echo_cancellation = false; value.noise_suppression = false; value.auto_gain_control = false;
  return value;
}
void AudioSource::AddSink(webrtc::AudioTrackSinkInterface* sink) {
  std::lock_guard lock(mutex_);
  if (!stopped_ && std::find(sinks_.begin(), sinks_.end(), sink) == sinks_.end()) sinks_.push_back(sink);
}
void AudioSource::RemoveSink(webrtc::AudioTrackSinkInterface* sink) {
  std::lock_guard lock(mutex_); std::erase(sinks_, sink);
}
int AudioSource::Push(const SR_AudioInput& input) {
  if (!input.samples || input.sample_count != static_cast<uint32_t>(rate_ / 100 * channels_) || input.timestamp_us < 0)
    return SR_INVALID_ARGUMENT;
  std::array<int16_t, SR_AUDIO_MAX_SAMPLES> block{};
  for (uint32_t i = 0; i < input.sample_count; ++i) {
    const float value = input.samples[i];
    if (!std::isfinite(value) || value < -1 || value > 1) return SR_INVALID_ARGUMENT;
    block[i] = static_cast<int16_t>(std::clamp(std::lround(value * 32768.f), -32768l, 32767l));
  }
  std::lock_guard lock(mutex_);
  if (stopped_) return SR_INVALID_STATE;
  if (input.timestamp_us <= last_timestamp_) return SR_INVALID_ARGUMENT;
  if (queue_.size() >= SR_AUDIO_QUEUE_BLOCKS) return SR_RESOURCE_EXHAUSTED;
  queue_.push_back(std::move(block)); last_timestamp_ = input.timestamp_us; ++admitted_;
  return SR_OK;
}
void AudioSource::Pump() {
  std::lock_guard lock(mutex_);
  if (stopped_ || sinks_.empty()) return;
  std::array<int16_t, SR_AUDIO_MAX_SAMPLES> input{}, output{};
  if (!queue_.empty()) { input = queue_.front(); queue_.pop_front(); ++consumed_; }
  else ++underruns_;
  resampler_.Resample(webrtc::InterleavedView<const int16_t>(input.data(), rate_ / 100, channels_),
                     webrtc::InterleavedView<int16_t>(output.data(), 480, channels_));
  if (muted_) output.fill(0);
  for (auto* sink : sinks_) sink->OnData(output.data(), 16, 48000, channels_, 480, std::nullopt);
}
void AudioSource::Mute(bool muted) { std::lock_guard lock(mutex_); muted_ = muted; queue_.clear(); }
void AudioSource::Stop() { std::lock_guard lock(mutex_); stopped_ = true; queue_.clear(); sinks_.clear(); }
void AudioSource::Snapshot(SR_AudioState& out) {
  std::lock_guard lock(mutex_);
  out.queued_input_blocks = queue_.size(); out.admitted_blocks = admitted_; out.consumed_blocks = consumed_;
  out.underrun_blocks = underruns_; out.muted = muted_;
}
void AudioReceiver::OnData(const void* data, int bits, int rate, size_t channels, size_t frames,
                          std::optional<int64_t>, const webrtc::RtpPacketInfos&) {
  std::lock_guard lock(mutex_);
  if (stopped_) return;
  if (!data || bits != 16 || rate <= 0 || (channels != 1 && channels != 2)
      || frames != static_cast<size_t>(rate / 100) || frames * channels > SR_AUDIO_MAX_SAMPLES) { ++dropped_; return; }
  SR_AudioOutput out{}; out.size = sizeof(out); out.abi_version = SR_ABI_VERSION;
  out.sample_rate = rate; out.channels = channels; out.sample_count = frames * channels;
  // Receiver-local monotonically increasing sample time, not remote capture time.
  out.timestamp_us = received_++ * 10000;
  const auto* pcm = static_cast<const int16_t*>(data);
  for (uint32_t i = 0; i < out.sample_count; ++i) out.samples[i] = pcm[i] / 32768.f;
  if (queue_.size() == SR_AUDIO_QUEUE_BLOCKS) { queue_.pop_front(); ++dropped_; }
  queue_.push_back(std::move(out));
}
void AudioReceiver::Poll(SR_AudioOutput& out) {
  std::lock_guard lock(mutex_); out = {}; out.size = sizeof(out); out.abi_version = SR_ABI_VERSION;
  if (!queue_.empty()) { out = queue_.front(); queue_.pop_front(); }
}
void AudioReceiver::Snapshot(SR_AudioState& out) {
  std::lock_guard lock(mutex_);
  out.queued_output_blocks = queue_.size(); out.received_blocks = received_; out.dropped_output_blocks = dropped_;
}
void AudioReceiver::Stop() { std::lock_guard lock(mutex_); stopped_ = true; queue_.clear(); }

AudioDevice::~AudioDevice() { Stop(); }
int32_t AudioDevice::RegisterAudioCallback(webrtc::AudioTransport* callback) {
  std::lock_guard lock(mutex_); callback_ = callback; return 0;
}
int32_t AudioDevice::StartPlayout() { std::lock_guard lock(mutex_); playing_ = true; return 0; }
int32_t AudioDevice::StopPlayout() { std::lock_guard lock(mutex_); playing_ = false; return 0; }
bool AudioDevice::Playing() const { std::lock_guard lock(mutex_); return playing_; }
int32_t AudioDevice::Terminate() { Stop(); return 0; }
bool AudioDevice::Start(webrtc::scoped_refptr<AudioSource> source) {
  std::lock_guard lock(mutex_);
  if (thread_.joinable() || stopping_) return false;
  source_ = std::move(source); thread_ = std::thread([this] { Run(); }); return true;
}
void AudioDevice::Stop() {
  { std::lock_guard lock(mutex_); stopping_ = true; wake_.notify_all(); }
  if (thread_.joinable()) thread_.join();
  source_ = nullptr;
}
void AudioDevice::Run() {
  using Clock = std::chrono::steady_clock;
  auto next = Clock::now();
  std::unique_lock lock(mutex_);
  while (!stopping_) {
    // The source never calls back into this device. Registration/teardown waits
    // for the in-flight callback before releasing AudioTransport.
    if (source_) source_->Pump();
    if (playing_ && callback_) {
      int16_t samples[960]{}; size_t count = 0; int64_t elapsed = 0, ntp = 0;
      callback_->NeedMorePlayData(480, sizeof(int16_t), 2, 48000, samples, count, &elapsed, &ntp);
    }
    next += std::chrono::milliseconds(10);
    // Do not emit a catch-up burst after suspension or scheduling starvation.
    if (next < Clock::now()) next = Clock::now() + std::chrono::milliseconds(10);
    wake_.wait_until(lock, next, [this] { return stopping_; });
  }
}
}

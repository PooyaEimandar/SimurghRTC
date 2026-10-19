// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "simurgh_rtc_abi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"
#include "rtc_base/buffer.h"
#include "rtc_base/thread.h"
#include "third_party/libyuv/include/libyuv.h"
namespace {
class DecodedSink final : public webrtc::DecodedImageCallback {
 public:
  DecodedSink(const SR_MediaInput& in, SR_MediaOutput& out) : in(in), out(out) {}
  const SR_MediaInput& in; SR_MediaOutput& out; bool valid = true;
  int32_t Decoded(webrtc::VideoFrame& frame) override {
    int index = out.decoded_frames;
    if (frame.width() != 64 || frame.height() != 64 || index >= 12
        || frame.rtp_timestamp() != uint32_t(90000 + index * 3000)) { valid = false; return -1; }
    auto i420 = frame.video_frame_buffer()->ToI420();
    if (!i420 || libyuv::I420ToARGB(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
        i420->DataV(), i420->StrideV(), out.bgra, 64 * 4, 64, 64)) { valid = false; return -1; }
    for (int y : {16, 48}) for (int x : {16, 48}) for (int c = 0; c < 3; ++c) {
      int offset = (y * 64 + x) * 4 + c;
      out.max_color_error = std::max(out.max_color_error,
        std::abs(int(out.bgra[offset]) - int(in.bgra_frames[index * SR_VIDEO_BYTES + offset])));
    }
    ++out.decoded_frames; return 0;
  }
};
class EncodedSink final : public webrtc::EncodedImageCallback {
 public:
  EncodedSink(SR_MediaOutput& out, webrtc::VideoDecoder& decoder) : out(out), decoder(decoder) {}
  SR_MediaOutput& out; webrtc::VideoDecoder& decoder; bool valid = true;
  void OnFrameDropped(uint32_t, int, bool) override { valid = false; }
  Result OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo*) override {
    if (!image.size() || image.size() > 65536 || out.encoded_frames >= 12) {
      valid = false; return Result(Result::ERROR_SEND_FAILED);
    }
    ++out.encoded_frames;
    out.max_video_packet_bytes = std::max(out.max_video_packet_bytes, int(image.size()));
    if (decoder.Decode(image, false, 0)) valid = false;
    return Result(valid ? Result::OK : Result::ERROR_SEND_FAILED);
  }
};
bool Video(const SR_MediaInput& in, SR_MediaOutput& out, std::string& reason) {
  auto fail = [&](const char* text) { reason = text; return false; };
  auto env = webrtc::CreateEnvironment();
  auto encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
  auto decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();
  auto encoder = encoder_factory->Create(env, webrtc::SdpVideoFormat("VP8"));
  auto decoder = decoder_factory->Create(env, webrtc::SdpVideoFormat("VP8"));
  if (!encoder || !decoder) return fail("VP8 factory returned null.");
  DecodedSink decoded(in, out); EncodedSink encoded(out, *decoder);
  // Release codecs before callback objects leave scope, including every failure path.
  struct Release {
    webrtc::VideoEncoder& encoder; webrtc::VideoDecoder& decoder;
    ~Release() { encoder.Release(); decoder.Release(); }
  } release{*encoder, *decoder};
  webrtc::VideoDecoder::Settings ds;
  ds.set_codec_type(webrtc::kVideoCodecVP8); ds.set_number_of_cores(1);
  ds.set_max_render_resolution({64, 64}); ds.set_buffer_pool_size(2);
  if (!decoder->Configure(ds) || decoder->RegisterDecodeCompleteCallback(&decoded)) return fail("VP8 decoder configuration failed.");
  webrtc::VideoCodec config;
  config.codecType = webrtc::kVideoCodecVP8; config.width = 64; config.height = 64;
  config.startBitrate = 300; config.minBitrate = 100; config.maxBitrate = 1000;
  config.maxFramerate = 30; config.active = true; config.qpMax = 56;
  config.mode = webrtc::VideoCodecMode::kRealtimeVideo;
  *config.VP8() = webrtc::VideoEncoder::GetDefaultVp8Settings();
  config.VP8()->denoisingOn = false; config.SetFrameDropEnabled(false);
  const webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200);
  if (encoder->InitEncode(&config, settings) || encoder->RegisterEncodeCompleteCallback(&encoded)) return fail("VP8 encoder configuration failed.");
  // M155 only reports the selected encoder after InitEncode.
  std::snprintf(out.video_encoder, sizeof(out.video_encoder), "%s", encoder->GetEncoderInfo().implementation_name.c_str());
  std::snprintf(out.video_decoder, sizeof(out.video_decoder), "%s", decoder->GetDecoderInfo().implementation_name.c_str());
  if (encoder->GetEncoderInfo().is_hardware_accelerated || decoder->GetDecoderInfo().is_hardware_accelerated) return fail("VP8 unexpectedly uses hardware.");
  webrtc::VideoBitrateAllocation allocation; allocation.SetBitrate(0, 0, 300000);
  encoder->SetRates(webrtc::VideoEncoder::RateControlParameters(allocation, 30.0));
  for (int index = 0; index < 12; ++index) {
    auto i420 = webrtc::I420Buffer::Create(64, 64);
    if (libyuv::ARGBToI420(in.bgra_frames + index * SR_VIDEO_BYTES, 64 * 4,
        i420->MutableDataY(), i420->StrideY(), i420->MutableDataU(), i420->StrideU(),
        i420->MutableDataV(), i420->StrideV(), 64, 64)) return fail("VP8 color conversion failed.");
    auto frame = webrtc::VideoFrame::Builder().set_video_frame_buffer(i420)
      .set_rtp_timestamp(90000 + index * 3000).set_timestamp_us(1000000 + index * 33333).build();
    std::vector<webrtc::VideoFrameType> types{index == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta};
    int status = encoder->Encode(frame, &types);
    if (status || !encoded.valid || !decoded.valid || out.decoded_frames != index + 1) {
      reason = "VP8 callback validation: status=" + std::to_string(status) + " encoded=" + std::to_string(out.encoded_frames) + " decoded=" + std::to_string(out.decoded_frames); return false;
    }
  }
  return out.max_color_error <= 12 || fail("VP8 decoded color mismatch.");
}
bool Audio(const SR_MediaInput& in, SR_MediaOutput& out) {
  const auto env = webrtc::CreateEnvironment();
  const webrtc::SdpAudioFormat format("opus", 48000, 2, {{"stereo", "1"}, {"ptime", "20"}});
  webrtc::AudioEncoderFactory::Options options; options.payload_type = 111;
  auto encoder = webrtc::CreateBuiltinAudioEncoderFactory()->Create(env, format, options);
  auto decoder = webrtc::CreateBuiltinAudioDecoderFactory()->Create(env, format);
  if (!encoder || !decoder || encoder->SampleRateHz() != 48000 || encoder->NumChannels() != 2 || decoder->Channels() != 2) return false;
  out.channels = 2;
  int16_t decoded[5760 * 2];
  for (int chunk = 0; chunk < 100; ++chunk) {
    webrtc::Buffer packet;
    const auto info = encoder->Encode(chunk * 480, std::span<const int16_t>(in.pcm + chunk * 960, 960), &packet);
    if (!info.encoded_bytes) continue;
    if (packet.size() != info.encoded_bytes || packet.size() > 4096) return false;
    webrtc::AudioDecoder::SpeechType type;
    const int count = decoder->Decode(packet.data(), packet.size(), 48000, sizeof(decoded), decoded, &type);
    if (count <= 0 || count > 11520 || count % 2 || out.pcm_samples + count > SR_PCM_SAMPLES) return false;
    std::memcpy(out.pcm + out.pcm_samples, decoded, count * sizeof(int16_t)); out.pcm_samples += count;
    ++out.audio_packets; out.max_audio_packet_bytes = std::max(out.max_audio_packet_bytes, int(packet.size()));
  }
  if (out.pcm_samples != SR_PCM_SAMPLES || out.audio_packets != 50) return false;
  double energy = 0, sin_sum = 0, cos_sum = 0;
  for (int n = 4800; n < 48000; ++n) {
    double value = out.pcm[n * 2], phase = 2.0 * 3.14159265358979323846 * 440.0 * n / 48000.0;
    energy += value * value; sin_sum += value * std::sin(phase); cos_sum += value * std::cos(phase);
  }
  out.audio_rms = std::sqrt(energy / 43200);
  out.tone_correlation = energy > 0 ? std::sqrt(2.0 * (sin_sum * sin_sum + cos_sum * cos_sum) / (43200 * energy)) : 0;
  return out.audio_rms >= 3000 && out.audio_rms <= 9000 && out.tone_correlation >= 0.95;
}
}
extern "C" int SimurghRTC_MediaProbeV1(const SR_MediaInput* in, SR_MediaOutput* out, char* error, uint32_t capacity) {
  if (error && capacity) error[0] = 0;
  auto fail = [&](const char* message) {
    if (error && capacity) std::snprintf(error, capacity, "%s", message);
    return 1;
  };
  if (!in || !out || in->size != sizeof(*in) || in->abi_version != SR_ABI_VERSION
      || out->size != sizeof(*out) || out->abi_version != SR_ABI_VERSION
      || !in->bgra_frames || in->bgra_bytes != 12 * SR_VIDEO_BYTES
      || !in->pcm || in->pcm_samples != SR_PCM_SAMPLES) return fail("Invalid media fixture buffers or ABI.");
  *out = {}; out->size = sizeof(*out); out->abi_version = SR_ABI_VERSION;
  auto worker = webrtc::Thread::Create(); worker->SetName("SimurghRTC Codec Probe", nullptr);
  if (!worker->Start()) return fail("Cannot start codec worker.");
  bool video = false, audio = false;
  std::string reason;
  worker->BlockingCall([&] { video = Video(*in, *out, reason); if (video) audio = Audio(*in, *out); });
  worker->Stop();
  return video && audio ? 0 : fail(video ? "Opus fixture validation failed." : reason.c_str());
}

// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "SimurghRTCNativeMediaProbe.h"
#include "Misc/ScopeExit.h"
#include "Fixtures/SimurghRTCMediaFixtureV1.h"

#if SIMURGHRTC_WITH_ENGINE_WEBRTC
THIRD_PARTY_INCLUDES_START
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"
#include "rtc_base/buffer.h"
#include "rtc_base/thread.h"
#include "third_party/libyuv/include/libyuv.h"
THIRD_PARTY_INCLUDES_END
#endif

namespace SimurghRTC
{
#if SIMURGHRTC_WITH_ENGINE_WEBRTC
namespace
{
using FResult = FNativeMediaProbeResult;
constexpr int32 PixelCount = FResult::Width * FResult::Height;
static_assert(FResult::Width == FixtureV1::Width && FResult::Height == FixtureV1::Height
    && FResult::FrameCount == FixtureV1::FrameCount && FResult::SampleRate == FixtureV1::SampleRate);

// Quadrants detect orientation/channel mistakes; changing brightness detects stale frames.
FColor Pattern(int32 X, int32 Y, int32 Frame)
{
    return FixtureV1::VideoPixel(X, Y, Frame);
}

class FDecodedSink final : public webrtc::DecodedImageCallback
{
public:
    explicit FDecodedSink(FResult& InOut) : Out(InOut) {}
    FResult& Out;
    bool bValid = true;
    int32_t Decoded(webrtc::VideoFrame& Frame) override
    {
        // This probe pins the synchronous software VP8 implementation, on one codec worker.
        const int32 Index = Out.DecodedFrames;
        if (Frame.width() != FResult::Width || Frame.height() != FResult::Height
            || Index >= FResult::FrameCount || Frame.timestamp() != uint32(90000 + Index * 3000))
        { bValid = false; return -1; }
        auto I420 = Frame.video_frame_buffer()->ToI420();
        Out.DecodedBGRA.SetNumUninitialized(PixelCount);
        if (!I420 || libyuv::I420ToARGB(I420->DataY(), I420->StrideY(), I420->DataU(), I420->StrideU(),
            I420->DataV(), I420->StrideV(), reinterpret_cast<uint8*>(Out.DecodedBGRA.GetData()),
            FResult::Width * 4, FResult::Width, FResult::Height) != 0)
        { bValid = false; return -1; }
        for (int32 Y : {16, 48}) for (int32 X : {16, 48})
        {
            const FColor Want = Pattern(X, Y, Index);
            const FColor Got = Out.DecodedBGRA[Y * FResult::Width + X];
            Out.MaxColorError = FMath::Max(Out.MaxColorError, FMath::Max3(
                FMath::Abs(int32(Got.R) - Want.R), FMath::Abs(int32(Got.G) - Want.G), FMath::Abs(int32(Got.B) - Want.B)));
        }
        ++Out.DecodedFrames;
        return 0;
    }
};

class FEncodedSink final : public webrtc::EncodedImageCallback
{
public:
    FEncodedSink(FResult& InOut, webrtc::VideoDecoder& InDecoder) : Out(InOut), Decoder(InDecoder) {}
    FResult& Out;
    webrtc::VideoDecoder& Decoder;
    bool bValid = true;
    Result OnEncodedImage(const webrtc::EncodedImage& Image, const webrtc::CodecSpecificInfo*) override
    {
        if (Image.size() == 0 || Image.size() > 65536 || Out.EncodedFrames >= FResult::FrameCount)
        { bValid = false; return Result(Result::ERROR_SEND_FAILED); }
        ++Out.EncodedFrames;
        Out.MaxVideoPacketBytes = FMath::Max(Out.MaxVideoPacketBytes, int32(Image.size()));
        if (Decoder.Decode(Image, false, 0) != 0) bValid = false;
        return Result(bValid ? Result::OK : Result::ERROR_SEND_FAILED);
    }
};

bool ProbeVideo(FResult& Out, FString& Error)
{
    auto EncoderFactory = webrtc::CreateBuiltinVideoEncoderFactory();
    auto DecoderFactory = webrtc::CreateBuiltinVideoDecoderFactory();
    auto Encoder = EncoderFactory->CreateVideoEncoder(webrtc::SdpVideoFormat("VP8"));
    auto Decoder = DecoderFactory->CreateVideoDecoder(webrtc::SdpVideoFormat("VP8"));
    if (!Encoder || !Decoder) { Error = TEXT("VP8 encoder/decoder unavailable."); return false; }
    FDecodedSink Decoded(Out);
    FEncodedSink Encoded(Out, *Decoder);
    ON_SCOPE_EXIT { Encoder->Release(); Decoder->Release(); }; // Drain before callbacks die.
    Out.VideoEncoder = UTF8_TO_TCHAR(Encoder->GetEncoderInfo().implementation_name.c_str());
    Out.VideoDecoder = UTF8_TO_TCHAR(Decoder->GetDecoderInfo().implementation_name.c_str());
    if (Encoder->GetEncoderInfo().is_hardware_accelerated || Decoder->GetDecoderInfo().is_hardware_accelerated)
    { Error = TEXT("Fixture requires synchronous software VP8."); return false; }
    webrtc::VideoDecoder::Settings DecoderSettings;
    DecoderSettings.set_codec_type(webrtc::kVideoCodecVP8);
    DecoderSettings.set_number_of_cores(1);
    DecoderSettings.set_max_render_resolution({FResult::Width, FResult::Height});
    DecoderSettings.set_buffer_pool_size(2);
    if (!Decoder->Configure(DecoderSettings) || Decoder->RegisterDecodeCompleteCallback(&Decoded) != 0)
    { Error = TEXT("VP8 decoder configuration failed."); return false; }
    webrtc::VideoCodec Config;
    Config.codecType = webrtc::kVideoCodecVP8;
    Config.width = FResult::Width;
    Config.height = FResult::Height;
    Config.startBitrate = 300;
    Config.minBitrate = 100;
    Config.maxBitrate = 1000;
    Config.maxFramerate = 30;
    Config.active = true;
    Config.qpMax = 56;
    Config.mode = webrtc::VideoCodecMode::kRealtimeVideo;
    *Config.VP8() = webrtc::VideoEncoder::GetDefaultVp8Settings();
    Config.VP8()->denoisingOn = false;
    Config.SetFrameDropEnabled(false);
    const webrtc::VideoEncoder::Settings Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200);
    if (Encoder->InitEncode(&Config, Settings) != 0 || Encoder->RegisterEncodeCompleteCallback(&Encoded) != 0)
    { Error = TEXT("VP8 encoder configuration failed."); return false; }
    webrtc::VideoBitrateAllocation Allocation;
    Allocation.SetBitrate(0, 0, 300000);
    Encoder->SetRates(webrtc::VideoEncoder::RateControlParameters(Allocation, 30.0));
    TArray<FColor> Pixels;
    Pixels.SetNumUninitialized(PixelCount);
    for (int32 Index = 0; Index < FResult::FrameCount; ++Index)
    {
        for (int32 Y = 0; Y < FResult::Height; ++Y) for (int32 X = 0; X < FResult::Width; ++X)
            Pixels[Y * FResult::Width + X] = Pattern(X, Y, Index);
        auto I420 = webrtc::I420Buffer::Create(FResult::Width, FResult::Height);
        if (libyuv::ARGBToI420(reinterpret_cast<const uint8*>(Pixels.GetData()), FResult::Width * 4,
            I420->MutableDataY(), I420->StrideY(), I420->MutableDataU(), I420->StrideU(),
            I420->MutableDataV(), I420->StrideV(), FResult::Width, FResult::Height) != 0)
        { Error = TEXT("BGRA to I420 conversion failed."); return false; }
        auto Frame = webrtc::VideoFrame::Builder().set_video_frame_buffer(I420)
            .set_timestamp_rtp(90000 + Index * 3000).set_timestamp_us(1000000 + Index * 33333).build();
        std::vector<webrtc::VideoFrameType> Types{Index == 0 ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta};
        if (Encoder->Encode(Frame, &Types) != 0 || !Encoded.bValid || !Decoded.bValid || Out.DecodedFrames != Index + 1)
        { Error = TEXT("VP8 encode/decode callback, dimensions or timestamp validation failed."); return false; }
    }
    if (Out.MaxColorError > 12) { Error = TEXT("Decoded video color/orientation mismatch."); return false; }
    return true;
}

bool ProbeAudio(FResult& Out, FString& Error)
{
    const webrtc::SdpAudioFormat Format("opus", 48000, 2, {{"stereo", "1"}, {"ptime", "20"}});
    auto Encoder = webrtc::CreateBuiltinAudioEncoderFactory()->MakeAudioEncoder(111, Format, absl::nullopt);
    auto Decoder = webrtc::CreateBuiltinAudioDecoderFactory()->MakeAudioDecoder(Format, absl::nullopt);
    if (!Encoder || !Decoder || Encoder->SampleRateHz() != 48000 || Encoder->NumChannels() != 2 || Decoder->Channels() != 2)
    { Error = TEXT("48 kHz stereo Opus unavailable."); return false; }
    Out.Channels = 2;
    int16 Input[480 * 2];
    int16 Decoded[5760 * 2]; // Opus maximum 120 ms, with explicit decoder byte capacity.
    for (int32 Chunk = 0; Chunk < 100; ++Chunk)
    {
        for (int32 N = 0; N < 480; ++N)
        {
            const int16 Value = FixtureV1::AudioSample(Chunk * 480 + N);
            Input[N * 2] = Input[N * 2 + 1] = Value;
        }
        rtc::Buffer Packet;
        const auto Info = Encoder->Encode(Chunk * 480, rtc::ArrayView<const int16_t>(Input, 960), &Packet);
        if (Info.encoded_bytes == 0) continue;
        if (Packet.size() != Info.encoded_bytes || Packet.size() > 4096)
        { Error = TEXT("Invalid or oversized Opus packet."); return false; }
        webrtc::AudioDecoder::SpeechType Type;
        const int32 Count = Decoder->Decode(Packet.data(), Packet.size(), 48000, sizeof(Decoded), Decoded, &Type);
        if (Count <= 0 || Count > 11520 || Count % 2 != 0 || Out.DecodedPCM.Num() + Count > 96000)
        { Error = TEXT("Opus decoded PCM violates fixture bounds."); return false; }
        Out.DecodedPCM.Append(Decoded, Count);
        ++Out.AudioPackets;
        Out.MaxAudioPacketBytes = FMath::Max(Out.MaxAudioPacketBytes, int32(Packet.size()));
    }
    if (Out.DecodedPCM.Num() != 96000 || Out.AudioPackets != 50)
    { Error = TEXT("Expected one second of decoded PCM in 50 Opus packets."); return false; }
    // Phase-independent correlation rejects silence/noise while allowing codec delay.
    double Energy = 0, SinSum = 0, CosSum = 0;
    constexpr int32 Start = 4800, Count = 43200;
    for (int32 N = Start; N < 48000; ++N)
    {
        const double Value = Out.DecodedPCM[N * 2];
        const double Phase = 2.0 * UE_DOUBLE_PI * 440.0 * N / 48000.0;
        Energy += Value * Value;
        SinSum += Value * FMath::Sin(Phase);
        CosSum += Value * FMath::Cos(Phase);
    }
    Out.AudioRms = FMath::Sqrt(Energy / Count);
    Out.ToneCorrelation = Energy > 0 ? FMath::Sqrt(2.0 * (SinSum * SinSum + CosSum * CosSum) / (Count * Energy)) : 0;
    if (Out.AudioRms < 3000 || Out.AudioRms > 9000 || Out.ToneCorrelation < 0.95)
    { Error = TEXT("Decoded Opus PCM does not reproduce the generated 440 Hz tone."); return false; }
    return true;
}
}
#endif

bool RunNativeMediaProbe(FNativeMediaProbeResult& Out, FString& OutError)
{
    check(IsInGameThread());
    Out = FNativeMediaProbeResult{};
    OutError.Reset();
#if SIMURGHRTC_WITH_ENGINE_WEBRTC
    auto Worker = rtc::Thread::Create();
    Worker->SetName("SimurghRTC Codec Probe", nullptr);
    if (!Worker->Start()) { OutError = TEXT("Cannot start codec worker."); return false; }
    bool bPassed = false;
    Worker->BlockingCall([&] { bPassed = ProbeVideo(Out, OutError) && ProbeAudio(Out, OutError); });
    Worker->Stop();
    return bPassed;
#else
    OutError = TEXT("Native media probe requires a linked desktop libwebrtc adapter.");
    return false;
#endif
}
}

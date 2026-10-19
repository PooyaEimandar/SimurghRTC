// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#ifndef SIMURGH_RTC_ABI_H
#define SIMURGH_RTC_ABI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Private ABI v1. All buffers belong to the caller and are used only during the call.
// A handle must be used serially and destroyed on its creating thread. No WebRTC,
// STL, Unreal types, exceptions or cross-library deallocation cross this boundary.
#define SR_NATIVE_EXPORT __attribute__((visibility("default")))
#define SR_ABI_VERSION 1
#define SR_VIDEO_BYTES (64 * 64 * 4)
#define SR_PCM_SAMPLES 96000
struct SR_Runtime;
typedef struct SR_Diagnostics {
    uint32_t size, abi_version;
    int32_t factory_ready, peer_ready, dummy_audio, audio_processing;
    char audio_codecs[512], video_codecs[512]; // Semicolon-separated UTF-8 names.
} SR_Diagnostics;
typedef struct SR_MediaInput {
    uint32_t size, abi_version;
    const uint8_t* bgra_frames; // Exactly 12 tightly packed 64x64 BGRA frames.
    uint32_t bgra_bytes;
    const int16_t* pcm; // Exactly one second of 48 kHz interleaved stereo PCM.
    uint32_t pcm_samples;
} SR_MediaInput;
typedef struct SR_MediaOutput {
    uint32_t size, abi_version;
    int32_t encoded_frames, decoded_frames, audio_packets, channels;
    int32_t max_video_packet_bytes, max_audio_packet_bytes, max_color_error;
    uint32_t pcm_samples;
    double tone_correlation, audio_rms;
    char video_encoder[128], video_decoder[128];
    uint8_t bgra[SR_VIDEO_BYTES];
    int16_t pcm[SR_PCM_SAMPLES];
} SR_MediaOutput;
// Return zero on success. Error text, when provided, is UTF-8 and NUL-terminated.
SR_NATIVE_EXPORT int SimurghRTC_CreateV1(int media, struct SR_Runtime** out, char* error, uint32_t capacity);
SR_NATIVE_EXPORT int SimurghRTC_CreatePeerV1(struct SR_Runtime* runtime, char* error, uint32_t capacity);
SR_NATIVE_EXPORT int SimurghRTC_DiagnosticsV1(struct SR_Runtime* runtime, SR_Diagnostics* out);
SR_NATIVE_EXPORT void SimurghRTC_DestroyV1(struct SR_Runtime* runtime);
SR_NATIVE_EXPORT int SimurghRTC_MediaProbeV1(const SR_MediaInput* input, SR_MediaOutput* out, char* error, uint32_t capacity);
SR_NATIVE_EXPORT const char* SimurghRTC_NativeVersionV1(int component); // Static text; caller must not free.
#ifdef __cplusplus
}
#endif
#endif

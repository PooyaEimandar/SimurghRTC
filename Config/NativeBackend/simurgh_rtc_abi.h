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

// Additive signaling API. Requests copy their inputs before returning. Completion
// and transport events are polled on the runtime's owner thread, never callbacks
// into the consumer. Only initial offer/answer negotiation is supported.
#define SR_MAX_SDP_BYTES 131072
#define SR_MAX_ICE_BYTES 2048
#define SR_MAX_MID_BYTES 32
#define SR_MAX_UFRAG_BYTES 256
#define SR_MAX_REMOTE_ICE 32 // Includes an optional end-of-candidates marker.
#define SR_MAX_SIGNAL_EVENTS 64
enum SR_SignalAction { SR_CREATE_OFFER = 1, SR_CREATE_ANSWER, SR_SET_REMOTE, SR_ADD_ICE, SR_END_ICE };
enum SR_SdpType { SR_SDP_NONE = 0, SR_SDP_OFFER = 1, SR_SDP_ANSWER = 2 };
enum SR_SignalError { SR_OK = 0, SR_INVALID_ARGUMENT, SR_INVALID_STATE, SR_RESOURCE_EXHAUSTED, SR_TIMED_OUT, SR_NATIVE_ERROR };
enum SR_EventType { SR_EVENT_NONE = 0, SR_OPERATION_COMPLETE, SR_LOCAL_ICE, SR_LOCAL_ICE_COMPLETE,
  SR_CONNECTION_CHANGED, SR_REMOTE_ICE_APPLIED, SR_DATA_TRANSPORT_CHANGED, SR_FATAL_ERROR };
enum SR_ConnectionState { SR_CONNECTION_NEW = 0, SR_CONNECTION_CONNECTING, SR_CONNECTION_CONNECTED,
  SR_CONNECTION_DISCONNECTED, SR_CONNECTION_FAILED, SR_CONNECTION_CLOSED };
typedef struct SR_SignalRequest {
  uint32_t size, abi_version;
  uint64_t operation_id; // Nonzero, monotonically increasing per runtime.
  int32_t action, sdp_type, sdp_mline_index;
  uint32_t timeout_ms; // 1..300000, including asynchronous native work.
  const char* text; uint32_t text_bytes;
  const char* sdp_mid; uint32_t sdp_mid_bytes;
  const char* username_fragment; uint32_t username_fragment_bytes;
} SR_SignalRequest;
typedef struct SR_TransportState {
  int32_t connection, local_description_set, remote_description_set;
  int32_t local_ice_complete, remote_ice_complete, data_transport_open;
  uint32_t remote_ice_queued, remote_ice_applied;
} SR_TransportState;
typedef struct SR_SignalEvent {
  uint32_t size, abi_version;
  uint64_t operation_id;
  int32_t type, error_code, sdp_type, sdp_mline_index, queued;
  SR_TransportState transport;
  char error[1024];
  char text[SR_MAX_SDP_BYTES + 1]; // SDP or a candidate, depending on type.
  char sdp_mid[SR_MAX_MID_BYTES + 1], username_fragment[SR_MAX_UFRAG_BYTES + 1];
} SR_SignalEvent;
// Zero accepts a request; nonzero is SR_SignalError and produces no completion.
// Accepted ICE before a remote description completes with queued=1. Later apply
// failures become SR_FATAL_ERROR; success is reflected in remote_ice_applied.
SR_NATIVE_EXPORT int SimurghRTC_SignalV1(struct SR_Runtime* runtime, const SR_SignalRequest* request, char* error, uint32_t capacity);
// Zero succeeds; event.type == SR_EVENT_NONE means the queue is empty. Poll at
// least every 10ms during negotiation to enforce deadlines and drain bounded events.
SR_NATIVE_EXPORT int SimurghRTC_PollEventV1(struct SR_Runtime* runtime, SR_SignalEvent* event);
#ifdef __cplusplus
}
#endif
#endif

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
// Explicit local-test mode, enabled only before peer creation. Normal runtimes use network interfaces.
SR_NATIVE_EXPORT int SimurghRTC_EnableLoopbackProbeV1(struct SR_Runtime* runtime);
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
  // Redacted diagnostics. ICE: new/checking/connected/completed/failed/disconnected/closed = 0..6.
  // DTLS: unknown/new/connecting/connected/closed/failed = 0..5. Protocol: none/UDP/TCP/other = 0..3.
  int32_t ice_state, dtls_state, selected_protocol;
  uint32_t local_udp_candidates, local_tcp_candidates, candidate_errors, last_candidate_error;
  uint32_t stats_samples, stats_age_ms, candidate_pairs, succeeded_pairs, failed_pairs;
  uint64_t stun_requests_sent, stun_responses_received, packets_discarded_on_send;
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

// Additive application DataChannel API. IDs are local to one runtime, never SCTP SIDs.
// Text is length-delimited UTF-8 (embedded NUL is allowed); binary is opaque.
// Commands copy accepted input. Send success means local admission, not delivery.
#define SR_MAX_CHANNELS 16
#define SR_MAX_CHANNEL_RECORDS 64
#define SR_MAX_CHANNEL_LABEL_BYTES 65535
#define SR_MAX_MESSAGE_BYTES 65536
#define SR_MAX_CHANNEL_BUFFER_BYTES 262144
#define SR_MAX_DATA_EVENTS 64
#define SR_MAX_DATA_EVENT_BYTES 1048576
enum SR_ChannelAction { SR_CHANNEL_OPEN = 1, SR_CHANNEL_SEND, SR_CHANNEL_CLOSE, SR_CHANNEL_QUERY };
enum SR_ChannelState { SR_CHANNEL_CONNECTING = 0, SR_CHANNEL_OPENED, SR_CHANNEL_CLOSING, SR_CHANNEL_CLOSED };
enum SR_ChannelReliability { SR_RELIABLE = 0, SR_MAX_RETRANSMITS, SR_MAX_LIFETIME };
enum SR_DataKind { SR_TEXT = 0, SR_BINARY };
enum SR_DataEventType { SR_DATA_NONE = 0, SR_CHANNEL_CHANGED, SR_MESSAGE_RECEIVED, SR_DATA_FATAL };
typedef struct SR_ChannelRequest {
  uint32_t size, abi_version;
  uint64_t channel_id;
  int32_t action, kind, ordered, reliability, reliability_value;
  const char* label; uint32_t label_bytes;
  const uint8_t* data; uint32_t data_bytes;
} SR_ChannelRequest;
typedef struct SR_ChannelInfo {
  uint64_t channel_id;
  int32_t state, ordered, reliability, reliability_value, remote;
  uint32_t maximum_message_bytes, negotiated_message_bytes; // Zero means unknown.
  uint64_t buffered_bytes;
  char label[SR_MAX_CHANNEL_LABEL_BYTES + 1];
} SR_ChannelInfo;
typedef struct SR_DataEvent {
  uint32_t size, abi_version;
  int32_t type, kind, error_code;
  SR_ChannelInfo channel;
  uint32_t data_bytes;
  uint8_t data[SR_MAX_MESSAGE_BYTES];
} SR_DataEvent;
// Uses SR_SignalError codes. Oversized/invalid UTF-8 input is SR_INVALID_ARGUMENT;
// unknown/closed channels are SR_INVALID_STATE; full queues are SR_RESOURCE_EXHAUSTED.
SR_NATIVE_EXPORT int SimurghRTC_ChannelV1(struct SR_Runtime* runtime, const SR_ChannelRequest* request, SR_ChannelInfo* out);
SR_NATIVE_EXPORT int SimurghRTC_PollDataV1(struct SR_Runtime* runtime, SR_DataEvent* event);
#ifdef __cplusplus
}
#endif
#endif

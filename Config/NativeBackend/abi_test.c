// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "simurgh_rtc_abi.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
  char error[1024];
  struct SR_Runtime* a = NULL; struct SR_Runtime* b = NULL;
  assert(SimurghRTC_CreateV1(2, &a, error, sizeof(error)) && !a);
  assert(SimurghRTC_CreatePeerV1(NULL, error, sizeof(error)));
  for (int n = 0; n < 3; ++n) {
    assert(!SimurghRTC_CreateV1(1, &a, error, sizeof(error)));
    assert(!SimurghRTC_CreateV1(0, &b, error, sizeof(error)));
    SR_Diagnostics d = {0}; d.size = sizeof(d); d.abi_version = SR_ABI_VERSION;
    assert(!SimurghRTC_DiagnosticsV1(a, &d));
    assert(d.factory_ready && d.dummy_audio && d.audio_processing && strstr(d.audio_codecs,"opus;") && strstr(d.video_codecs,"VP8;"));
    assert(!SimurghRTC_CreatePeerV1(a, error, sizeof(error)));
    assert(SimurghRTC_CreatePeerV1(a, error, sizeof(error)));
    SimurghRTC_DestroyV1(a); a = NULL;
    SR_IceConfig ice = {sizeof(ice), SR_ABI_VERSION, NULL, 0, 0};
    assert(SimurghRTC_ConfigureIceV1(NULL, &ice) == SR_INVALID_ARGUMENT);
    assert(!SimurghRTC_ConfigureIceV1(b, &ice));
    ice.relay_only = 1; assert(SimurghRTC_ConfigureIceV1(b, &ice) == SR_INVALID_ARGUMENT);
    ice.relay_only = 2; assert(SimurghRTC_ConfigureIceV1(b, &ice) == SR_INVALID_ARGUMENT);
    ice.relay_only = 0; ice.server_count = 17; assert(SimurghRTC_ConfigureIceV1(b, &ice) == SR_INVALID_ARGUMENT);
    char url[] = "turn:127.0.0.1:9?transport=udp";
    SR_IceServer server = {url, (uint32_t)strlen(url), "fixture-user", 12, "fixture-password", 16};
    ice.servers = &server; ice.server_count = 1; ice.relay_only = 1;
    assert(!SimurghRTC_ConfigureIceV1(b, &ice));
    server.credential_bytes = 0; assert(SimurghRTC_ConfigureIceV1(b, &ice) == SR_INVALID_ARGUMENT);
    server.credential_bytes = 513; assert(SimurghRTC_ConfigureIceV1(b, &ice) == SR_INVALID_ARGUMENT);
    server.credential_bytes = 16; server.url_bytes = 2049; assert(SimurghRTC_ConfigureIceV1(b, &ice) == SR_INVALID_ARGUMENT);
    server.url_bytes = (uint32_t)strlen(url); url[0] = '@';
    assert(SimurghRTC_ConfigureIceV1(b, &ice) == SR_INVALID_ARGUMENT);
    // Rejected inputs leave the previous owned configuration intact.
    assert(!SimurghRTC_CreatePeerV1(b, error, sizeof(error)));
    assert(SimurghRTC_ConfigureIceV1(b, &ice) == SR_INVALID_STATE);
    SR_IceRoute route = {sizeof(route), SR_ABI_VERSION, 0, 0, 0, 0};
    assert(!SimurghRTC_GetIceRouteV1(b, &route));
    assert(!route.stats_samples && !route.local_type && !route.remote_type);
    route.size--; assert(SimurghRTC_GetIceRouteV1(b, &route) == SR_INVALID_ARGUMENT);
    assert(!SimurghRTC_DiagnosticsV1(b, &d));
    assert(d.peer_ready && !d.dummy_audio && !d.audio_processing && !d.audio_codecs[0] && !d.video_codecs[0]);
    d.abi_version = 2; assert(SimurghRTC_DiagnosticsV1(b, &d));
    SimurghRTC_DestroyV1(b); b = NULL;
  }
  SimurghRTC_DestroyV1(NULL);
  uint8_t* frames = malloc(SR_VIDEO_BYTES * 12);
  int16_t* pcm = malloc(SR_PCM_SAMPLES * sizeof(int16_t));
  SR_MediaOutput* out = calloc(1, sizeof(*out));
  assert(frames && pcm && out);
  for (int f = 0; f < 12; ++f) for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
    uint8_t* pixel = frames + f * SR_VIDEO_BYTES + (y * 64 + x) * 4;
    int high = 160 + f * 4;
    pixel[2] = (y < 32 ? x < 32 : x >= 32) ? high : 30;
    pixel[1] = x >= 32 ? high : 30; pixel[0] = y >= 32 && x < 32 ? high : 30; pixel[3] = 255;
  }
  for (int i = 0; i < 48000; ++i) pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)(8000 * sin(2 * 3.14159265358979323846 * 440 * i / 48000));
  SR_MediaInput input = {sizeof(input), SR_ABI_VERSION, frames, SR_VIDEO_BYTES * 12, pcm, SR_PCM_SAMPLES};
  out->size = sizeof(*out); out->abi_version = SR_ABI_VERSION;
  input.pcm_samples--; assert(SimurghRTC_MediaProbeV1(&input, out, error, sizeof(error))); input.pcm_samples++;
  for (int n = 0; n < 2; ++n) {
    int result = SimurghRTC_MediaProbeV1(&input, out, error, sizeof(error));
    if (result) fprintf(stderr, "%s\n", error);
    assert(!result && out->decoded_frames == 12 && out->encoded_frames == 12 && out->pcm_samples == 96000);
    printf("media: %d frames, %d packets, color error %d, correlation %.6f\n", out->decoded_frames, out->audio_packets, out->max_color_error, out->tone_correlation);
  }
  printf("PASS: six peers, overlapping owners, ABI rejection, repeated VP8/Opus; %s | %s | %s | %s\n",
      SimurghRTC_NativeVersionV1(0), SimurghRTC_NativeVersionV1(1), SimurghRTC_NativeVersionV1(2), SimurghRTC_NativeVersionV1(3));
  free(out); free(pcm); free(frames); return 0;
}

// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
// Link this with the isolated candidate and Unreal's original dependencies.
#include <stdio.h>
#include <string.h>
#include <openssl/crypto.h>
#include <opus.h>
#include <vpx/vpx_codec.h>
int SimurghRTC_NativeProbeV1(int media);
const char* SimurghRTC_NativeVersionV1(int component);
int main(void) {
  const char* crypto = SimurghRTC_NativeVersionV1(0);
  const char* opus = SimurghRTC_NativeVersionV1(1);
  const char* vpx = SimurghRTC_NativeVersionV1(2);
  printf("candidate: %s | %s | %s\n", crypto, opus, vpx);
  printf("engine: %s | %s | %s\n", OpenSSL_version(0), opus_get_version_string(), vpx_codec_version_str());
  if (strcmp(crypto, "BoringSSL") != 0 || strncmp(OpenSSL_version(0), "OpenSSL 1.1.1t", 14) != 0
      || strcmp(opus, opus_get_version_string()) == 0 || strcmp(vpx, vpx_codec_version_str()) == 0) return 1;
  if (SimurghRTC_NativeVersionV1(-1) != NULL || SimurghRTC_NativeProbeV1(-1) != 1) return 2;
  for (int cycle = 0; cycle < 3; ++cycle) {
    for (int media = 0; media <= 1; ++media) {
      int status = SimurghRTC_NativeProbeV1(media);
      if (status) { printf("FAIL cycle=%d media=%d status=%d\n", cycle, media, status); return 3; }
    }
  }
  puts("PASS: six factory/peer lifecycles; isolated crypto and codec versions");
  return 0;
}

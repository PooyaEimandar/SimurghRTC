// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
// Exercise CVE-2026-27135 through the public API, compiled against UE's headers.
#include <nghttp2/nghttp2.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error The security regression must run with assertions enabled.
#endif

struct State { int invalid; int terminated; int frames; };

static int invalid_frame(nghttp2_session *session, const nghttp2_frame *frame,
                         int error, void *user) {
  struct State *state = user;
  (void)error;
  assert(frame->hd.type == NGHTTP2_ALTSVC);
  ++state->invalid;
  ++state->terminated;
  return nghttp2_session_terminate_session(session, NGHTTP2_PROTOCOL_ERROR);
}

static int received_frame(nghttp2_session *session, const nghttp2_frame *frame,
                          void *user) {
  struct State *state = user;
  ++state->frames;
  if (frame->hd.type == NGHTTP2_ALTSVC) {
    ++state->terminated;
    return nghttp2_session_terminate_session(session, NGHTTP2_PROTOCOL_ERROR);
  }
  return 0;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const int mode = atoi(argv[1]);
  assert(mode >= 0 && mode <= 2);
  struct State state = {0};
  nghttp2_session_callbacks *callbacks;
  nghttp2_option *option;
  nghttp2_session *session;
  assert(nghttp2_session_callbacks_new(&callbacks) == 0);
  nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(callbacks, invalid_frame);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, received_frame);
  assert(nghttp2_option_new(&option) == 0);
  nghttp2_option_set_builtin_recv_extension_type(option, NGHTTP2_ALTSVC);
  assert(nghttp2_session_client_new2(&session, callbacks, &state, option) == 0);

  // Server SETTINGS, then ALTSVC, then an invalid zero-length PING in one read.
  // Mode 0 is a valid SETTINGS/PING exchange to detect a broken test setup.
  const uint8_t settings[] = {0,0,0, NGHTTP2_SETTINGS,0, 0,0,0,0};
  const uint8_t invalid_altsvc[] = {0,0,3, NGHTTP2_ALTSVC,0, 0,0,0,0, 0,0,'x'};
  const uint8_t valid_altsvc[] = {0,0,4, NGHTTP2_ALTSVC,0, 0,0,0,0, 0,1,'o','x'};
  const uint8_t bad_ping[] = {0,0,0, NGHTTP2_PING,0, 0,0,0,0};
  const uint8_t ping[] = {0,0,8, NGHTTP2_PING,0, 0,0,0,0, 0,0,0,0,0,0,0,0};
  uint8_t data[64];
  size_t size = sizeof(settings);
  memcpy(data, settings, size);
  if (mode) {
    const uint8_t *altsvc = mode == 1 ? invalid_altsvc : valid_altsvc;
    const size_t length = mode == 1 ? sizeof(invalid_altsvc) : sizeof(valid_altsvc);
    memcpy(data + size, altsvc, length); size += length;
    memcpy(data + size, bad_ping, sizeof(bad_ping)); size += sizeof(bad_ping);
  } else {
    memcpy(data + size, ping, sizeof(ping)); size += sizeof(ping);
  }
  assert(nghttp2_session_mem_recv2(session, data, size) == (nghttp2_ssize)size);
  assert(state.invalid == (mode == 1));
  assert(state.terminated == (mode != 0));
  assert(state.frames == (mode == 1 ? 1 : 2));
  if (mode) {
    // A subsequent read must also be ignored after termination.
    const int frames = state.frames;
    assert(nghttp2_session_mem_recv2(session, ping, sizeof(ping)) == sizeof(ping));
    assert(state.frames == frames);
  }
  printf("PASS mode=%d runtime=%s headers=%s\n", mode,
         nghttp2_version(0)->version_str, NGHTTP2_VERSION);
  nghttp2_session_del(session);
  nghttp2_option_del(option);
  nghttp2_session_callbacks_del(callbacks);
  return 0;
}

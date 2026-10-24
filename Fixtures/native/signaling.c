// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
// Real local sockets/ICE/DTLS/SCTP, without STUN/TURN or application payloads.
#include "simurgh_rtc_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef struct Peer {
  struct SR_Runtime* runtime;
  uint64_t next, done;
  int queued;
  SR_TransportState state;
  SR_SignalEvent* event;
  SR_SignalEvent* ice[32];
  unsigned ice_count;
  char sdp[SR_MAX_SDP_BYTES+1];
} Peer;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static void pause_ms(void) { struct timespec t={0,1000000}; nanosleep(&t,NULL); }
static void open_peer(Peer* p,int media) {
  memset(p,0,sizeof(*p)); char error[1024];
  CHECK(!SimurghRTC_CreateV1(media,&p->runtime,error,sizeof(error)));
  CHECK(!SimurghRTC_CreatePeerV1(p->runtime,error,sizeof(error)));
  p->event=calloc(1,sizeof(*p->event)); CHECK(p->event);
  p->event->size=sizeof(*p->event); p->event->abi_version=SR_ABI_VERSION;
}
static void close_peer(Peer* p) {
  SimurghRTC_DestroyV1(p->runtime); free(p->event);
  for(unsigned i=0;i<p->ice_count;i++) free(p->ice[i]);
}
static SR_SignalRequest request(Peer* p,int action) {
  SR_SignalRequest q={0}; q.size=sizeof(q); q.abi_version=SR_ABI_VERSION;
  q.operation_id=++p->next; q.action=action; q.timeout_ms=10000; return q;
}
static void submit(Peer* p,SR_SignalRequest q,int expected) {
  char error[1024]; int code=SimurghRTC_SignalV1(p->runtime,&q,error,sizeof(error));
  if(code!=expected) fprintf(stderr,"action %d code %d expected %d: %s\n",q.action,code,expected,error);
  CHECK(code==expected);
}
static void poll(Peer* p) {
  for(int n=0;n<32;n++) {
    CHECK(!SimurghRTC_PollEventV1(p->runtime,p->event));
    SR_SignalEvent* e=p->event; p->state=e->transport;
    if(e->type==SR_EVENT_NONE) break;
    if(e->type==SR_FATAL_ERROR || (e->type==SR_OPERATION_COMPLETE && e->error_code))
      fprintf(stderr,"event failure: %s\n",e->error);
    CHECK(e->type!=SR_FATAL_ERROR);
    if(e->type==SR_OPERATION_COMPLETE) {
      CHECK(e->error_code==SR_OK); p->done=e->operation_id; p->queued=e->queued;
      if(e->sdp_type) strcpy(p->sdp,e->text);
    }
    if(e->type==SR_LOCAL_ICE) {
      CHECK(p->ice_count<32); p->ice[p->ice_count]=malloc(sizeof(*e)); CHECK(p->ice[p->ice_count]);
      memcpy(p->ice[p->ice_count++],e,sizeof(*e));
    }
  }
}
static void wait_done(Peer* p,uint64_t id) {
  double limit=now()+10; while(p->done!=id && now()<limit) { poll(p); pause_ms(); } CHECK(p->done==id);
}
static void gather(Peer* p) {
  double limit=now()+10; while(!p->state.local_ice_complete && now()<limit) { poll(p); pause_ms(); }
  CHECK(p->state.local_ice_complete && p->ice_count>0);
}
static SR_SignalRequest candidate(Peer* to,const SR_SignalEvent* e) {
  SR_SignalRequest q=request(to,SR_ADD_ICE); q.text=e->text; q.text_bytes=(uint32_t)strlen(e->text);
  q.sdp_mid=e->sdp_mid; q.sdp_mid_bytes=(uint32_t)strlen(e->sdp_mid); q.sdp_mline_index=e->sdp_mline_index;
  q.username_fragment=e->username_fragment; q.username_fragment_bytes=(uint32_t)strlen(e->username_fragment); return q;
}
static void early(Peer* from,Peer* to) {
  for(unsigned i=0;i<from->ice_count;i++) {
    SR_SignalRequest q=candidate(to,from->ice[i]); submit(to,q,SR_OK); wait_done(to,q.operation_id); CHECK(to->queued);
  }
  CHECK(to->state.remote_ice_queued==from->ice_count && !to->state.remote_description_set);
  SR_SignalRequest duplicate=candidate(to,from->ice[0]); submit(to,duplicate,SR_OK); wait_done(to,duplicate.operation_id);
  CHECK(to->state.remote_ice_queued==from->ice_count);
  SR_SignalRequest end=request(to,SR_END_ICE); submit(to,end,SR_OK); wait_done(to,end.operation_id); CHECK(to->queued);
}
static void remote(Peer* to,Peer* from,int type) {
  SR_SignalRequest q=request(to,SR_SET_REMOTE); q.sdp_type=type;
  q.text=from->sdp; q.text_bytes=(uint32_t)strlen(q.text); submit(to,q,SR_OK); wait_done(to,q.operation_id);
}
static void expected_error(Peer* p,uint64_t id,int code) {
  double limit=now()+10; int found=0;
  while(!found && now()<limit) {
    CHECK(!SimurghRTC_PollEventV1(p->runtime,p->event));
    if(p->event->type==SR_OPERATION_COMPLETE && p->event->operation_id==id) {
      CHECK(p->event->error_code==code); found=1;
    }
    CHECK(p->event->type!=SR_FATAL_ERROR); pause_ms();
  }
  CHECK(found);
}
static void mismatches(Peer* source) {
  Peer *p=calloc(1,sizeof(Peer)); CHECK(p); open_peer(p,0); remote(p,source,SR_SDP_OFFER);
  // A rejected candidate must not be recorded as a successful duplicate on retry.
  for(int i=0;i<2;i++) {
    SR_SignalRequest q=candidate(p,source->ice[0]); q.sdp_mid="wrong"; q.sdp_mid_bytes=5;
    submit(p,q,SR_OK); expected_error(p,q.operation_id,SR_INVALID_ARGUMENT);
  }
  SR_SignalRequest q=candidate(p,source->ice[0]); q.username_fragment="stale"; q.username_fragment_bytes=5;
  submit(p,q,SR_INVALID_ARGUMENT); // Embedded and separately supplied ufrags must agree.
  q=request(p,SR_ADD_ICE); q.sdp_mid=source->ice[0]->sdp_mid; q.sdp_mid_bytes=(uint32_t)strlen(q.sdp_mid);
  q.sdp_mline_index=source->ice[0]->sdp_mline_index;
  q.text="candidate:stale 1 UDP 2122260223 127.0.0.1 10000 typ host ufrag stale"; q.text_bytes=(uint32_t)strlen(q.text);
  q.username_fragment="stale"; q.username_fragment_bytes=5;
  submit(p,q,SR_OK); expected_error(p,q.operation_id,SR_INVALID_ARGUMENT);
  q=candidate(p,source->ice[0]); submit(p,q,SR_OK); wait_done(p,q.operation_id); CHECK(p->state.remote_ice_applied==1);
  close_peer(p); open_peer(p,0);
  q=candidate(p,source->ice[0]); q.sdp_mid="wrong"; q.sdp_mid_bytes=5; submit(p,q,SR_OK); wait_done(p,q.operation_id); CHECK(p->queued);
  q=request(p,SR_SET_REMOTE); q.sdp_type=SR_SDP_OFFER; q.text=source->sdp; q.text_bytes=(uint32_t)strlen(q.text); submit(p,q,SR_OK);
  double limit=now()+10; int fatal=0;
  while(!fatal && now()<limit) {
    CHECK(!SimurghRTC_PollEventV1(p->runtime,p->event)); fatal=p->event->type==SR_FATAL_ERROR; pause_ms();
  }
  CHECK(fatal && p->event->error_code==SR_INVALID_ARGUMENT);
  close_peer(p); free(p);
}
static void pair(int media,int iteration) {
  Peer *a=calloc(1,sizeof(Peer)), *b=calloc(1,sizeof(Peer)); CHECK(a && b); open_peer(a,media); open_peer(b,media);
  SR_SignalRequest invalid=request(b,SR_CREATE_ANSWER); submit(b,invalid,SR_INVALID_STATE);
  invalid=request(b,SR_SET_REMOTE); invalid.sdp_type=SR_SDP_OFFER; invalid.text="v=0\n"; invalid.text_bytes=4;
  submit(b,invalid,SR_INVALID_ARGUMENT);
  SR_SignalRequest offer=request(a,SR_CREATE_OFFER); submit(a,offer,SR_OK); wait_done(a,offer.operation_id); gather(a);
  CHECK(strstr(a->sdp,"m=application") && strstr(a->sdp,"a=fingerprint:sha-256") && !strstr(a->sdp,"a=candidate:"));
  if(media==0 && iteration==0) mismatches(a);
  early(a,b); remote(b,a,SR_SDP_OFFER);
  SR_SignalRequest answer=request(b,SR_CREATE_ANSWER); submit(b,answer,SR_OK); wait_done(b,answer.operation_id); gather(b);
  early(b,a); remote(a,b,SR_SDP_ANSWER);
  double limit=now()+15;
  while(now()<limit) {
    poll(a); poll(b);
    if(a->state.connection==SR_CONNECTION_CONNECTED && b->state.connection==SR_CONNECTION_CONNECTED
      && a->state.data_transport_open && b->state.data_transport_open) break;
    pause_ms();
  }
  CHECK(a->state.connection==SR_CONNECTION_CONNECTED && b->state.connection==SR_CONNECTION_CONNECTED);
  CHECK(a->state.data_transport_open && b->state.data_transport_open);
  CHECK(!a->state.remote_ice_queued && !b->state.remote_ice_queued);
  CHECK(a->state.remote_ice_applied==b->ice_count && b->state.remote_ice_applied==a->ice_count);
  CHECK(a->state.remote_ice_complete && b->state.remote_ice_complete);
  SR_SignalRequest duplicate=candidate(b,a->ice[0]); submit(b,duplicate,SR_OK); wait_done(b,duplicate.operation_id);
  CHECK(b->state.remote_ice_applied==a->ice_count);
  invalid=request(a,SR_CREATE_OFFER); submit(a,invalid,SR_INVALID_STATE);
  printf("connected: mode=%d iteration=%d early_ice=%u/%u DTLS/SCTP=open\n",media,iteration,a->ice_count,b->ice_count);
  close_peer(a); close_peer(b); free(a); free(b);
}
static void bounds(void) {
  Peer *p=calloc(1,sizeof(Peer)); CHECK(p); open_peer(p,0);
  SR_SignalRequest q=request(p,SR_ADD_ICE); q.text="candidate:1 1 UDP 2122260223 127.0.0.1 10000 typ host";
  q.text_bytes=(uint32_t)strlen(q.text); q.sdp_mid="0"; q.sdp_mid_bytes=1;
  q.size--; submit(p,q,SR_INVALID_ARGUMENT); q.size++;
  q.text_bytes=SR_MAX_ICE_BYTES+1; submit(p,q,SR_INVALID_ARGUMENT); q.text_bytes=(uint32_t)strlen(q.text);
  for(int i=0;i<32;i++) {
    char text[128]; snprintf(text,sizeof(text),"candidate:%d 1 UDP 2122260223 127.0.0.1 %d typ host",i,10000+i);
    q.operation_id=++p->next; q.text=text; q.text_bytes=(uint32_t)strlen(text); submit(p,q,SR_OK); wait_done(p,q.operation_id);
  }
  CHECK(p->state.remote_ice_queued==32);
  q=request(p,SR_END_ICE); submit(p,q,SR_RESOURCE_EXHAUSTED);
  // Destruction with queued candidates and an in-flight CreateOffer must be safe.
  q=request(p,SR_CREATE_OFFER); submit(p,q,SR_OK); close_peer(p);
  open_peer(p,0);
  for(int i=0;i<65;i++) { q=request(p,SR_END_ICE); submit(p,q,SR_OK); }
  CHECK(!SimurghRTC_PollEventV1(p->runtime,p->event));
  CHECK(p->event->type==SR_FATAL_ERROR && p->event->error_code==SR_RESOURCE_EXHAUSTED);
  close_peer(p); free(p);
}
int main(void) {
  bounds(); for(int media=0;media<2;media++) for(int i=0;i<2;i++) pair(media,i);
  puts("PASS: four real local pairs, early ICE, deduplication, bounds, malformed SDP and teardown"); return 0;
}

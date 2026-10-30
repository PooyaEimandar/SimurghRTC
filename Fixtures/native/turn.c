// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#define SR_DATA_NO_MAIN
#include "data_channels.c"
static void relay_peer(Peer* p,const char* url,const char* user,const char* password) {
  memset(p,0,sizeof(*p));char error[1024];
  CHECK(!SimurghRTC_CreateV1(0,&p->runtime,error,sizeof(error)));
  SR_IceServer server={url,(uint32_t)strlen(url),user,(uint32_t)strlen(user),password,(uint32_t)strlen(password)};
  SR_IceConfig config={sizeof(config),SR_ABI_VERSION,&server,1,1};
  CHECK(!SimurghRTC_ConfigureIceV1(p->runtime,&config));
  CHECK(!SimurghRTC_CreatePeerV1(p->runtime,error,sizeof(error)));
  p->event=calloc(1,sizeof(*p->event));CHECK(p->event);
  p->event->size=sizeof(*p->event);p->event->abi_version=SR_ABI_VERSION;
}
int main(void) {
  char url[2049],user[513],password[513];
  CHECK(fgets(url,sizeof(url),stdin) && fgets(user,sizeof(user),stdin) && fgets(password,sizeof(password),stdin));
  url[strcspn(url,"\r\n")]=0;user[strcspn(user,"\r\n")]=0;password[strcspn(password,"\r\n")]=0;
  Peer *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b));CHECK(a && b);
  relay_peer(a,url,user,password);relay_peer(b,url,user,password);
  memset(password,0,sizeof(password));
  SR_SignalRequest q=request(a,SR_CREATE_OFFER);submit(a,q,SR_OK);wait_done(a,q.operation_id);gather(a);
  early(a,b);remote(b,a,SR_SDP_OFFER);
  q=request(b,SR_CREATE_ANSWER);submit(b,q,SR_OK);wait_done(b,q.operation_id);gather(b);
  early(b,a);remote(a,b,SR_SDP_ANSWER);
  SR_IceRoute left_route={sizeof(left_route),SR_ABI_VERSION,0,0,0,0},right_route=left_route;
  double until=now()+15;
  while(now()<until) {
    poll(a);poll(b);CHECK(!a->fatal && !b->fatal);
    CHECK(!SimurghRTC_GetIceRouteV1(a->runtime,&left_route) && !SimurghRTC_GetIceRouteV1(b->runtime,&right_route));
    if(a->state.data_transport_open && b->state.data_transport_open && left_route.local_type==4 && right_route.local_type==4)break;
    pause_ms();
  }
  CHECK(a->state.data_transport_open && b->state.data_transport_open);
  CHECK(left_route.local_type==4 && left_route.remote_type==4 && right_route.local_type==4 && right_route.remote_type==4);
  const int protocol=strstr(url,"transport=tcp") ? 2 : 1;
  CHECK(left_route.relay_protocol==protocol && right_route.relay_protocol==protocol);
  SR_ChannelInfo *left=info(),*right=info();opened(a,b,SR_RELIABLE,0,1,left,right);
  const uint8_t text[]={0x48,0x69,0x20,0xd8,0xb3,0xd9,0x84,0xd8,0xa7,0xd9,0x85};
  const uint8_t bytes[]={0,1,2,127,128,255,0};
  send(a,left->channel_id,text,sizeof(text),SR_TEXT,SR_OK);receive(b,right->channel_id,text,sizeof(text),SR_TEXT);
  send(b,right->channel_id,text,sizeof(text),SR_TEXT,SR_OK);receive(a,left->channel_id,text,sizeof(text),SR_TEXT);
  send(a,left->channel_id,bytes,sizeof(bytes),SR_BINARY,SR_OK);receive(b,right->channel_id,bytes,sizeof(bytes),SR_BINARY);
  send(b,right->channel_id,bytes,sizeof(bytes),SR_BINARY,SR_OK);receive(a,left->channel_id,bytes,sizeof(bytes),SR_BINARY);
  printf("PASS: TURN relay-only text/binary round trips; local=4 remote=4 protocol=%d stats=%u/%u\n",protocol,left_route.stats_samples,right_route.stats_samples);
  close_peer(a);close_peer(b);free(a);free(b);free(left);free(right);return 0;
}

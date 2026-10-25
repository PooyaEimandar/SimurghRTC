// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
// Reuse the real SDP/ICE fixture helpers; both source hashes are recorded by the runner.
#define main signaling_fixture_main
#include "signaling.c"
#undef main
static SR_ChannelRequest data_request(int action,uint64_t id) {
  SR_ChannelRequest q={0};q.size=sizeof(q);q.abi_version=SR_ABI_VERSION;q.action=action;q.channel_id=id;return q;
}
static SR_ChannelInfo* info(void) { SR_ChannelInfo* p=calloc(1,sizeof(*p));CHECK(p);return p; }
static SR_DataEvent* event(void) {
  SR_DataEvent* e=calloc(1,sizeof(*e));CHECK(e);e->size=sizeof(*e);e->abi_version=SR_ABI_VERSION;return e;
}
static void advertised_limit(Peer* p) {
  char* start=strstr(p->sdp,"a=max-message-size:");CHECK(start);start+=strlen("a=max-message-size:");
  char* end=strstr(start,"\r\n");CHECK(end);memmove(start+5,end,strlen(end)+1);memcpy(start,"32768",5);
}
static void connected(Peer* a,Peer* b,int small) {
  open_peer(a,0);open_peer(b,0);
  SR_SignalRequest q=request(a,SR_CREATE_OFFER);submit(a,q,SR_OK);wait_done(a,q.operation_id);gather(a);
  if(small)advertised_limit(a);
  early(a,b);remote(b,a,SR_SDP_OFFER);
  q=request(b,SR_CREATE_ANSWER);submit(b,q,SR_OK);wait_done(b,q.operation_id);gather(b);
  if(small)advertised_limit(b);
  early(b,a);remote(a,b,SR_SDP_ANSWER);
  double end=now()+10;
  while(now()<end && (!a->state.data_transport_open || !b->state.data_transport_open)) {poll(a);poll(b);pause_ms();}
  CHECK(a->state.data_transport_open && b->state.data_transport_open);
}
static void opened(Peer* a,Peer* b,int reliability,int value,int ordered,SR_ChannelInfo* left,SR_ChannelInfo* right) {
  SR_ChannelRequest q=data_request(SR_CHANNEL_OPEN,0);q.label="roundtrip";q.label_bytes=9;
  q.reliability=reliability;q.reliability_value=value;q.ordered=ordered;
  CHECK(!SimurghRTC_ChannelV1(a->runtime,&q,left));
  SR_DataEvent *ea=event(),*eb=event();double end=now()+10;
  while(now()<end) {
    CHECK(!SimurghRTC_PollDataV1(a->runtime,ea));CHECK(!SimurghRTC_PollDataV1(b->runtime,eb));
    CHECK(ea->type!=SR_DATA_FATAL && eb->type!=SR_DATA_FATAL);
    if(ea->type==SR_CHANNEL_CHANGED && ea->channel.channel_id==left->channel_id)*left=ea->channel;
    if(eb->type==SR_CHANNEL_CHANGED && eb->channel.remote && eb->channel.state!=SR_CHANNEL_CLOSED)*right=eb->channel;
    if(left->state==SR_CHANNEL_OPENED && right->state==SR_CHANNEL_OPENED && left->maximum_message_bytes && right->maximum_message_bytes)break;
    pause_ms();
  }
  CHECK(left->state==SR_CHANNEL_OPENED && right->state==SR_CHANNEL_OPENED);
  CHECK(left->maximum_message_bytes && right->maximum_message_bytes);
  CHECK(left->ordered==ordered && right->ordered==ordered && left->reliability==reliability && right->reliability==reliability);
  CHECK(left->reliability_value==value && right->reliability_value==value && !left->remote && right->remote);
  free(ea);free(eb);
}
static void receive(Peer* p,uint64_t id,const uint8_t* bytes,uint32_t length,int binary) {
  SR_DataEvent* e=event();double end=now()+10;int found=0;
  while(now()<end && !found) {
    CHECK(!SimurghRTC_PollDataV1(p->runtime,e));CHECK(e->type!=SR_DATA_FATAL);
    if(e->type==SR_MESSAGE_RECEIVED) {
      CHECK(e->channel.channel_id==id && e->kind==binary && e->data_bytes==length);
      CHECK(!length || !memcmp(e->data,bytes,length));found=1;
    }
    pause_ms();
  }
  CHECK(found);free(e);
}
static void send(Peer* p,uint64_t id,const uint8_t* bytes,uint32_t length,int binary,int expected) {
  SR_ChannelRequest q=data_request(SR_CHANNEL_SEND,id);q.data=bytes;q.data_bytes=length;q.kind=binary;
  SR_ChannelInfo* out=info();CHECK(SimurghRTC_ChannelV1(p->runtime,&q,out)==expected);free(out);
}
static void closed(Peer* a,Peer* b,SR_ChannelInfo* left,SR_ChannelInfo* right) {
  SR_ChannelRequest q=data_request(SR_CHANNEL_CLOSE,left->channel_id);CHECK(!SimurghRTC_ChannelV1(a->runtime,&q,left));
  SR_DataEvent *ea=event(),*eb=event();double end=now()+10;
  while(now()<end && (left->state!=SR_CHANNEL_CLOSED || right->state!=SR_CHANNEL_CLOSED)) {
    CHECK(!SimurghRTC_PollDataV1(a->runtime,ea));CHECK(!SimurghRTC_PollDataV1(b->runtime,eb));
    if(ea->type==SR_CHANNEL_CHANGED && ea->channel.channel_id==left->channel_id)*left=ea->channel;
    if(eb->type==SR_CHANNEL_CHANGED && eb->channel.channel_id==right->channel_id)*right=eb->channel;
    pause_ms();
  }
  CHECK(left->state==SR_CHANNEL_CLOSED && right->state==SR_CHANNEL_CLOSED);
  send(a,left->channel_id,NULL,0,1,SR_INVALID_STATE);
  CHECK(!SimurghRTC_ChannelV1(a->runtime,&q,left));free(ea);free(eb);
}
static void roundtrips(int small) {
  Peer *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b));CHECK(a && b);connected(a,b,small);
  SR_ChannelInfo *left=info(),*right=info();
  const int modes[][3]={{SR_RELIABLE,0,1},{SR_RELIABLE,0,0},{SR_MAX_RETRANSMITS,0,1},{SR_MAX_RETRANSMITS,2,0},{SR_MAX_LIFETIME,10000,1},{SR_MAX_LIFETIME,10000,0}};
  for(int c=0;c<6;c++) {
    *left=(SR_ChannelInfo){0};*right=(SR_ChannelInfo){0};opened(a,b,modes[c][0],modes[c][1],modes[c][2],left,right);
    CHECK(left->maximum_message_bytes==(small?32768:SR_MAX_MESSAGE_BYTES));
    uint8_t* data=malloc(left->maximum_message_bytes+1);CHECK(data);
    for(uint32_t i=0;i<=left->maximum_message_bytes;i++)data[i]=(uint8_t)(i*37+19);
    send(a,left->channel_id,data,left->maximum_message_bytes+1,SR_BINARY,SR_INVALID_ARGUMENT);
    const uint8_t invalid[]={0xc0,0x80};send(a,left->channel_id,invalid,2,SR_TEXT,SR_INVALID_ARGUMENT);
    const uint8_t utf8[]={'h','i',0,0xd8,0xb3,0xd9,0x84,0xd8,0xa7,0xd9,0x85,0xf0,0x9f,0x8c,0x8d};
    for(int kind=0;kind<2;kind++) {
      const uint8_t* payload=kind?data:utf8;uint32_t bytes=kind?left->maximum_message_bytes:sizeof(utf8);
      send(a,left->channel_id,payload,bytes,kind,SR_OK);receive(b,right->channel_id,payload,bytes,kind);
      send(b,right->channel_id,payload,bytes,kind,SR_OK);receive(a,left->channel_id,payload,bytes,kind);
      send(a,left->channel_id,NULL,0,kind,SR_OK);receive(b,right->channel_id,NULL,0,kind);
    }
    if(c==0) {
      for(uint8_t n=0;n<32;n++)send(a,left->channel_id,&n,1,SR_BINARY,SR_OK);
      for(uint8_t n=0;n<32;n++)receive(b,right->channel_id,&n,1,SR_BINARY);
    }
    free(data);closed(a,b,left,right);
  }
  // Zero lifetime is a supported setting, with no delivery guarantee.
  *left=(SR_ChannelInfo){0};*right=(SR_ChannelInfo){0};opened(b,a,SR_MAX_LIFETIME,0,0,left,right);closed(b,a,left,right);
  SR_ChannelRequest bad=data_request(SR_CHANNEL_OPEN,0);bad.reliability=SR_RELIABLE;bad.reliability_value=1;
  CHECK(SimurghRTC_ChannelV1(a->runtime,&bad,left)==SR_INVALID_ARGUMENT);
  bad.reliability_value=0;bad.label="bad\0label";bad.label_bytes=9;
  CHECK(SimurghRTC_ChannelV1(a->runtime,&bad,left)==SR_INVALID_ARGUMENT);
  bad.size--;CHECK(SimurghRTC_ChannelV1(a->runtime,&bad,left)==SR_INVALID_ARGUMENT);
  close_peer(a);close_peer(b);free(a);free(b);free(left);free(right);
}
static void receive_limit(void) {
  Peer *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b));CHECK(a && b);connected(a,b,0);
  SR_ChannelInfo *left=info(),*right=info();opened(a,b,SR_RELIABLE,0,1,left,right);
  for(int n=0;n<SR_MAX_DATA_EVENTS+1;n++)send(a,left->channel_id,NULL,0,SR_BINARY,SR_OK);
  struct timespec wait={0,250000000};nanosleep(&wait,NULL);
  SR_DataEvent* e=event();CHECK(!SimurghRTC_PollDataV1(b->runtime,e));
  CHECK(e->type==SR_DATA_FATAL && e->error_code==SR_RESOURCE_EXHAUSTED);
  close_peer(a);close_peer(b);free(a);free(b);free(left);free(right);free(e);
}
int main(void) {
  CHECK(SimurghRTC_ChannelV1(NULL,NULL,NULL)==SR_INVALID_ARGUMENT);
  CHECK(SimurghRTC_PollDataV1(NULL,NULL)==SR_INVALID_ARGUMENT);
  roundtrips(0);roundtrips(1);receive_limit();
  puts("PASS: DataChannel UTF-8/binary round trips, six reliability/order modes, negotiated limits, FIFO and bounded receive overflow");return 0;
}

// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
// Real PeerConnection Opus/SRTP delivery; generated PCM, no audio hardware.
#define main signaling_fixture_main
#include "signaling.c"
#undef main
#include <math.h>

typedef struct Tone { double energy, block_energy; uint64_t samples; } Tone;
static void observe(Tone* tone, const SR_AudioOutput* out, double frequency) {
  CHECK(out->sample_rate == 48000 && (out->channels == 1 || out->channels == 2));
  CHECK(out->sample_count == (uint32_t)(480 * out->channels));
  double sine=0,cosine=0;
  for (unsigned i=0; i<480; ++i) {
    double value=out->samples[i*out->channels]; ++tone->samples;
    CHECK(isfinite(value) && value>=-1 && value<=1);
    tone->energy+=value*value;
    const double block_phase=2*3.141592653589793*frequency*i/48000;
    sine+=value*sin(block_phase);cosine+=value*cos(block_phase);
  }
  tone->block_energy+=2*(sine*sine+cosine*cosine)/480;
}
static double correlation(const Tone* t) {
  // NetEq may adjust playout timing. Measure tone energy per decoded 10 ms
  // block, without requiring a fixed phase across jitter-buffer adjustments.
  return t->energy>0?sqrt(t->block_energy/t->energy):0;
}
static void tone_measurement(void) {
  SR_AudioOutput out={0};out.sample_rate=48000;out.channels=1;out.sample_count=480;
  Tone matching={0},wrong={0};
  for(int block=0;block<100;block++) {
    for(int i=0;i<480;i++)out.samples[i]=(float)(.25*sin(2*3.141592653589793*400*i/48000+block*.7));
    observe(&matching,&out,400);observe(&wrong,&out,800);
  }
  CHECK(correlation(&matching)>.999&&correlation(&wrong)<.001);
}
static void audio_state(Peer* p, SR_AudioState* state) {
  memset(state,0,sizeof(*state)); state->size=sizeof(*state); state->abi_version=SR_ABI_VERSION;
  CHECK(!SimurghRTC_AudioStateV1(p->runtime,state));
  CHECK(state->queued_input_blocks<=SR_AUDIO_QUEUE_BLOCKS && state->queued_output_blocks<=SR_AUDIO_QUEUE_BLOCKS);
}
static void configure(Peer* p, int rate, int channels) {
  SR_AudioConfig c={sizeof(c),SR_ABI_VERSION,rate,channels};
  CHECK(!SimurghRTC_ConfigureAudioV1(p->runtime,&c));
  CHECK(SimurghRTC_ConfigureAudioV1(p->runtime,&c)==SR_INVALID_STATE);
}
static void input(Peer* p, int rate, int channels, uint64_t frame, double frequency) {
  float samples[SR_AUDIO_MAX_SAMPLES]; unsigned count=rate/100*channels;
  for(unsigned i=0;i<count;i++) samples[i]=(float)(.25*sin(2*3.141592653589793*frequency*(frame*(rate/100)+i/channels)/rate));
  SR_AudioInput in={sizeof(in),SR_ABI_VERSION,samples,count,(int64_t)frame*10000};
  CHECK(!SimurghRTC_PushAudioV1(p->runtime,&in));
  // The native queue must own a copy, not this caller-owned stack.
  memset(samples,0,sizeof(samples));
  CHECK(SimurghRTC_PushAudioV1(p->runtime,&in)==SR_INVALID_ARGUMENT);
}
static void stream(Peer* a, Peer* b, int rate, int channels, uint64_t* frame,
                   double seconds, double skip, Tone* at, Tone* bt) {
  double start=now(),next=start,limit=start+seconds;
  SR_AudioOutput out={0}; out.size=sizeof(out);out.abi_version=SR_ABI_VERSION;
  while(now()<limit) {
    poll(a); poll(b); CHECK(!a->fatal && !b->fatal);
    if(now()>=next) {
      input(a,rate,channels,*frame,400); input(b,rate,channels,*frame,800); ++*frame;
      next+=.01; if(next<now()-.02) next=now()+.01;
    }
    for(int side=0;side<2;side++) {
      for(int j=0;j<SR_AUDIO_QUEUE_BLOCKS;j++) {
        CHECK(!SimurghRTC_PollAudioV1((side?b:a)->runtime,&out));
        if(!out.sample_count) break;
        if(now()-start>=skip) observe(side?bt:at,&out,side?400:800);
      }
    }
    pause_ms();
  }
}
static void audio_pair(int rate,int channels) {
  Peer *a=calloc(1,sizeof(Peer)), *b=calloc(1,sizeof(Peer)); CHECK(a&&b);
  open_peer(a,1); open_peer(b,1); configure(a,rate,channels); configure(b,rate,channels);
  SR_SignalRequest q=request(a,SR_CREATE_OFFER); submit(a,q,SR_OK);wait_done(a,q.operation_id);gather(a);
  CHECK(strstr(a->sdp,"m=audio")&&strstr(a->sdp,"opus/48000/2"));
  early(a,b);remote(b,a,SR_SDP_OFFER);
  q=request(b,SR_CREATE_ANSWER);submit(b,q,SR_OK);wait_done(b,q.operation_id);gather(b);
  early(b,a);remote(a,b,SR_SDP_ANSWER);
  double limit=now()+15;
  while(now()<limit) {
    poll(a);poll(b);CHECK(!a->fatal&&!b->fatal);
    if(a->state.connection==SR_CONNECTION_CONNECTED&&b->state.connection==SR_CONNECTION_CONNECTED
       &&a->state.data_transport_open&&b->state.data_transport_open)break;
    pause_ms();
  }
  CHECK(a->state.connection==SR_CONNECTION_CONNECTED&&b->state.connection==SR_CONNECTION_CONNECTED);
  uint64_t frame=1; Tone at={0},bt={0};
  stream(a,b,rate,channels,&frame,4,1.5,&at,&bt);
  fprintf(stderr,"audio rate=%d channels=%d samples=%llu/%llu correlation=%.5f/%.5f\n",rate,channels,
    (unsigned long long)at.samples,(unsigned long long)bt.samples,correlation(&at),correlation(&bt));
  CHECK(at.samples>=48000&&bt.samples>=48000&&correlation(&at)>.9&&correlation(&bt)>.9);
  CHECK(sqrt(at.energy/at.samples)>.1&&sqrt(bt.energy/bt.samples)>.1);
  CHECK(!SimurghRTC_MuteAudioV1(a->runtime,1));
  Tone muted_a={0},muted_b={0};stream(a,b,rate,channels,&frame,1.5,.8,&muted_a,&muted_b);
  CHECK(muted_b.samples>=24000&&sqrt(muted_b.energy/muted_b.samples)<.002);
  CHECK(correlation(&muted_a)>.9);
  CHECK(!SimurghRTC_MuteAudioV1(a->runtime,0));
  Tone resumed_a={0},resumed_b={0};stream(a,b,rate,channels,&frame,2,.8,&resumed_a,&resumed_b);
  CHECK(resumed_b.samples>=24000&&correlation(&resumed_b)>.9);
  // A slow receiver drops old audio instead of retaining an unbounded backlog.
  double quiet=now()+.4;while(now()<quiet)pause_ms();
  SR_AudioState sa,sb;audio_state(a,&sa);audio_state(b,&sb);
  CHECK(sa.remote_track&&sb.remote_track&&sa.dropped_output_blocks>0&&sb.dropped_output_blocks>0);
  CHECK(sa.admitted_blocks==frame-1&&sb.admitted_blocks==frame-1);
  CHECK(sa.consumed_blocks>200&&sb.consumed_blocks>200);
  printf("audio pair: rate=%d channels=%d received=%llu/%llu dropped=%llu/%llu mute/resume=pass\n",rate,channels,
    (unsigned long long)sa.received_blocks,(unsigned long long)sb.received_blocks,
    (unsigned long long)sa.dropped_output_blocks,(unsigned long long)sb.dropped_output_blocks);
  close_peer(a);close_peer(b);free(a);free(b);
}
static void audio_bounds(void) {
  Peer *p=calloc(1,sizeof(Peer));CHECK(p);open_peer(p,0);
  SR_AudioConfig config={sizeof(config),SR_ABI_VERSION,48000,1};
  CHECK(SimurghRTC_ConfigureAudioV1(p->runtime,&config)==SR_INVALID_STATE);
  close_peer(p);open_peer(p,1);
  config.sample_rate=12345;CHECK(SimurghRTC_ConfigureAudioV1(p->runtime,&config)==SR_INVALID_ARGUMENT);
  config.sample_rate=48000;CHECK(!SimurghRTC_ConfigureAudioV1(p->runtime,&config));
  float samples[480]={0};SR_AudioInput in={sizeof(in),SR_ABI_VERSION,samples,480,0};
  samples[0]=NAN;CHECK(SimurghRTC_PushAudioV1(p->runtime,&in)==SR_INVALID_ARGUMENT);samples[0]=0;
  in.sample_count=479;CHECK(SimurghRTC_PushAudioV1(p->runtime,&in)==SR_INVALID_ARGUMENT);in.sample_count=480;
  in.size--;CHECK(SimurghRTC_PushAudioV1(p->runtime,&in)==SR_INVALID_ARGUMENT);in.size++;
  int result=0;unsigned accepted=0;
  while(accepted<200) {
    in.timestamp_us=accepted*10000;result=SimurghRTC_PushAudioV1(p->runtime,&in);
    if(result==SR_RESOURCE_EXHAUSTED)break;CHECK(result==SR_OK);accepted++;
  }
  CHECK(result==SR_RESOURCE_EXHAUSTED&&accepted>=SR_AUDIO_QUEUE_BLOCKS);
  CHECK(!SimurghRTC_MuteAudioV1(p->runtime,1)); // Clears obsolete queued input.
  CHECK(!SimurghRTC_PushAudioV1(p->runtime,&in)); // Rejected timestamp was not consumed.
  CHECK(SimurghRTC_MuteAudioV1(p->runtime,2)==SR_INVALID_ARGUMENT);
  close_peer(p);free(p);
}
int main(void) {
  tone_measurement();audio_bounds();
  const int rates[]={8000,16000,32000,44100,48000};
  for(unsigned i=0;i<5;i++)audio_pair(rates[i],i%2+1);
  puts("PASS: five bidirectional Opus audio pairs, resampling, copied PCM, bounded queues, mute/resume and teardown");return 0;
}

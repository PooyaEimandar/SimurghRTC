// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "signaling.h"
#include <cstdio>
#include <cstring>
#include "api/make_ref_counted.h"
#include "rtc_base/ip_address.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "pc/session_description.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"

namespace sr {
namespace {
int Reject(int code, char* error, uint32_t capacity, const char* message) {
  if (error && capacity) std::snprintf(error, capacity, "%s", message);
  return code;
}
bool Text(const char* data, uint32_t length, uint32_t maximum) {
  return length <= maximum && (!length || (data && !std::memchr(data, 0, length)));
}
std::string Copy(const char* data, uint32_t length) { return length ? std::string(data, length) : std::string(); }
class StatsObserver : public webrtc::RTCStatsCollectorCallback {
 public:
  explicit StatsObserver(std::shared_ptr<Signaling> owner) : owner_(std::move(owner)) {}
  void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override { owner_->Stats(*report); }
 private:
  std::shared_ptr<Signaling> owner_;
};
class LocalObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  LocalObserver(std::shared_ptr<Signaling> owner, uint64_t id, int type, std::string sdp)
      : owner_(std::move(owner)), id_(id), type_(type), sdp_(std::move(sdp)) {}
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    owner_->LocalSet(id_, type_, std::move(sdp_), std::move(error));
  }
 private:
  std::shared_ptr<Signaling> owner_; uint64_t id_; int type_; std::string sdp_;
};
class RemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  RemoteObserver(std::shared_ptr<Signaling> owner, uint64_t id) : owner_(std::move(owner)), id_(id) {}
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override { owner_->RemoteSet(id_, std::move(error)); }
 private:
  std::shared_ptr<Signaling> owner_; uint64_t id_;
};
class CreateObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  CreateObserver(std::shared_ptr<Signaling> owner, uint64_t id, int type) : owner_(std::move(owner)), id_(id), type_(type) {}
  void OnSuccess(webrtc::SessionDescriptionInterface* description) override {
    owner_->Created(id_, type_, std::unique_ptr<webrtc::SessionDescriptionInterface>(description));
  }
  void OnFailure(webrtc::RTCError error) override { owner_->Complete(id_, SR_NATIVE_ERROR, error.message()); }
 private:
  std::shared_ptr<Signaling> owner_; uint64_t id_; int type_;
};
}

void Signaling::Shutdown() {
  stopped_ = true; peer_ = nullptr;
  channels_.Shutdown();
  if (channel_) { channel_->UnregisterObserver(); channel_->Close(); channel_ = nullptr; }
  pending_.clear(); early_ice_.clear(); events_.clear(); seen_ice_.clear();
}
bool Signaling::Active(uint64_t id) const { return !stopped_ && !failed_ && pending_.contains(id); }
void Signaling::Push(Event event) {
  if (stopped_ || failed_) return;
  if (events_.size() >= SR_MAX_SIGNAL_EVENTS) { Fatal(SR_RESOURCE_EXHAUSTED, "Native signaling event queue is full."); return; }
  event.state = state_; events_.push_back(std::move(event));
}
void Signaling::Fatal(int code, const std::string& error) {
  if (stopped_ || failed_) return;
  failed_ = true; state_.connection = SR_CONNECTION_FAILED;
  if (state_.stats_samples) state_.stats_age_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stats_received_).count());
  pending_.clear(); sdp_id_ = 0; early_ice_.clear(); state_.remote_ice_queued = 0;
  events_.clear(); Event event; event.type = SR_FATAL_ERROR; event.code = code; event.error = error; event.state = state_;
  events_.push_back(std::move(event));
}
void Signaling::Complete(uint64_t id, int code, const std::string& error, int type, std::string text, bool queued) {
  if (!Active(id)) return;
  pending_.erase(id); if (sdp_id_ == id) sdp_id_ = 0;
  Event event; event.type = SR_OPERATION_COMPLETE; event.id = id; event.code = code;
  event.error = error; event.sdp_type = type; event.text = std::move(text); event.queued = queued; Push(std::move(event));
}
void Signaling::Poll(SR_SignalEvent& out) {
  if (channels_.Failed()) Fatal(SR_RESOURCE_EXHAUSTED, "Application channel receive failed or exceeded its bounds.");
  for (const auto& entry : pending_) if (Clock::now() >= entry.second) {
    Fatal(SR_TIMED_OUT, "Native signaling operation timed out."); break;
  }
  if (state_.stats_samples) state_.stats_age_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stats_received_).count());
  if (!stopped_ && !failed_ && peer_ && state_.local_description_set && !stats_pending_
      && Clock::now() - stats_requested_ >= std::chrono::milliseconds(250)) {
    stats_requested_ = Clock::now(); stats_pending_ = true;
    const auto callback = webrtc::make_ref_counted<StatsObserver>(shared_from_this());
    peer_->GetStats(callback.get());  // GetStats retains shared ownership.
  }
  out.type = SR_EVENT_NONE; out.transport = state_;
  if (events_.empty()) return;
  Event event = std::move(events_.front()); events_.pop_front();
  out.type = event.type; out.operation_id = event.id; out.error_code = event.code; out.queued = event.queued;
  out.sdp_type = event.sdp_type; out.sdp_mline_index = event.mline; out.transport = event.state;
  std::snprintf(out.error, sizeof(out.error), "%s", event.error.c_str());
  std::snprintf(out.text, sizeof(out.text), "%s", event.text.c_str());
  std::snprintf(out.sdp_mid, sizeof(out.sdp_mid), "%s", event.mid.c_str());
  std::snprintf(out.username_fragment, sizeof(out.username_fragment), "%s", event.ufrag.c_str());
}

int Signaling::Submit(const SR_SignalRequest& q, char* error, uint32_t capacity) {
  if (error && capacity) error[0] = 0;
  if (stopped_ || failed_ || !peer_) return Reject(SR_INVALID_STATE, error, capacity, "Peer is not available.");
  if (!q.operation_id || q.operation_id <= last_id_ || q.timeout_ms < 1 || q.timeout_ms > 300000
      || q.action < SR_CREATE_OFFER || q.action > SR_END_ICE
      || !Text(q.text, q.text_bytes, q.action == SR_ADD_ICE ? SR_MAX_ICE_BYTES : SR_MAX_SDP_BYTES)
      || !Text(q.sdp_mid, q.sdp_mid_bytes, SR_MAX_MID_BYTES)
      || !Text(q.username_fragment, q.username_fragment_bytes, SR_MAX_UFRAG_BYTES))
    return Reject(SR_INVALID_ARGUMENT, error, capacity, "Invalid signaling request, ID, deadline or text bounds.");
  const bool sdp = q.action <= SR_SET_REMOTE;
  if ((sdp && sdp_id_) || pending_.size() >= SR_MAX_SIGNAL_EVENTS)
    return Reject(SR_RESOURCE_EXHAUSTED, error, capacity, "Another SDP operation is pending or signaling capacity is full.");
  if (q.action != SR_ADD_ICE && (q.sdp_mid_bytes || q.username_fragment_bytes))
    return Reject(SR_INVALID_ARGUMENT, error, capacity, "SDP operations do not take candidate fields.");
  if ((q.action == SR_CREATE_OFFER || q.action == SR_CREATE_ANSWER || q.action == SR_END_ICE) && q.text_bytes)
    return Reject(SR_INVALID_ARGUMENT, error, capacity, "Unexpected signaling payload.");
  if (q.action == SR_SET_REMOTE && (q.sdp_type != SR_SDP_OFFER && q.sdp_type != SR_SDP_ANSWER))
    return Reject(SR_INVALID_ARGUMENT, error, capacity, "Expected offer or answer SDP.");
  const auto signaling = peer_->signaling_state();
  if ((q.action == SR_CREATE_OFFER && (state_.local_description_set || state_.remote_description_set || signaling != webrtc::PeerConnectionInterface::kStable))
      || (q.action == SR_CREATE_ANSWER && (state_.local_description_set || signaling != webrtc::PeerConnectionInterface::kHaveRemoteOffer))
      || (q.action == SR_SET_REMOTE && (state_.remote_description_set
          || (q.sdp_type == SR_SDP_OFFER && (state_.local_description_set || signaling != webrtc::PeerConnectionInterface::kStable))
          || (q.sdp_type == SR_SDP_ANSWER && signaling != webrtc::PeerConnectionInterface::kHaveLocalOffer))))
    return Reject(SR_INVALID_STATE, error, capacity, "Invalid initial negotiation state; restart with a fresh peer.");

  std::unique_ptr<webrtc::SessionDescriptionInterface> description;
  std::unique_ptr<webrtc::IceCandidate> candidate;
  std::string candidate_key;
  if (q.action == SR_SET_REMOTE) {
    std::string text = Copy(q.text, q.text_bytes);
    if (!text.starts_with("v=0\r\n") || !text.ends_with("\r\n"))
      return Reject(SR_INVALID_ARGUMENT, error, capacity, "Expected nonempty CRLF-terminated SDP.");
    webrtc::SdpParseError parse;
    description = webrtc::CreateSessionDescription(q.sdp_type == SR_SDP_OFFER ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer, text, &parse);
    if (!description) return Reject(SR_INVALID_ARGUMENT, error, capacity, "Remote SDP could not be parsed.");
  }
  if (q.action == SR_ADD_ICE) {
    const auto mid = Copy(q.sdp_mid, q.sdp_mid_bytes);
    const auto text = Copy(q.text, q.text_bytes);
    const auto ufrag = Copy(q.username_fragment, q.username_fragment_bytes);
    if (mid.empty() || q.sdp_mline_index < 0 || q.sdp_mline_index > 65535
        || !text.starts_with("candidate:") || text.find_first_of("\r\n") != std::string::npos)
      return Reject(SR_INVALID_ARGUMENT, error, capacity, "Invalid ICE candidate or media section.");
    candidate = webrtc::IceCandidate::Create(mid, q.sdp_mline_index, text);
    if (!candidate) return Reject(SR_INVALID_ARGUMENT, error, capacity, "ICE candidate could not be parsed.");
    auto value = candidate->candidate();
    if (loopback_probe_ && !webrtc::IPIsLoopback(value.address().ipaddr()))
      return Reject(SR_INVALID_ARGUMENT, error, capacity, "Loopback probes only accept loopback candidates.");
    if (!ufrag.empty()) {
      if (!value.username().empty() && value.username() != ufrag)
        return Reject(SR_INVALID_ARGUMENT, error, capacity, "Candidate username fragments differ.");
      value.set_username(ufrag); candidate = webrtc::CreateIceCandidate(mid, q.sdp_mline_index, value);
    }
    candidate_key = mid + "\n" + std::to_string(q.sdp_mline_index) + "\n" + candidate->ToString() + "\n" + value.username();
    if (!seen_ice_.contains(candidate_key) && (end_ice_ || seen_ice_.size() >= SR_MAX_REMOTE_ICE))
      return Reject(end_ice_ ? SR_INVALID_STATE : SR_RESOURCE_EXHAUSTED, error, capacity, "Remote ICE is complete or its candidate limit was reached.");
  }
  if (q.action == SR_END_ICE && !end_ice_ && seen_ice_.size() >= SR_MAX_REMOTE_ICE)
    return Reject(SR_RESOURCE_EXHAUSTED, error, capacity, "Remote ICE entry limit was reached.");

  last_id_ = q.operation_id;
  pending_[q.operation_id] = Clock::now() + std::chrono::milliseconds(q.timeout_ms);
  if (sdp) sdp_id_ = q.operation_id;
  if (q.action == SR_CREATE_OFFER || q.action == SR_CREATE_ANSWER) {
    if (q.action == SR_CREATE_OFFER && !channel_) {
      // Keep a private bootstrap channel so application channels can open after negotiation.
      webrtc::DataChannelInit options;
      options.protocol = "simurghrtc-internal/1";
      auto channel = peer_->CreateDataChannelOrError("simurghrtc-negotiation", &options);
      if (!channel.ok()) { Complete(q.operation_id, SR_NATIVE_ERROR, channel.error().message()); return SR_OK; }
      OnDataChannel(channel.MoveValue());
    }
    auto observer = webrtc::make_ref_counted<CreateObserver>(shared_from_this(), q.operation_id,
      q.action == SR_CREATE_OFFER ? SR_SDP_OFFER : SR_SDP_ANSWER);
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    if (q.action == SR_CREATE_OFFER) peer_->CreateOffer(observer.get(), options);
    else peer_->CreateAnswer(observer.get(), options);
  } else if (q.action == SR_SET_REMOTE) {
    peer_->SetRemoteDescription(std::move(description), webrtc::make_ref_counted<RemoteObserver>(shared_from_this(), q.operation_id));
  } else if (q.action == SR_ADD_ICE) {
    if (seen_ice_.contains(candidate_key)) { Complete(q.operation_id, SR_OK, {}, 0, {}, !state_.remote_description_set); return SR_OK; }
    seen_ice_.insert(candidate_key);
    if (!state_.remote_description_set) {
      early_ice_.push_back(std::move(candidate)); state_.remote_ice_queued = static_cast<uint32_t>(early_ice_.size());
      Complete(q.operation_id, SR_OK, {}, 0, {}, true);
    } else ApplyIce(q.operation_id, std::move(candidate), false);
  } else {
    end_ice_ = true; RefreshRemoteCompletion();
    Complete(q.operation_id, SR_OK, {}, 0, {}, !state_.remote_ice_complete);
  }
  return SR_OK;
}

void Signaling::Created(uint64_t id, int type, std::unique_ptr<webrtc::SessionDescriptionInterface> description) {
  if (!Active(id)) return;
  std::string sdp;
  if (!description || !description->ToString(&sdp) || sdp.empty() || sdp.size() > SR_MAX_SDP_BYTES) {
    Complete(id, SR_NATIVE_ERROR, "Generated SDP is empty or exceeds the limit."); return;
  }
  auto observer = webrtc::make_ref_counted<LocalObserver>(shared_from_this(), id, type, std::move(sdp));
  peer_->SetLocalDescription(std::move(description), std::move(observer));
}
void Signaling::LocalSet(uint64_t id, int type, std::string sdp, webrtc::RTCError error) {
  if (!Active(id)) return;
  if (!error.ok()) { Complete(id, SR_NATIVE_ERROR, error.message()); return; }
  state_.local_description_set = 1;
  Complete(id, SR_OK, {}, type, std::move(sdp));
}
void Signaling::RemoteSet(uint64_t id, webrtc::RTCError error) {
  if (!Active(id)) return;
  if (!error.ok()) { Complete(id, SR_NATIVE_ERROR, error.message()); return; }
  state_.remote_description_set = 1; Complete(id, SR_OK);
  while (!early_ice_.empty() && !failed_ && !stopped_) {
    auto candidate = std::move(early_ice_.front()); early_ice_.pop_front();
    state_.remote_ice_queued = static_cast<uint32_t>(early_ice_.size());
    ApplyIce(0, std::move(candidate), true);
  }
  RefreshRemoteCompletion();
}
void Signaling::ApplyIce(uint64_t id, std::unique_ptr<webrtc::IceCandidate> candidate, bool was_queued) {
  const auto key = candidate->sdp_mid() + "\n" + std::to_string(candidate->sdp_mline_index()) + "\n"
      + candidate->ToString() + "\n" + candidate->candidate().username();
  const auto* remote = peer_->remote_description();
  const auto& contents = remote->description()->contents();
  const auto* info = remote->description()->GetTransportInfoByName(candidate->sdp_mid());
  const auto& ufrag = candidate->candidate().username();
  if (static_cast<size_t>(candidate->sdp_mline_index()) >= contents.size()
      || contents[candidate->sdp_mline_index()].mid() != candidate->sdp_mid() || !info
      || (!ufrag.empty() && ufrag != info->description.ice_ufrag)) {
    if (was_queued) Fatal(SR_INVALID_ARGUMENT, "Queued ICE does not match the remote media section or username fragment.");
    else { seen_ice_.erase(key); Complete(id, SR_INVALID_ARGUMENT, "ICE does not match the remote media section or username fragment."); }
    return;
  }
  ++applying_ice_;
  peer_->AddIceCandidate(std::move(candidate), [self = shared_from_this(), id, was_queued, key](webrtc::RTCError error) {
    if (self->stopped_ || self->failed_) return;
    --self->applying_ice_;
    if (!error.ok()) {
      if (was_queued) self->Fatal(SR_NATIVE_ERROR, "Queued ICE could not be applied.");
      else { self->seen_ice_.erase(key); self->Complete(id, SR_NATIVE_ERROR, error.message()); }
      return;
    }
    ++self->state_.remote_ice_applied; self->RefreshRemoteCompletion();
    if (!was_queued) self->Complete(id, SR_OK);
    else { Event event; event.type = SR_REMOTE_ICE_APPLIED; self->Push(std::move(event)); }
  });
}
void Signaling::RefreshRemoteCompletion() {
  state_.remote_ice_complete = end_ice_ && state_.remote_description_set && early_ice_.empty() && !applying_ice_;
}
void Signaling::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  if (stopped_ || failed_) { channel->Close(); return; }
  if (channel->protocol() != "simurghrtc-internal/1") { channels_.Accept(std::move(channel)); return; }
  if (channel_ || channel->label() != "simurghrtc-negotiation") { channel->Close(); return; }
  channel_ = std::move(channel); channel_->RegisterObserver(this); OnStateChange();
}
void Signaling::OnStateChange() {
  if (stopped_ || failed_ || !channel_) return;
  state_.data_transport_open = channel_->state() == webrtc::DataChannelInterface::kOpen;
  Event event; event.type = SR_DATA_TRANSPORT_CHANGED; Push(std::move(event));
}
void Signaling::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) {
  if (stopped_ || failed_ || state != webrtc::PeerConnectionInterface::kIceGatheringComplete) return;
  state_.local_ice_complete = 1; Event event; event.type = SR_LOCAL_ICE_COMPLETE; Push(std::move(event));
}
void Signaling::OnIceCandidate(const webrtc::IceCandidate* candidate) {
  if (stopped_ || failed_) return;
  if (loopback_probe_ && !webrtc::IPIsLoopback(candidate->candidate().address().ipaddr())) return;
  if (candidate->candidate().protocol() == "udp") ++state_.local_udp_candidates;
  if (candidate->candidate().protocol() == "tcp") ++state_.local_tcp_candidates;
  Event event; event.type = SR_LOCAL_ICE; event.text = candidate->ToString();
  event.mid = candidate->sdp_mid(); event.mline = candidate->sdp_mline_index(); event.ufrag = candidate->candidate().username();
  if (++local_ice_count_ >= SR_MAX_REMOTE_ICE || event.text.size() > SR_MAX_ICE_BYTES
      || event.mid.size() > SR_MAX_MID_BYTES || event.ufrag.size() > SR_MAX_UFRAG_BYTES) {
    Fatal(SR_RESOURCE_EXHAUSTED, "Local ICE exceeds the signaling limits."); return;
  }
  Push(std::move(event));
}
void Signaling::OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) {
  if (stopped_ || failed_) return;
  state_.ice_state = static_cast<int32_t>(state);
}
void Signaling::OnIceCandidateError(const std::string&, int, const std::string&, int code, const std::string&) {
  if (stopped_ || failed_) return;
  ++state_.candidate_errors; state_.last_candidate_error = static_cast<uint32_t>(code);
}
void Signaling::Stats(const webrtc::RTCStatsReport& report) {
  if (stopped_ || failed_) return;
  stats_pending_ = false; stats_received_ = Clock::now(); ++state_.stats_samples; state_.stats_age_ms = 0;
  state_.candidate_pairs = state_.succeeded_pairs = state_.failed_pairs = 0;
  state_.stun_requests_sent = state_.stun_responses_received = state_.packets_discarded_on_send = 0;
  state_.selected_protocol = 0; state_.dtls_state = 0;
  for (const auto* pair : report.GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
    ++state_.candidate_pairs;
    if (pair->state == "succeeded") ++state_.succeeded_pairs;
    if (pair->state == "failed") ++state_.failed_pairs;
    state_.stun_requests_sent += pair->requests_sent.value_or(0);
    state_.stun_responses_received += pair->responses_received.value_or(0);
    state_.packets_discarded_on_send += pair->packets_discarded_on_send.value_or(0);
  }
  for (const auto* transport : report.GetStatsOfType<webrtc::RTCTransportStats>()) {
    const auto dtls = transport->dtls_state.value_or("");
    state_.dtls_state = dtls == "new" ? 1 : dtls == "connecting" ? 2 : dtls == "connected" ? 3 : dtls == "closed" ? 4 : dtls == "failed" ? 5 : 0;
    if (transport->selected_candidate_pair_id) {
      const auto* pair = report.GetAs<webrtc::RTCIceCandidatePairStats>(*transport->selected_candidate_pair_id);
      const auto* candidate = pair && pair->local_candidate_id ? report.GetAs<webrtc::RTCLocalIceCandidateStats>(*pair->local_candidate_id) : nullptr;
      if (candidate) { const auto protocol = candidate->protocol.value_or(""); state_.selected_protocol = protocol == "udp" ? 1 : protocol == "tcp" ? 2 : 3; }
    }
  }
}
void Signaling::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) {
  if (stopped_ || failed_ || !peer_) return;
  using State = webrtc::PeerConnectionInterface::PeerConnectionState;
  switch (state) {
    case State::kNew: state_.connection = SR_CONNECTION_NEW; break;
    case State::kConnecting: state_.connection = SR_CONNECTION_CONNECTING; break;
    case State::kConnected: state_.connection = SR_CONNECTION_CONNECTED; break;
    case State::kDisconnected: state_.connection = SR_CONNECTION_DISCONNECTED; break;
    case State::kFailed:
      state_.ice_state = static_cast<int32_t>(peer_->ice_connection_state());
      Fatal(SR_NATIVE_ERROR, state_.ice_state == webrtc::PeerConnectionInterface::kIceConnectionFailed
          ? "PeerConnection failed during ICE connectivity checks." : "PeerConnection failed after ICE setup; inspect transport diagnostics."); return;
    case State::kClosed: state_.connection = SR_CONNECTION_CLOSED; break;
  }
  Event event; event.type = SR_CONNECTION_CHANGED; Push(std::move(event));
}
}

// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "simurgh_rtc_abi.h"
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include "api/peer_connection_interface.h"

namespace sr {
// All methods and callbacks run on the native signaling thread. Asynchronous
// observers retain this context, not SR_Runtime. Shutdown invalidates the raw peer
// before releasing it; late observers can only discard their results.
class Signaling final : public webrtc::PeerConnectionObserver,
                        public webrtc::DataChannelObserver,
                        public std::enable_shared_from_this<Signaling> {
 public:
  void Attach(webrtc::PeerConnectionInterface* peer) { peer_ = peer; }
  void Shutdown();
  int Submit(const SR_SignalRequest& request, char* error, uint32_t capacity);
  void Poll(SR_SignalEvent& out);
  bool Active(uint64_t id) const;
  void Created(uint64_t id, int type, std::unique_ptr<webrtc::SessionDescriptionInterface> description);
  void LocalSet(uint64_t id, int type, std::string sdp, webrtc::RTCError error);
  void RemoteSet(uint64_t id, webrtc::RTCError error);
  void Complete(uint64_t id, int code, const std::string& error = {}, int type = 0,
                std::string text = {}, bool queued = false);
  void Fatal(int code, const std::string& error);

  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override;
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override;
  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer&) override {} // Public DataChannel I/O is SR-103.

 private:
  struct Event {
    int type = 0, code = 0, sdp_type = 0, mline = 0;
    uint64_t id = 0;
    bool queued = false;
    std::string error, text, mid, ufrag;
    SR_TransportState state{};
  };
  using Clock = std::chrono::steady_clock;
  void Push(Event event);
  void ApplyIce(uint64_t id, std::unique_ptr<webrtc::IceCandidate> candidate, bool was_queued);
  void RefreshRemoteCompletion();
  bool stopped_ = false, failed_ = false, end_ice_ = false;
  uint64_t last_id_ = 0, sdp_id_ = 0;
  uint32_t local_ice_count_ = 0, applying_ice_ = 0;
  webrtc::PeerConnectionInterface* peer_ = nullptr;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> channel_;
  SR_TransportState state_{};
  std::map<uint64_t, Clock::time_point> pending_;
  std::deque<Event> events_;
  std::deque<std::unique_ptr<webrtc::IceCandidate>> early_ice_;
  std::set<std::string> seen_ice_;
};
}

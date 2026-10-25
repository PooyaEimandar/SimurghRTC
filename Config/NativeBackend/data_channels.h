// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "simurgh_rtc_abi.h"
#include "api/peer_connection_interface.h"
#include <memory>

namespace sr {
// Signaling-thread facade. Observers copy directly into a bounded mailbox on the
// network thread; they never call back into WebRTC or post unbounded message tasks.
class Channels {
 public:
  Channels();
  ~Channels();
  void Attach(webrtc::PeerConnectionInterface* peer);
  void Shutdown();
  void Accept(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel);
  int Command(const SR_ChannelRequest& request, SR_ChannelInfo& out);
  void Poll(SR_DataEvent& out);
  bool Failed() const;
 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}

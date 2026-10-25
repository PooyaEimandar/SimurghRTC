// Copyright SimurghRTC contributors. SPDX-License-Identifier: Apache-2.0
#include "data_channels.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace sr {
namespace {
constexpr char Protocol[] = "simurghrtc-data/1";
bool Utf8(const uint8_t* data, size_t bytes) {
  if (bytes && !data) return false;
  for (size_t i = 0; i < bytes;) {
    uint32_t c = data[i++]; unsigned n = 0; uint32_t minimum = 0;
    if (c < 0x80) continue;
    if (c >= 0xc2 && c <= 0xdf) { c &= 0x1f; n = 1; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { c &= 0xf; n = 2; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { c &= 7; n = 3; minimum = 0x10000; }
    else return false;
    if (bytes - i < n) return false;
    while (n--) { const auto b = data[i++]; if ((b & 0xc0) != 0x80) return false; c = (c << 6) | (b & 0x3f); }
    if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
  }
  return true;
}
struct Message { uint64_t id; bool binary; std::vector<uint8_t> bytes; };
struct Mailbox {
  std::mutex mutex;
  std::deque<Message> messages;
  size_t bytes = 0;
  int failure = 0;
  bool stopped = false;
};
class Observer final : public webrtc::DataChannelObserver {
 public:
  Observer(uint64_t id, std::shared_ptr<Mailbox> mail) : id_(id), mail_(std::move(mail)) {}
  bool IsOkToCallOnTheNetworkThread() override { return true; }
  void OnStateChange() override {} // State/buffer snapshots are sampled by Poll, outside callbacks.
  void OnBufferedAmountChange(uint64_t) override {}
  void OnMaxMessageSize(int maximum) override {
    std::lock_guard<std::mutex> lock(mail_->mutex); maximum_ = maximum;
  }
  int Maximum() const { std::lock_guard<std::mutex> lock(mail_->mutex); return maximum_; }
  void OnMessage(const webrtc::DataBuffer& buffer) override {
    std::lock_guard<std::mutex> lock(mail_->mutex);
    if (mail_->stopped || mail_->failure) return;
    if (buffer.size() > SR_MAX_MESSAGE_BYTES || (!buffer.binary && !Utf8(buffer.data.data(), buffer.size()))) {
      mail_->failure = SR_INVALID_ARGUMENT; return;
    }
    if (mail_->messages.size() >= SR_MAX_DATA_EVENTS || buffer.size() > SR_MAX_DATA_EVENT_BYTES - mail_->bytes) {
      mail_->failure = SR_RESOURCE_EXHAUSTED; return;
    }
    Message message{id_, buffer.binary, {}};
    if (buffer.size()) message.bytes.assign(buffer.data.data(), buffer.data.data() + buffer.size());
    mail_->bytes += message.bytes.size(); mail_->messages.push_back(std::move(message));
  }
 private:
  uint64_t id_;
  std::shared_ptr<Mailbox> mail_;
  int maximum_ = 0; // Protected by the mailbox mutex.
};
}
class Channels::Impl {
 public:
  struct Entry {
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
    std::unique_ptr<Observer> observer;
    SR_ChannelInfo info{};
    bool closing = false;
  };
  webrtc::PeerConnectionInterface* peer = nullptr;
  std::shared_ptr<Mailbox> mail = std::make_shared<Mailbox>();
  std::map<uint64_t, Entry> records;
  std::deque<SR_ChannelInfo> changes;
  uint64_t next = 1;
  bool stopped = false, fatal_reported = false;
  std::chrono::steady_clock::time_point sampled{};
  bool Failed() const { std::lock_guard<std::mutex> lock(mail->mutex); return mail->failure != 0; }
  void Fail(int code) { std::lock_guard<std::mutex> lock(mail->mutex); if (!mail->failure) mail->failure = code; }
  void Shutdown() {
    if (stopped) return;
    stopped = true; peer = nullptr;
    { std::lock_guard<std::mutex> lock(mail->mutex); mail->stopped = true; mail->messages.clear(); mail->bytes = 0; }
    for (auto& [id, e] : records) if (e.channel) {
      e.channel->UnregisterObserver(); e.channel->Close(); e.channel = nullptr; e.observer.reset();
    }
    changes.clear();
  }
  void Changed(const SR_ChannelInfo& info) {
    // Coalesce repeated buffered-amount updates, but preserve state transitions.
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) if (it->channel_id == info.channel_id) {
      if (it->state == info.state) *it = info;
      else break;
      return;
    }
    if (changes.size() >= SR_MAX_DATA_EVENTS) { Fail(SR_RESOURCE_EXHAUSTED); return; }
    changes.push_back(info);
  }
  void Refresh(Entry& e) {
    if (!e.channel) return;
    auto info = e.info;
    info.state = static_cast<int32_t>(e.channel->state());
    info.buffered_bytes = e.channel->buffered_amount();
    const int maximum = e.observer->Maximum();
    info.negotiated_message_bytes = maximum > 0 ? uint32_t(maximum) : 0;
    info.maximum_message_bytes = std::min(info.negotiated_message_bytes, uint32_t(SR_MAX_MESSAGE_BYTES));
    // Deliver already-received messages before reporting a graceful remote close.
    if (!e.closing && info.state >= SR_CHANNEL_CLOSING) {
      std::lock_guard<std::mutex> lock(mail->mutex);
      if (std::any_of(mail->messages.begin(), mail->messages.end(), [&](const auto& m) { return m.id == info.channel_id; }))
        info.state = e.info.state;
    }
    if (info.state != e.info.state || info.buffered_bytes != e.info.buffered_bytes
        || info.maximum_message_bytes != e.info.maximum_message_bytes
        || info.negotiated_message_bytes != e.info.negotiated_message_bytes) { e.info = info; Changed(info); }
    if (info.state == SR_CHANNEL_CLOSED) {
      e.channel->UnregisterObserver(); e.channel = nullptr; e.observer.reset();
    }
  }
  int Add(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel, bool remote, SR_ChannelInfo& out) {
    size_t live = 0; for (const auto& [id, e] : records) if (e.channel) ++live;
    if (stopped || Failed() || live >= SR_MAX_CHANNELS || records.size() >= SR_MAX_CHANNEL_RECORDS) {
      channel->Close(); return SR_RESOURCE_EXHAUSTED;
    }
    const auto label = channel->label();
    const auto retransmits = channel->maxRetransmitsOpt(), lifetime = channel->maxPacketLifeTime();
    if (channel->protocol() != Protocol || channel->negotiated() || label.size() > SR_MAX_CHANNEL_LABEL_BYTES
        || label.find('\0') != std::string::npos || !Utf8(reinterpret_cast<const uint8_t*>(label.data()), label.size())
        || (retransmits && lifetime)) { channel->Close(); return SR_INVALID_ARGUMENT; }
    const auto id = next++;
    auto& e = records[id]; e.channel = std::move(channel);
    e.info.channel_id = id; e.info.ordered = e.channel->ordered(); e.info.remote = remote;
    e.info.reliability = retransmits ? SR_MAX_RETRANSMITS : lifetime ? SR_MAX_LIFETIME : SR_RELIABLE;
    e.info.reliability_value = retransmits ? *retransmits : lifetime ? *lifetime : 0;
    if (e.info.reliability_value < 0 || e.info.reliability_value > 65535) { e.channel->Close(); records.erase(id); return SR_INVALID_ARGUMENT; }
    std::memcpy(e.info.label, label.data(), label.size());
    e.observer = std::make_unique<Observer>(id, mail);
    e.channel->RegisterObserver(e.observer.get());
    Changed(e.info); Refresh(e); out = e.info; return SR_OK;
  }
};
Channels::Channels() : impl_(std::make_unique<Impl>()) {}
Channels::~Channels() { impl_->Shutdown(); }
void Channels::Attach(webrtc::PeerConnectionInterface* peer) { impl_->peer = peer; }
void Channels::Shutdown() { impl_->Shutdown(); }
bool Channels::Failed() const { return impl_->Failed(); }
void Channels::Accept(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  SR_ChannelInfo ignored{};
  const int code = impl_->Add(std::move(channel), true, ignored);
  if (code) impl_->Fail(code); // Never silently discard an admitted remote channel.
}
int Channels::Command(const SR_ChannelRequest& q, SR_ChannelInfo& out) {
  auto& s = *impl_; out = {};
  if (s.stopped || s.Failed() || !s.peer) return SR_INVALID_STATE;
  if (q.action < SR_CHANNEL_OPEN || q.action > SR_CHANNEL_QUERY) return SR_INVALID_ARGUMENT;
  if (q.action == SR_CHANNEL_OPEN) {
    if (q.channel_id || q.data_bytes || q.label_bytes > SR_MAX_CHANNEL_LABEL_BYTES
        || !Utf8(reinterpret_cast<const uint8_t*>(q.label), q.label_bytes)
        || (q.label_bytes && std::memchr(q.label, 0, q.label_bytes))
        || (q.ordered != 0 && q.ordered != 1) || q.reliability < SR_RELIABLE || q.reliability > SR_MAX_LIFETIME
        || q.reliability_value < 0 || q.reliability_value > 65535 || (q.reliability == SR_RELIABLE && q.reliability_value)) return SR_INVALID_ARGUMENT;
    size_t live = 0; for (const auto& [id, e] : s.records) if (e.channel) ++live;
    if (live >= SR_MAX_CHANNELS || s.records.size() >= SR_MAX_CHANNEL_RECORDS) return SR_RESOURCE_EXHAUSTED;
    webrtc::DataChannelInit options;
    options.ordered = q.ordered != 0; options.protocol = Protocol;
    if (q.reliability == SR_MAX_RETRANSMITS) options.maxRetransmits = q.reliability_value;
    if (q.reliability == SR_MAX_LIFETIME) options.maxRetransmitTime = q.reliability_value;
    auto created = s.peer->CreateDataChannelOrError(q.label_bytes ? std::string(q.label, q.label_bytes) : std::string(), &options);
    if (!created.ok()) return SR_NATIVE_ERROR;
    return s.Add(created.MoveValue(), false, out);
  }
  if (!q.channel_id || q.label_bytes) return SR_INVALID_ARGUMENT;
  auto found = s.records.find(q.channel_id);
  if (found == s.records.end()) return SR_INVALID_STATE;
  auto& e = found->second; s.Refresh(e); out = e.info;
  if (q.action == SR_CHANNEL_QUERY) return q.data_bytes ? SR_INVALID_ARGUMENT : SR_OK;
  if (q.action == SR_CHANNEL_CLOSE) {
    if (q.data_bytes) return SR_INVALID_ARGUMENT;
    e.closing = true;
    { std::lock_guard<std::mutex> lock(s.mail->mutex);
      for (auto it = s.mail->messages.begin(); it != s.mail->messages.end();) {
        if (it->id == q.channel_id) { s.mail->bytes -= it->bytes.size(); it = s.mail->messages.erase(it); }
        else ++it;
      }
    }
    if (e.channel) { e.channel->Close(); s.Refresh(e); }
    out = e.info; return SR_OK;
  }
  if (q.kind != SR_TEXT && q.kind != SR_BINARY) return SR_INVALID_ARGUMENT;
  if (q.data_bytes > SR_MAX_MESSAGE_BYTES || (q.data_bytes && !q.data)
      || (q.kind == SR_TEXT && !Utf8(q.data, q.data_bytes))) return SR_INVALID_ARGUMENT;
  if (!e.channel || e.closing || e.info.state != SR_CHANNEL_OPENED || !e.info.maximum_message_bytes) return SR_INVALID_STATE;
  if (q.data_bytes > e.info.maximum_message_bytes) return SR_INVALID_ARGUMENT;
  if (e.info.buffered_bytes + q.data_bytes > SR_MAX_CHANNEL_BUFFER_BYTES) return SR_RESOURCE_EXHAUSTED;
  const webrtc::DataBuffer data(webrtc::CopyOnWriteBuffer(q.data, q.data_bytes), q.kind == SR_BINARY);
  const bool sent = e.channel->Send(data); // Executed on the native worker, never Unreal's game thread.
  s.Refresh(e); out = e.info;
  return sent ? SR_OK : e.info.state == SR_CHANNEL_OPENED ? SR_RESOURCE_EXHAUSTED : SR_INVALID_STATE;
}
void Channels::Poll(SR_DataEvent& out) {
  auto& s = *impl_; out.type = SR_DATA_NONE; out.data_bytes = 0; out.error_code = 0;
  if (s.Failed()) {
    if (!s.fatal_reported) {
      { std::lock_guard<std::mutex> lock(s.mail->mutex); out.error_code = s.mail->failure; }
      s.Shutdown(); s.fatal_reported = true; out.type = SR_DATA_FATAL;
    }
    return;
  }
  if (s.stopped) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - s.sampled >= std::chrono::milliseconds(10)) {
    s.sampled = now;
    for (auto& [id, e] : s.records) s.Refresh(e);
  }
  if (!s.changes.empty()) {
    out.type = SR_CHANNEL_CHANGED; out.channel = s.changes.front(); s.changes.pop_front(); return;
  }
  Message message;
  {
    std::lock_guard<std::mutex> lock(s.mail->mutex);
    if (s.mail->messages.empty()) return;
    message = std::move(s.mail->messages.front()); s.mail->messages.pop_front(); s.mail->bytes -= message.bytes.size();
  }
  const auto found = s.records.find(message.id);
  if (found == s.records.end() || found->second.closing) return;
  out.type = SR_MESSAGE_RECEIVED; out.channel = found->second.info; out.kind = message.binary ? SR_BINARY : SR_TEXT;
  out.data_bytes = uint32_t(message.bytes.size());
  if (out.data_bytes) std::memcpy(out.data, message.bytes.data(), out.data_bytes);
}
}

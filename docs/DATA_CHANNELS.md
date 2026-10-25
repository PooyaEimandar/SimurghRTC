# DataChannels

SR-103 adds text and binary messages to the internal native and Direct providers. HTTP negotiates the connection; application messages travel over WebRTC's DTLS/SCTP transport. The shared `USimurghRTCSession` C++/Blueprint facade is still waiting for SR-105.

On 2026-10-25, packaged Unreal 5.8.3 Mac ARM64 Full client and dedicated Server passed **292 exact round trips across 24 channels and four fresh connections**, including backpressure recovery and native teardown. All 20 Unreal and 166 Python tests pass. The [validation record](reviews/SR103-data-channel-validation.json) includes artifact/build hashes, scope and earlier failed attempts.

## Use a channel

Wait for the Direct client to reach `Connected`. Install a handler, open a channel, then wait for its `Open` event before sending. All provider calls and callbacks run on Unreal's game thread.

```cpp
Client->SetChannelHandler([](const SimurghRTC::FNativeChannelEvent& Event)
{
    if (Event.Type == SimurghRTC::ENativeDataEvent::Message)
    {
        // Event.Message contains Text or Bytes and the local channel handle.
        // Copy it if you need to retain it after this callback.
    }
    // Changed reports opening, closing, buffered bytes and writable status.
});
FSimurghRTCChannelOptions Options;
Options.Label = TEXT("chat"); // Reliable and ordered by default.
FSimurghRTCChannelHandle Channel;
const FSimurghRTCError Error = Client->OpenChannel(Options, Channel);
```

A successful `OpenChannel` returns a local handle in `Connecting`. Native creation finishes asynchronously; an error event reports failure. Once open, call `SendText(Channel, Text)` or `SendBytes(Channel, Bytes)`. Call `GetChannelInfo` for current state and limits, and `CloseChannel` to close it. Closing the same known channel again is harmless. A foreign handle or a send after close returns `InvalidState`.

The Server receives addressed callbacks through `FDirectServerConfig::OnChannelEvent(SessionId, Event)`. Its channel methods also take `SessionId`. Each peer has its own local handles; do not send a client's handle to the Server. The sample probe provides a bounded echo queue. The provider does not echo automatically.

For concurrent clients, private echoes and authorized room fan-out, see [Server sessions and routing](SERVER_ROUTING.md).

## Ordering and reliability

| Option | Meaning |
|---|---|
| `bOrdered = true` | Deliver received messages in channel order. |
| `bOrdered = false` | Allow delivery without ordering. |
| `Reliable` | Retransmit as needed; `ReliabilityValue` must be zero. |
| `MaxRetransmits` | Retry at most `ReliabilityValue` times; zero is allowed. |
| `MaxLifetime` | Give a message up to `ReliabilityValue` milliseconds to send. |

The two partial-reliability limits accept 0–65535 and cannot be combined. Partial reliability can drop messages; a successful send does not promise delivery. The remote channel reports the negotiated options. Application channels use protocol `simurghrtc-data/1`; the private negotiation channel remains internal.

## Payload ownership and limits

Send copies the payload before returning. The caller may immediately change or release its input. Receive callbacks own their copied values for the duration of the callback. Text is length-delimited UTF-8 on the wire, including embedded NUL characters; binary bytes are unchanged. Empty messages are valid.

| Limit | Behavior |
|---|---|
| Message | At most 64 KiB, or the smaller negotiated SCTP limit. Oversize or invalid text returns `InvalidArgument`. |
| Queued send bytes per channel | 256 KiB, including queued Unreal bytes and the latest native buffered amount. |
| Queued sends per peer | 64 messages and 1 MiB. Empty messages consume a slot. |
| Channels per peer | 16 live channels and 64 total channel records during that peer's lifetime. Closed handles are not reused. |
| Receive queues | Native: 64 messages/1 MiB plus 64 state events. Unreal: 64 events/1 MiB of payload storage, counting converted text. |

`ResourceExhausted` means the send was **not admitted**. Retain that message in your own bounded queue and retry after a `Changed` event reports `bWritable`. The blocked channel resumes below the 64 KiB low-water threshold once peer queue capacity is also available. Writable status is a hint: a particular message may still be too large for the remaining space.

An accepted send may wait in the native worker if SCTP is full. It has a 30-second queue deadline. Expiry, a native send failure, invalid incoming data or receive overflow fails the peer explicitly. Accepted means copied for sending, not acknowledged by the remote application. Closing a channel immediately suppresses its remaining message callbacks and schedules native closure. The worker cancels sends still queued when it processes that close; messages already sent may arrive. Closing the peer or losing its UObject owner suppresses callbacks; native teardown completes asynchronously. After closing a whole connection, confirm cleanup with the client’s `bNativeClosed` and the Server’s `LivePeers == 0`; individual channel callbacks may already have been cancelled.

## Run the checks

Rebuild the native library after changing the bridge. Previous artifacts fail the source/hash checks. Mac qualification does not qualify the updated bridge on Android or iOS; rebuild and validate those targets separately.

```sh
python3 tools/run_native_data_probe.py \
  --artifact LocalBuilds/Artifacts/M155-Mac \
  --output Intermediate/NativeDataProbe

python3 tools/run_data_probe.py \
  --server-project "$SERVER_PROJECT" --server-executable "$SERVER_EXE" \
  --client-project "$CLIENT_PROJECT" --client-executable "$CLIENT_EXE" \
  --pairs 4 --output Intermediate/DataProbe
```

Use separately packaged Mac ARM64 Game and dedicated Server apps with the same current artifact; see [building](BUILDING.md) and [Direct signaling](DIRECT_SIGNALING.md). The runner checks saved reports, executable and artifact hashes, distinct process IDs, exact message counts, channel closure and native cleanup. It removes temporary credentials and stops the processes it starts.

The native fixture additionally tests a lower advertised 32 KiB limit, zero-lifetime configuration, a Server-originated channel and explicit receive-overflow failure. The Unreal fixture tests six ordering/reliability combinations, exact Unicode/binary echoes, empty and maximum-size messages, a 32-message ordered burst, rejected sends, copied inputs and queue recovery. These are controlled loopback tests. Packet-loss/expiry behavior, cross-device traffic, mobile packages and public C++/Blueprint session integration need separate acceptance.

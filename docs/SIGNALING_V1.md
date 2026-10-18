# Signaling examples — v1

Signaling is the exchange of messages that sets up a WebRTC connection. These examples describe the planned Direct protocol and selected Janus messages.

**The Direct protocol is still a draft.** The Python tests check its message rules without making a connection. The Unreal transport is not implemented yet. Applications will use the [shared C++/Blueprint API](API_CONTRACT.md); provider JSON stays inside the plugin.

## Read the examples

Files are in [Fixtures/signaling/v1](../Fixtures/signaling/v1).

| File | What it shows |
|---|---|
| `direct-session.json` | Create a session, send ICE and an offer, receive a test answer, poll, then close |
| `janus-videoroom.json` | Create a Janus session/handle, join a room, receive the final event, then clean up |
| `janus-jsep.json` | An EchoTest offer and Janus's ICE message format |
| `data-offer.sdp`, `data-answer.sdp` | Example SDP: the description of a proposed connection |
| `errors.json` | Selected Direct errors and Janus authentication/session errors |
| `manifest.json` | Fixture version, pinned Janus commit and file checksums |

The SDP files contain placeholder fingerprints, ICE credentials and addresses. They cannot establish a real connection. Janus examples also omit the API secret; the [live test](FIXTURES.md) reads it from a local secret file.

Run all contract tests from the repository root:

```sh
python3 -m unittest discover -s tools/tests -v
```

## Direct: the basic flow

1. **Create** a session.
2. Send an **offer**, which describes the connection, and **ICE candidates**, which describe possible network addresses. Candidates may arrive before the offer; the test model queues them.
3. **Poll** for events. The test inserts a sample answer to exercise this step.
4. **Close** the session and clear its pending work.

Each request has an ID and a sequence number. The first sequence is 0. An accepted request advances it by one; a rejected request does not.

Retries are safe within the test model: sending the same request ID and content returns the original result. Changing the content while reusing an ID produces `RequestConflict`. Skipping a sequence produces `OutOfOrder`. Retrying a poll returns its original event snapshot.

A result marked `accepted` means the model recorded the request. It does not mean a peer connected or received data. `tools/signaling_contract.py` is the test model; the future C++ transport should pass the same examples before the protocol is finalized.

## Janus: an ACK is not completion

Janus may acknowledge a plugin request immediately with `janus: "ack"`, then send the result later. The client must wait for the final event and match its transaction, session, handle and plugin.

An error can arrive without an ACK. Unrelated events belong to other handlers, and a repeated final event must not complete a request twice. The `JanusTransaction` test helper checks these rules for plugin messages. It does not implement a full Janus client.

These examples target Janus multistream **1.4.2**. The [local service test](FIXTURES.md) checks the actual HTTP/VideoRoom flow. WebSocket message handling and Unreal-to-Janus media remain untested.

## Direct message reference

Use this section when implementing or reviewing the transport.

<details>
<summary>Request fields, state changes and limits</summary>

Every request contains exactly these fields:

| Field | Rule |
|---|---|
| `protocol` | `"simurghrtc.direct"` |
| `version` | Integer `1` |
| `type` | `create`, `offer`, `ice`, `poll` or `close` |
| `request_id` | 1–64 ASCII letters, digits, underscores or hyphens |
| `sequence` | Integer 0 through 2³¹−1, in order |
| `body` | Fields for the selected request type, listed below |
| `session_id` | Required after `create`; a lowercase UUID string |

Unknown fields and unsupported versions are rejected. Strings must be valid UTF-8 without NUL. Booleans and floating-point values do not count as integers. JSON object-key order does not affect retry matching.

| Request | Exact body fields | Result |
|---|---|---|
| `create` | `purpose`: `data`, `call` or `text-room`; `identity`: 1–128 UTF-8 bytes | State changes from `new` to `open` |
| `offer` | Integer `revision: 1`; `sdp` | State becomes `offer-set`; queued ICE is applied |
| `ice` | `revision: 1`, `candidate`, `sdp_mid`, `sdp_mline_index`, `username_fragment` | Queue before the offer; apply afterward |
| `ice` completion | Exactly `revision: 1`, `completed: true` | Reject further new ICE entries |
| `poll` | Integer `after_event` | Return events after this cursor and `last_event`; 0 starts the stream; a future cursor is invalid |
| `close` | Empty object | Clear ICE/events and enter `closed`; an identical retry succeeds, new operations fail |

The test uses one fixed session UUID for repeatable results. A real server must issue unique IDs and authorize access to them. The `identity` field is metadata, not authentication.

Successful responses echo protocol, version, request ID, sequence and session ID. They also contain `type: "result"`, `status: "accepted"` and the operation's result in `body`.

The test-only `record_answer_fixture()` helper adds an event with an increasing `event_id`, `type: "answer"` and body `{revision: 1, sdp: ...}`. It is not a wire request or an SDP negotiation engine.

| Resource | Limit |
|---|---|
| Compact UTF-8 request | 256 KiB |
| SDP | 128 KiB; starts with `v=0`, uses CRLF line endings and ends with CRLF |
| ICE candidate | 2,048 UTF-8 bytes; starts with `candidate:`; no CR/LF |
| MID | 32 UTF-8 bytes |
| M-line index | Integer 0–31 |
| Username fragment | 256 UTF-8 bytes |
| ICE entries | 32 across queued/applied entries, including the completion marker |
| Saved request results | 128; reject new requests at capacity rather than forget earlier retries |
| Test events | 64; overflow returns `ResourceLimit` |

The model raises `ContractError.code`: `UnsupportedVersion`, `InvalidArgument`, `PayloadTooLarge`, `RequestConflict`, `OutOfOrder`, `InvalidState`, `SessionNotFound`, `SessionClosed`, `IceComplete`, `InvalidCursor` or `ResourceLimit`.

Only the initial offer is modeled. Full SDP parsing, remote ICE events, renegotiation, simultaneous offers, ICE restart, reconnect, authentication and network timeouts remain SR-102 work. HTTP routes and status-code mapping are also undecided. A production session must expire saved results and allow cleanup even when queues are full; the test model is simply discarded after each test.

</details>

## Janus message reference

<details>
<summary>HTTP paths, IDs and SDP/ICE fields</summary>

| HTTP path | Use |
|---|---|
| `/janus` | Create a session |
| `/janus/{session}` | Attach a handle, poll events or destroy the session |
| `/janus/{session}/{handle}` | Send plugin messages or ICE candidates, or detach the handle |

Keep IDs as exact integers. Never convert them through floating point. Examples use small IDs; the test above 2⁵³ checks parser precision, not the range Janus generates.

Janus ICE messages use `sdpMid` and `sdpMLineIndex`. End-of-candidates is `candidate: {completed: true}`. SDP goes in the request's `jsep` field, outside the plugin `body`. The Direct draft uses its own field names; the adapter handles the difference.

References: [Janus REST and transaction rules](https://janus.conf.meetecho.com/docs/rest.html), [VideoRoom messages](https://janus.conf.meetecho.com/docs/videoroom.html). Changes in newer upstream documentation do not automatically change these pinned examples.

</details>

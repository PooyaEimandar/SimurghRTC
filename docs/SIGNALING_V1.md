# Signaling examples — v1

Signaling is the exchange of messages that sets up a WebRTC connection. This page describes Direct HTTP protocol v1 and selected Janus messages.

The internal Direct provider uses Unreal HTTPServer and the HTTP client to negotiate native peers. See [Direct signaling](DIRECT_SIGNALING.md) for usage and process validation. The shared C++/Blueprint session facade is integrated separately in SR-105; provider JSON stays inside the plugin.

## Read the examples

Files are in [Fixtures/signaling/v1](../Fixtures/signaling/v1).

| File | What it shows |
|---|---|
| `direct-http.json` | HTTP endpoint, authentication placeholder, remote ICE/state/error events |
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
2. Send an **offer**, which describes the connection, and **ICE candidates**, which describe possible network addresses. Candidates may arrive before the offer; the native peer queues them.
3. **Poll** for events. The Server publishes its answer, local ICE, connection state and errors as ordered events.
4. **Close** the session and clear its pending work.

For generated audio, use the existing `call` purpose. Both peers create one initially muted Opus source before their first SDP exchange. Offer/answer, ICE, polling and close retain the same v1 envelopes and limits; audio itself uses encrypted WebRTC media packets. This does not implement call invitations or SDP renegotiation. See [Shared audio](SHARED_AUDIO.md).

Each request has an ID and a sequence number. The first sequence is 0. An accepted request advances it by one; a rejected request does not. Close is the exception: it accepts any nonnegative sequence so cancellation can clean up after a lost response.

Within the retained replay window, retries are idempotent: sending the same request ID and content returns the original result. Changing the content while reusing an ID produces `RequestConflict`. Skipping a sequence produces `OutOfOrder`. Retrying a poll returns its original event snapshot.

A result marked `accepted` means the Server recorded the request. Native work finishes asynchronously; a later error event can still fail negotiation. It does not mean a peer connected or received data. `tools/signaling_contract.py` is an offline model of the basic message rules; it does not model native operations, HTTP, authentication or elapsed time. The C++ parser consumes the same versioned request examples.

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

Unknown fields, duplicate JSON keys, malformed UTF-8 and unsupported versions are rejected. Nesting is limited to eight levels. Strings must be valid UTF-8 without NUL. Booleans and floating-point values do not count as integers. JSON object-key order does not affect retry matching.

| Request | Exact body fields | Result |
|---|---|---|
| `create` | `purpose`: `data`, `call` or `text-room`; `identity`: 1–128 UTF-8 bytes | State changes from `new` to `open` |
| `offer` | Integer `revision: 1`; `sdp` | State becomes `offer-set`; queued ICE is applied |
| `ice` | `revision: 1`, `candidate`, `sdp_mid`, `sdp_mline_index`, `username_fragment` | Queue before the offer; submit to the native peer afterward |
| `ice` completion | Exactly `revision: 1`, `completed: true` | Reject further new ICE entries |
| `poll` | Integer `after_event` | Return events after this cursor and `last_event`; 0 starts the stream; a future cursor is invalid |
| `close` | Empty object | Clear ICE/events and earlier replay results; enter `closed`; an identical close retry succeeds, new operations fail |

The fixture uses one fixed UUID. The live Server generates a new UUID and binds it to the authenticated principal. Every request, including polls and close, requires authorization. The `identity` field is metadata, not authentication.

Successful responses echo protocol, version, request ID, sequence and session ID. They also contain `type: "result"`, `status: "accepted"` and the operation's result in `body`.

Events have increasing `event_id`, `type` and `body` fields:

| Event | Body |
|---|---|
| `answer` | `{revision: 1, sdp: ...}` |
| `ice` | The candidate or completion body from the request table |
| `connection` | `{state: "connected"}` or `{state: "disconnected"}` |
| `error` | `{code: "NativeError"}` or `{code: "ResourceLimit"}`; the native peer closes |

ICE events may precede the answer. A poll returns every event after its cursor. Events remain available until the session closes or expires. The test-only `record_answer_fixture()` helper inserts a placeholder answer without negotiating SDP.

The `pending_ice` and `applied_ice` response counts describe accepted wire entries, including completion. They do not confirm native application. Inspect transport state for actual connectivity.

| Resource | Limit |
|---|---|
| Raw HTTP request / response | 256 KiB each |
| SDP | 128 KiB; starts with `v=0`, uses CRLF line endings and ends with CRLF |
| ICE candidate | 2,048 UTF-8 bytes; starts with `candidate:`; no CR/LF |
| MID | 32 UTF-8 bytes |
| M-line index | Integer 0–31 |
| Username fragment | 256 UTF-8 bytes |
| ICE entries | 32 across queued/applied entries, including the completion marker |
| Saved results | 128 after create; at most 2 MiB of serialized replies per session, plus the create result |
| Events | 64; overflow fails the peer with `ResourceLimit` |
| Live peers / retained records | Default 16 / hard limit 64 |
| Idle lease / replay window | Default 30 seconds each; host-configurable from 1–300 seconds |

The model raises `ContractError.code`: `UnsupportedVersion`, `InvalidArgument`, `PayloadTooLarge`, `RequestConflict`, `OutOfOrder`, `InvalidState`, `SessionNotFound`, `SessionClosed`, `IceComplete`, `InvalidCursor` or `ResourceLimit`.

Only the initial offer/answer is supported. Native SDP parsing, remote ICE events, authentication hooks and deadlines are implemented. Renegotiation, simultaneous offers and ICE restart remain unsupported; reconnect creates a fresh session.

A reply is retained for the configured replay window. After eviction, the old sequence is rejected; it is never executed again as a new operation. The create result remains attached to the session and its closed record. Close invalidates other results, but keeps its own retry result and the original create result. Closed records expire after the replay window; a create retry after that retention period may create a new session. Clients must not retry indefinitely.

A full cache rejects new work before accepting its effects. Authenticated close bypasses cache capacity and sequence ordering. Idle sessions close their peers and retain a short closed record. The client sends at most one HTTP operation at a time, retries transport failures up to twice using the same ID/body, and applies an overall connection deadline.

### HTTP and authentication

Use `POST /simurghrtc/v1/signal`, `Content-Type: application/json` and `Authorization: Bearer <credential>`. No query parameters are accepted. The host's admission hook validates credentials on every request, returns a stable principal, and separately approves session creation. The sample credential is local test data; the plugin does not provide an identity service.

An error response uses the normal result envelope with `status: "error"` and `body: {code: ...}`. Unparseable envelopes use an empty request/session ID and sequence 0. Responses are marked `Cache-Control: no-store`.

| HTTP status | Codes |
|---|---|
| 200 | Accepted request or retained replay |
| 400 | `InvalidArgument`, `InvalidCursor` |
| 401 / 403 | `Unauthorized` / `AdmissionRejected` |
| 404 | `SessionNotFound`, including a session owned by another principal |
| 409 | `RequestConflict`, `OutOfOrder`, `InvalidState`, `SessionClosed`, `IceComplete` |
| 413 / 426 | `PayloadTooLarge` / `UnsupportedVersion` |
| 503 | `ResourceLimit`, `Unavailable` |

Use local HTTP only for development. Deploy HTTPServer behind a trusted HTTPS reverse proxy for remote use, with request/header/connection limits and timeouts. HTTPServer buffers requests before plugin handlers run, so the plugin's body check does not replace those proxy limits. WebRTC carries the transport separately from HTTP. See the [deployment notes](DIRECT_SIGNALING.md#deployment).

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

Janus ICE messages use `sdpMid` and `sdpMLineIndex`. End-of-candidates is `candidate: {completed: true}`. SDP goes in the request's `jsep` field, outside the plugin `body`. Direct v1 uses its own field names; the adapter handles the difference.

References: [Janus REST and transaction rules](https://janus.conf.meetecho.com/docs/rest.html), [VideoRoom messages](https://janus.conf.meetecho.com/docs/videoroom.html). Changes in newer upstream documentation do not automatically change these pinned examples.

</details>

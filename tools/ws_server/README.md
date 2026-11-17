# Ubstream Local WebSocket Server

This is a dependency-free local simulator for the UbstreamModeler Unreal plugin.

It listens on:

```text
ws://127.0.0.1:8765
```

It also exposes a local frame viewer at:

```text
http://127.0.0.1:8766/
```

It can:

- Accept WebSocket connections.
- Simulate the real FastStream binary protocol or the legacy `UBS1` simulator protocol.
- Log text JSON messages with authentication credentials redacted.
- Print a compact summary for `modeling_selection_state` messages.
- Accept binary messages.
- Save binary payloads to `tools/ws_server/received/`.
- Parse `UBS1` frame bundles and save color/depth parts separately.
- Send a `returned_frame_header` JSON message followed by the received RGB JPEG as a binary message over the same WebSocket connection.
- Send a JSON ack for every received message.
- Send a mock `generation_result` after a frame bundle.
- Send mock `generation_status` and `generation_result` messages for `generate_request`.
- Return a `file://` result URL pointing at the last saved color image, so the Unreal plugin can test `Import Result` locally.
- Show received RGB/depth frames in the HTTP viewer as a debugging convenience.

## Authentication boundary

This loopback simulator accepts auth messages to exercise the protocol but does
not validate account keys or model production authorization. The plugin still
requires Unbound Loop sign-in. Use a development account. Auth-message contents
and Unbound credential-shaped strings are redacted from text logs; FastStream logs
only token byte counts.

## Run

Legacy `UBS1` simulator mode:

```bash
node tools/ws_server/server.mjs
```

FastStream passthrough mode, matching the real stream backend shape:

```bash
UBSTREAM_WS_PROTOCOL=faststream UBSTREAM_WS_PORT=3002 node tools/ws_server/server.mjs
```

Then point the plugin at:

```text
ws://127.0.0.1:3002/api/ws/ue-editor
```

## Smoke Test

With the server running, in another terminal:

```bash
node tools/ws_server/smoke_client.mjs
```

FastStream smoke test:

```bash
UBSTREAM_SMOKE_PROTOCOL=faststream UBSTREAM_WS_URL=ws://127.0.0.1:3002/api/ws/ue-editor node tools/ws_server/smoke_client.mjs
```

Expected result:

- The server logs a `hello` JSON message.
- The server logs a `modeling_selection_state` JSON message and selected-object summary.
- The server saves one binary payload under `received/`.
- The client logs ack messages plus a returned RGB binary payload.

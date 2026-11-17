const url = process.env.UBSTREAM_WS_URL ?? 'ws://127.0.0.1:8765';
const protocol = (process.env.UBSTREAM_SMOKE_PROTOCOL ?? 'ubs1').toLowerCase();

function waitForOpen(socket) {
  return new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, { once: true });
    socket.addEventListener('error', reject, { once: true });
  });
}

function wait(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

const socket = new WebSocket(url);

socket.addEventListener('message', async (event) => {
  if (typeof event.data === 'string') {
    console.log(`[client] text ${event.data}`);
    return;
  }

  const bytes = new Uint8Array(await event.data.arrayBuffer());
  const magic = Array.from(bytes.slice(0, 8)).map((byte) => byte.toString(16).padStart(2, '0')).join(' ');
  if (protocol === 'faststream') {
    if (bytes[0] === 0x02) {
      console.log(`[client] faststream status=${bytes[1]} json=${new TextDecoder().decode(bytes.slice(2))}`);
      return;
    }
    if (bytes[0] === 0x01) {
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      console.log(`[client] faststream response frame=${view.getUint32(1, true)} imageBytes=${bytes.byteLength - 5}`);
      return;
    }
  }
  console.log(`[client] binary ${bytes.byteLength} bytes magic=${magic}`);
});

await waitForOpen(socket);
console.log(`[client] connected ${url}`);

if (protocol === 'faststream') {
  const params = new TextEncoder().encode(JSON.stringify({
    prompt: 'smoke test frame',
    strength: 0.5,
    depthStrength: 0.5,
    seed: 2
  }));
  const paramsMessage = new Uint8Array(1 + params.byteLength);
  paramsMessage[0] = 0x03;
  paramsMessage.set(params, 1);
  socket.send(paramsMessage);

  const color = new Uint8Array([0xff, 0xd8, 0xff, 0xdb, 0x00, 0x43, 0xff, 0xd9]);
  const depth = new Uint8Array([0xff, 0xd8, 0xff, 0xdb, 0x00, 0x43, 0xff, 0xd9]);
  const message = new Uint8Array(13 + color.byteLength + depth.byteLength);
  const view = new DataView(message.buffer);
  view.setUint8(0, 0x01);
  view.setUint32(1, 1, true);
  view.setUint32(5, color.byteLength, true);
  view.setUint32(9, depth.byteLength, true);
  message.set(color, 13);
  message.set(depth, 13 + color.byteLength);
  socket.send(message);

  await wait(500);
  socket.close();
  process.exit(0);
}

socket.send(JSON.stringify({
  type: 'hello',
  protocol: 'ubstream.websocket.v1',
  client: 'node-smoke-client'
}));

socket.send(JSON.stringify({
  type: 'modeling_selection_state',
  protocol: 'ubstream.websocket.v1',
  sessionId: 'smoke_session',
  objects: [
    {
      actorName: 'SmokeCube',
      actorPath: 'PersistentLevel.SmokeCube',
      className: 'StaticMeshActor',
      type: 'StaticMeshActor',
      assetPath: '/Game/Smoke/SmokeCube',
      tags: ['UbstreamSandbox'],
      bounds: {
        origin: [0, 0, 0],
        extent: [50, 50, 50]
      },
      transform: {
        location: [0, 0, 0],
        rotation: [0, 0, 0],
        scale: [1, 1, 1]
      }
    }
  ]
}));

socket.send(new Uint8Array([0x55, 0x42, 0x53, 0x30, 0x01, 0x02, 0x03, 0x04]));

const header = new TextEncoder().encode(JSON.stringify({
  type: 'frame_header',
  protocol: 'ubstream.websocket.v1',
  sessionId: 'smoke_session',
  frameId: 1,
  width: 2,
  height: 2,
  colorFormat: 'jpeg',
  depthFormat: 'gray_jpeg'
}));
const color = new Uint8Array([0xff, 0xd8, 0xff, 0xdb, 0x00, 0x43, 0xff, 0xd9]);
const depth = new Uint8Array([0xff, 0xd8, 0xff, 0xdb, 0x00, 0x43, 0xff, 0xd9]);
const bundle = new Uint8Array(36 + header.byteLength + color.byteLength + depth.byteLength);
const view = new DataView(bundle.buffer);
bundle.set(new TextEncoder().encode('UBS1'), 0);
view.setUint16(4, 1, true);
view.setUint16(6, 1, true);
view.setBigUint64(8, 1n, true);
view.setUint32(16, 0, true);
view.setUint32(20, header.byteLength, true);
view.setUint32(24, color.byteLength, true);
view.setUint32(28, depth.byteLength, true);
view.setUint32(32, 0, true);
bundle.set(header, 36);
bundle.set(color, 36 + header.byteLength);
bundle.set(depth, 36 + header.byteLength + color.byteLength);
socket.send(bundle);

await wait(500);
socket.close();

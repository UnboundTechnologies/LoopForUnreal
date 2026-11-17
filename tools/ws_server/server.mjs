import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HOST = process.env.UBSTREAM_WS_HOST ?? '127.0.0.1';
const PORT = Number.parseInt(process.env.UBSTREAM_WS_PORT ?? '8765', 10);
const VIEW_HOST = process.env.UBSTREAM_VIEW_HOST ?? HOST;
const VIEW_PORT = Number.parseInt(process.env.UBSTREAM_VIEW_PORT ?? '8766', 10);
const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const PROTOCOL = 'ubstream.websocket.v1';
const MAX_MESSAGE_BYTES = Number.parseInt(process.env.UBSTREAM_WS_MAX_BYTES ?? String(64 * 1024 * 1024), 10);
const SIM_PROTOCOL = (process.env.UBSTREAM_WS_PROTOCOL ?? 'ubs1').toLowerCase();
const FASTSTREAM_WIDTH = Number.parseInt(process.env.UBSTREAM_FASTSTREAM_WIDTH ?? '512', 10);
const FASTSTREAM_HEIGHT = Number.parseInt(process.env.UBSTREAM_FASTSTREAM_HEIGHT ?? '512', 10);
const FASTSTREAM_DEPTH_SUPPORT = process.env.UBSTREAM_FASTSTREAM_DEPTH_SUPPORT !== '0';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const RECEIVED_DIR = path.join(__dirname, 'received');

fs.mkdirSync(RECEIVED_DIR, { recursive: true });

let nextConnectionId = 1;
let nextBinaryId = 1;
const lastFrameByConnection = new Map();
const frameHistory = [];

function timestampForFile() {
  return new Date().toISOString().replace(/[:.]/g, '-');
}

function detectExtension(buffer, fallback = 'bin') {
  if (buffer.length >= 3 && buffer[0] === 0xff && buffer[1] === 0xd8 && buffer[2] === 0xff) {
    return 'jpg';
  }

  if (
    buffer.length >= 8 &&
    buffer[0] === 0x89 &&
    buffer[1] === 0x50 &&
    buffer[2] === 0x4e &&
    buffer[3] === 0x47 &&
    buffer[4] === 0x0d &&
    buffer[5] === 0x0a &&
    buffer[6] === 0x1a &&
    buffer[7] === 0x0a
  ) {
    return 'png';
  }

  return fallback;
}

function escapeHtml(value) {
  return String(value)
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#039;');
}

function getMimeType(filePath) {
  const extension = path.extname(filePath).toLowerCase();
  if (extension === '.jpg' || extension === '.jpeg') return 'image/jpeg';
  if (extension === '.png') return 'image/png';
  if (extension === '.json') return 'application/json; charset=utf-8';
  if (extension === '.html') return 'text/html; charset=utf-8';
  if (extension === '.js') return 'application/javascript; charset=utf-8';
  if (extension === '.css') return 'text/css; charset=utf-8';
  return 'application/octet-stream';
}

function receivedUrl(filePath) {
  return `/received/${encodeURIComponent(path.basename(filePath))}`;
}

function viewerAssetUrl(filePath) {
  return `http://${VIEW_HOST}:${VIEW_PORT}${receivedUrl(filePath)}`;
}

function recordFrame(saved) {
  if (saved.kind !== 'frame_bundle' && saved.kind !== 'faststream_frame') return;

  frameHistory.unshift({
    id: `${Date.now()}-${saved.frameId}`,
    receivedAt: new Date().toISOString(),
    kind: saved.kind,
    frameId: saved.frameId,
    bytes: saved.bytes,
    colorBytes: saved.colorBytes,
    depthBytes: saved.depthBytes,
    width: saved.width,
    height: saved.height,
    headerUrl: saved.headerPath ? receivedUrl(saved.headerPath) : null,
    colorUrl: receivedUrl(saved.colorPath),
    depthUrl: saved.depthPath ? receivedUrl(saved.depthPath) : null,
    colorAbsoluteUrl: viewerAssetUrl(saved.colorPath),
    depthAbsoluteUrl: saved.depthPath ? viewerAssetUrl(saved.depthPath) : null
  });

  frameHistory.splice(200);
}

function renderViewerHtml() {
  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Ubstream Frame Viewer</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #111316;
      --panel: #191d22;
      --muted: #9ca3af;
      --text: #f3f4f6;
      --line: #2d333b;
      --accent: #7dd3fc;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      background: var(--bg);
      color: var(--text);
      font: 14px/1.45 ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }
    header {
      position: sticky;
      top: 0;
      z-index: 10;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 16px;
      padding: 14px 18px;
      border-bottom: 1px solid var(--line);
      background: rgba(17, 19, 22, 0.95);
      backdrop-filter: blur(10px);
    }
    h1 {
      margin: 0;
      font-size: 18px;
      font-weight: 650;
    }
    .status { color: var(--muted); }
    main { padding: 18px; }
    .empty {
      padding: 32px;
      border: 1px solid var(--line);
      background: var(--panel);
      border-radius: 8px;
      color: var(--muted);
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(420px, 1fr));
      gap: 16px;
    }
    article {
      overflow: hidden;
      border: 1px solid var(--line);
      background: var(--panel);
      border-radius: 8px;
    }
    .meta {
      display: flex;
      flex-wrap: wrap;
      gap: 8px 14px;
      padding: 12px;
      border-bottom: 1px solid var(--line);
      color: var(--muted);
      font-size: 12px;
    }
    .meta strong { color: var(--text); }
    .images {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 1px;
      background: var(--line);
    }
    figure {
      margin: 0;
      min-width: 0;
      background: #0b0d10;
    }
    figcaption {
      padding: 8px 10px;
      color: var(--muted);
      font-size: 12px;
      border-bottom: 1px solid var(--line);
    }
    img {
      display: block;
      width: 100%;
      height: auto;
      image-rendering: auto;
    }
    a { color: var(--accent); text-decoration: none; }
    a:hover { text-decoration: underline; }
  </style>
</head>
<body>
  <header>
    <h1>Ubstream Frame Viewer</h1>
    <div class="status" id="status">Waiting for frames...</div>
  </header>
  <main id="frames"><div class="empty">No frames received yet. Open Unreal, connect the Ubstream panel, and let streaming start.</div></main>
  <script>
    const framesEl = document.getElementById('frames');
    const statusEl = document.getElementById('status');

    function formatBytes(bytes) {
      if (!Number.isFinite(bytes)) return '';
      if (bytes < 1024) return bytes + ' B';
      if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
      return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
    }

    function render(frames) {
      statusEl.textContent = frames.length
        ? frames.length + ' frame' + (frames.length === 1 ? '' : 's') + ' in this server session'
        : 'Waiting for frames...';

      if (!frames.length) {
        framesEl.innerHTML = '<div class="empty">No frames received yet. Open Unreal, connect the Ubstream panel, and let streaming start.</div>';
        return;
      }

      framesEl.innerHTML = '<div class="grid">' + frames.map((frame) => {
        const size = frame.width && frame.height ? frame.width + 'x' + frame.height : 'unknown size';
        return '<article>' +
          '<div class="meta">' +
            '<span><strong>Frame</strong> ' + frame.frameId + '</span>' +
            '<span><strong>Size</strong> ' + size + '</span>' +
            '<span><strong>Bundle</strong> ' + formatBytes(frame.bytes) + '</span>' +
            '<span><strong>RGB</strong> ' + formatBytes(frame.colorBytes) + '</span>' +
            '<span><strong>Depth</strong> ' + formatBytes(frame.depthBytes) + '</span>' +
            (frame.headerUrl ? '<span><a href="' + frame.headerUrl + '" target="_blank" rel="noreferrer">header JSON</a></span>' : '') +
          '</div>' +
          '<div class="images">' +
            '<figure><figcaption>RGB</figcaption><a href="' + frame.colorUrl + '" target="_blank" rel="noreferrer"><img src="' + frame.colorUrl + '?t=' + encodeURIComponent(frame.receivedAt) + '" alt="RGB frame ' + frame.frameId + '"></a></figure>' +
            (frame.depthUrl
              ? '<figure><figcaption>Depth</figcaption><a href="' + frame.depthUrl + '" target="_blank" rel="noreferrer"><img src="' + frame.depthUrl + '?t=' + encodeURIComponent(frame.receivedAt) + '" alt="Depth frame ' + frame.frameId + '"></a></figure>'
              : '<figure><figcaption>Depth</figcaption><div class="empty">No depth payload</div></figure>') +
          '</div>' +
        '</article>';
      }).join('') + '</div>';
    }

    async function refresh() {
      try {
        const response = await fetch('/frames', { cache: 'no-store' });
        const payload = await response.json();
        render(payload.frames || []);
      } catch (error) {
        statusEl.textContent = 'Viewer refresh failed: ' + error.message;
      }
    }

    refresh();
    setInterval(refresh, 1000);
  </script>
</body>
</html>`;
}

function sendHttp(res, statusCode, body, contentType = 'text/plain; charset=utf-8') {
  const payload = Buffer.isBuffer(body) ? body : Buffer.from(String(body));
  res.writeHead(statusCode, {
    'content-type': contentType,
    'content-length': payload.length,
    'cache-control': 'no-store'
  });
  res.end(payload);
}

function handleViewerRequest(req, res) {
  const requestUrl = new URL(req.url, `http://${req.headers.host ?? `${VIEW_HOST}:${VIEW_PORT}`}`);

  if (requestUrl.pathname === '/') {
    sendHttp(res, 200, renderViewerHtml(), 'text/html; charset=utf-8');
    return;
  }

  if (requestUrl.pathname === '/frames') {
    sendHttp(res, 200, JSON.stringify({ frames: frameHistory }, null, 2), 'application/json; charset=utf-8');
    return;
  }

  if (requestUrl.pathname.startsWith('/received/')) {
    const requestedName = decodeURIComponent(requestUrl.pathname.slice('/received/'.length));
    const safeName = path.basename(requestedName);
    if (safeName !== requestedName) {
      sendHttp(res, 400, 'Invalid received file name');
      return;
    }

    const filePath = path.join(RECEIVED_DIR, safeName);
    if (!filePath.startsWith(RECEIVED_DIR) || !fs.existsSync(filePath)) {
      sendHttp(res, 404, 'Not found');
      return;
    }

    sendHttp(res, 200, fs.readFileSync(filePath), getMimeType(filePath));
    return;
  }

  sendHttp(res, 404, 'Not found');
}

function sendFrame(socket, opcode, payload) {
  const data = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  let header;

  if (data.length < 126) {
    header = Buffer.alloc(2);
    header[1] = data.length;
  } else if (data.length <= 0xffff) {
    header = Buffer.alloc(4);
    header[1] = 126;
    header.writeUInt16BE(data.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(data.length), 2);
  }

  header[0] = 0x80 | opcode;
  socket.write(Buffer.concat([header, data]));
}

function sendJson(socket, message) {
  sendFrame(socket, 0x1, JSON.stringify(message));
}

function sendFastStreamStatus(socket, status, payload = null) {
  const body = payload ? Buffer.from(JSON.stringify(payload), 'utf8') : Buffer.alloc(0);
  sendFrame(socket, 0x2, Buffer.concat([Buffer.from([0x02, status]), body]));
}

function sendFastStreamReady(socket) {
  sendFastStreamStatus(socket, 0x01, {
    protocol_version: 1,
    model_name: 'local-faststream-passthrough',
    width: FASTSTREAM_WIDTH,
    height: FASTSTREAM_HEIGHT,
    depth_support: FASTSTREAM_DEPTH_SUPPORT
  });
}

function writeUInt32LE(value) {
  const buffer = Buffer.alloc(4);
  buffer.writeUInt32LE(value >>> 0, 0);
  return buffer;
}

function sendClose(socket, code = 1000, reason = '') {
  const reasonBuffer = Buffer.from(reason);
  const payload = Buffer.alloc(2 + reasonBuffer.length);
  payload.writeUInt16BE(code, 0);
  reasonBuffer.copy(payload, 2);
  sendFrame(socket, 0x8, payload);
  socket.end();
}

function parseHeaders(rawHeader) {
  const lines = rawHeader.split('\r\n');
  const requestLine = lines.shift();
  const headers = new Map();

  for (const line of lines) {
    const separator = line.indexOf(':');
    if (separator === -1) continue;
    headers.set(line.slice(0, separator).trim().toLowerCase(), line.slice(separator + 1).trim());
  }

  return { requestLine, headers };
}

function acceptHandshake(socket, rawHeader, connectionId) {
  const { requestLine, headers } = parseHeaders(rawHeader);
  const key = headers.get('sec-websocket-key');

  if (!requestLine?.startsWith('GET ') || !key) {
    socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
    return false;
  }

  const accept = crypto.createHash('sha1').update(`${key}${GUID}`).digest('base64');
  const response = [
    'HTTP/1.1 101 Switching Protocols',
    'Upgrade: websocket',
    'Connection: Upgrade',
    `Sec-WebSocket-Accept: ${accept}`,
    '\r\n'
  ].join('\r\n');

  socket.write(response);
  console.log(`[ws:${connectionId}] connected ${requestLine}`);

  if (SIM_PROTOCOL === 'faststream') {
    sendFastStreamReady(socket);
  } else {
    sendJson(socket, {
      type: 'hello',
      protocol: PROTOCOL,
      server: 'ubstream-local-ws',
      connectionId
    });
  }
  return true;
}

function parseBundle(buffer) {
  if (buffer.length < 36 || buffer.subarray(0, 4).toString('ascii') !== 'UBS1') {
    return null;
  }

  const version = buffer.readUInt16LE(4);
  const messageType = buffer.readUInt16LE(6);
  const frameId = Number(buffer.readBigUInt64LE(8));
  const flags = buffer.readUInt32LE(16);
  const headerJsonBytes = buffer.readUInt32LE(20);
  const colorBytes = buffer.readUInt32LE(24);
  const depthBytes = buffer.readUInt32LE(28);
  const reserved = buffer.readUInt32LE(32);
  const expectedSize = 36 + headerJsonBytes + colorBytes + depthBytes;

  if (expectedSize > buffer.length) {
    throw new Error(`Incomplete UBS1 bundle: expected ${expectedSize} bytes, got ${buffer.length}`);
  }

  const headerStart = 36;
  const colorStart = headerStart + headerJsonBytes;
  const depthStart = colorStart + colorBytes;
  const headerText = buffer.subarray(headerStart, colorStart).toString('utf8');
  const header = headerText ? JSON.parse(headerText) : {};

  return {
    version,
    messageType,
    frameId,
    flags,
    reserved,
    header,
    color: buffer.subarray(colorStart, depthStart),
    depth: buffer.subarray(depthStart, depthStart + depthBytes)
  };
}

function saveBinaryPayload(buffer, connectionId) {
  const id = nextBinaryId++;
  const stamp = timestampForFile();

  try {
    const bundle = parseBundle(buffer);
    if (bundle) {
      const baseName = `${stamp}_conn${connectionId}_frame${bundle.frameId}`;
      const headerPath = path.join(RECEIVED_DIR, `${baseName}_header.json`);
      const colorExt = bundle.header.colorFormat === 'jpeg' ? 'jpg' : detectExtension(bundle.color);
      const depthExt = bundle.header.depthFormat?.includes('png') ? 'png' : detectExtension(bundle.depth);
      const colorPath = path.join(RECEIVED_DIR, `${baseName}_color.${colorExt}`);
      const depthPath = path.join(RECEIVED_DIR, `${baseName}_depth.${depthExt}`);

      fs.writeFileSync(headerPath, JSON.stringify({
        version: bundle.version,
        messageType: bundle.messageType,
        frameId: bundle.frameId,
        flags: bundle.flags,
        reserved: bundle.reserved,
        header: bundle.header
      }, null, 2));
      fs.writeFileSync(colorPath, bundle.color);
      fs.writeFileSync(depthPath, bundle.depth);

      return {
        kind: 'frame_bundle',
        frameId: bundle.frameId,
        bytes: buffer.length,
        header: bundle.header,
        colorBytes: bundle.color.length,
        depthBytes: bundle.depth.length,
        width: bundle.header.width,
        height: bundle.header.height,
        colorFormat: bundle.header.colorFormat ?? colorExt,
        depthFormat: bundle.header.depthFormat ?? depthExt,
        headerPath,
        colorPath,
        depthPath
      };
    }
  } catch (error) {
    console.warn(`[ws:${connectionId}] failed to parse UBS1 bundle: ${error.message}`);
  }

  const ext = detectExtension(buffer);
  const payloadPath = path.join(RECEIVED_DIR, `${stamp}_conn${connectionId}_binary${id}.${ext}`);
  fs.writeFileSync(payloadPath, buffer);

  return {
    kind: 'binary',
    frameId: null,
    bytes: buffer.length,
    payloadPath
  };
}

function parseFastStreamFrame(buffer) {
  if (buffer.length < 13 || buffer[0] !== 0x01) {
    return null;
  }

  const frameId = buffer.readUInt32LE(1);
  const colorBytes = buffer.readUInt32LE(5);
  const depthBytes = buffer.readUInt32LE(9);
  const expectedSize = 13 + colorBytes + depthBytes;
  if (expectedSize > buffer.length) {
    throw new Error(`Incomplete FastStream frame: expected ${expectedSize} bytes, got ${buffer.length}`);
  }

  return {
    frameId,
    color: buffer.subarray(13, 13 + colorBytes),
    depth: buffer.subarray(13 + colorBytes, expectedSize)
  };
}

function saveFastStreamFrame(buffer, connectionId) {
  const frame = parseFastStreamFrame(buffer);
  if (!frame) return null;

  const stamp = timestampForFile();
  const baseName = `${stamp}_conn${connectionId}_faststream_frame${frame.frameId}`;
  const headerPath = path.join(RECEIVED_DIR, `${baseName}_header.json`);
  const colorExt = detectExtension(frame.color, 'jpg');
  const depthExt = frame.depth.length > 0 ? detectExtension(frame.depth, 'jpg') : null;
  const colorPath = path.join(RECEIVED_DIR, `${baseName}_color.${colorExt}`);
  const depthPath = depthExt ? path.join(RECEIVED_DIR, `${baseName}_depth.${depthExt}`) : null;

  fs.writeFileSync(headerPath, JSON.stringify({
    protocol: 'faststream.v1',
    frameId: frame.frameId,
    colorBytes: frame.color.length,
    depthBytes: frame.depth.length,
    width: FASTSTREAM_WIDTH,
    height: FASTSTREAM_HEIGHT
  }, null, 2));
  fs.writeFileSync(colorPath, frame.color);
  if (depthPath) {
    fs.writeFileSync(depthPath, frame.depth);
  }

  return {
    kind: 'faststream_frame',
    frameId: frame.frameId,
    bytes: buffer.length,
    colorBytes: frame.color.length,
    depthBytes: frame.depth.length,
    width: FASTSTREAM_WIDTH,
    height: FASTSTREAM_HEIGHT,
    colorFormat: colorExt,
    depthFormat: depthExt,
    headerPath,
    colorPath,
    depthPath
  };
}

function handleFastStreamBinary(socket, connectionId, payload) {
  if (payload.length === 0) return;

  const type = payload[0];
  if (type === 0x01) {
    let saved;
    try {
      saved = saveFastStreamFrame(payload, connectionId);
    } catch (error) {
      console.warn(`[ws:${connectionId}] failed to parse FastStream frame: ${error.message}`);
      return;
    }

    if (!saved) {
      console.warn(`[ws:${connectionId}] invalid FastStream frame`);
      return;
    }

    recordFrame(saved);
    lastFrameByConnection.set(connectionId, saved);
    console.log(
      `[ws:${connectionId}] faststream frame=${saved.frameId} color=${saved.colorBytes}B depth=${saved.depthBytes}B`
    );

    const returnedColor = fs.readFileSync(saved.colorPath);
    sendFrame(socket, 0x2, Buffer.concat([
      Buffer.from([0x01]),
      writeUInt32LE(saved.frameId),
      returnedColor
    ]));
    return;
  }

  if (type === 0x02) {
    const frameId = payload.length >= 5 ? payload.readUInt32LE(1) : 0;
    console.log(`[ws:${connectionId}] faststream flush frame=${frameId}`);
    return;
  }

  if (type === 0x03) {
    const text = payload.subarray(1).toString('utf8');
    console.log(`[ws:${connectionId}] faststream params ${text}`);
    return;
  }

  if (type === 0x04) {
    console.log(`[ws:${connectionId}] faststream auth token bytes=${payload.length - 1}`);
    sendFastStreamReady(socket);
    return;
  }

  console.warn(`[ws:${connectionId}] unknown FastStream message type 0x${type.toString(16).padStart(2, '0')}`);
}

function createFrameParser(socket, connectionId) {
  let pending = Buffer.alloc(0);
  let fragmentOpcode = null;
  let fragmentParts = [];

  function dispatchMessage(opcode, payload) {
    if (opcode === 0x1) {
      const text = payload.toString('utf8');

      let parsed = null;
      try {
        parsed = JSON.parse(text);
      } catch {
        // Plain text is still valid for smoke testing.
      }

      const logText = parsed?.type === 'auth'
        ? 'auth [credential redacted]'
        : text.replace(/ub(?:ue|ud)_[a-f0-9]{20}_[A-Za-z0-9_-]{43}/g, '[credential redacted]');
      console.log(`[ws:${connectionId}] text ${logText}`);

      sendJson(socket, {
        type: 'ack',
        ackType: parsed?.type ?? 'text',
        frameId: parsed?.frameId ?? null,
        status: 'ok'
      });

      if (parsed?.type === 'modeling_selection_state') {
        const objects = Array.isArray(parsed.objects) ? parsed.objects : [];
        console.log(`[ws:${connectionId}] selection objects=${objects.length}`);
        for (const object of objects.slice(0, 8)) {
          console.log(
            `[ws:${connectionId}] selected ${object.actorName ?? '<unnamed>'} type=${object.type ?? '<unknown>'} asset=${object.assetPath ?? ''}`
          );
        }
      }

      if (parsed?.type === 'generate_request') {
        const lastFrame = lastFrameByConnection.get(connectionId);
        sendJson(socket, {
          type: 'generation_status',
          sessionId: parsed.sessionId ?? 'local_mock_session',
          frameId: parsed.frameId ?? null,
          status: 'queued',
          message: 'Local server accepted generate_request'
        });

        sendJson(socket, {
          type: 'generation_result',
          sessionId: parsed.sessionId ?? 'local_mock_session',
          frameId: parsed.frameId ?? null,
          status: 'complete',
          result: {
            kind: 'mock',
            url: lastFrame?.colorPath ? viewerAssetUrl(lastFrame.colorPath) : undefined,
            fileUrl: lastFrame?.colorPath ? pathToFileURL(lastFrame.colorPath).href : undefined,
            prompt: parsed.prompt ?? '',
            message: 'Local server completed mock generation'
          }
        });
      }
      return;
    }

    if (opcode === 0x2) {
      if (SIM_PROTOCOL === 'faststream') {
        handleFastStreamBinary(socket, connectionId, payload);
        return;
      }

      const saved = saveBinaryPayload(payload, connectionId);
      recordFrame(saved);
      console.log(`[ws:${connectionId}] binary ${saved.bytes} bytes saved`, saved);
      if (saved.kind === 'frame_bundle') {
        console.log(
          `[ws:${connectionId}] stream frame_bundle frame=${saved.frameId} size=${saved.width}x${saved.height} color=${saved.colorBytes}B depth=${saved.depthBytes}B`
        );
      }

      const logText = parsed?.type === 'auth'
        ? 'auth [credential redacted]'
        : text.replace(/ub(?:ue|ud)_[a-f0-9]{20}_[A-Za-z0-9_-]{43}/g, '[credential redacted]');
      console.log(`[ws:${connectionId}] text ${logText}`);

      sendJson(socket, {
        type: 'ack',
        ackType: saved.kind,
        frameId: saved.frameId,
        bytes: saved.bytes,
        status: 'ok'
      });

      if (saved.kind === 'frame_bundle') {
        lastFrameByConnection.set(connectionId, saved);
        const returnedColor = fs.readFileSync(saved.colorPath);
        sendJson(socket, {
          type: 'returned_frame_header',
          protocol: PROTOCOL,
          sessionId: saved.header?.sessionId ?? 'local_mock_session',
          frameId: saved.frameId,
          payloadType: 'rgb',
          format: saved.colorFormat,
          bytes: returnedColor.length,
          width: saved.width,
          height: saved.height,
          message: 'Local server passthrough of received RGB frame'
        });
        sendFrame(socket, 0x2, returnedColor);

        sendJson(socket, {
          type: 'generation_result',
          sessionId: 'local_mock_session',
          frameId: saved.frameId,
          status: 'complete',
          result: {
            kind: 'mock',
            url: saved.colorPath ? viewerAssetUrl(saved.colorPath) : undefined,
            fileUrl: saved.colorPath ? pathToFileURL(saved.colorPath).href : undefined,
            message: 'Local server received frame bundle'
          }
        });
      }
    }
  }

  return function parseIncoming(data) {
    pending = Buffer.concat([pending, data]);

    while (pending.length >= 2) {
      const first = pending[0];
      const second = pending[1];
      const fin = (first & 0x80) !== 0;
      const opcode = first & 0x0f;
      const masked = (second & 0x80) !== 0;
      let payloadLength = second & 0x7f;
      let offset = 2;

      if (payloadLength === 126) {
        if (pending.length < offset + 2) return;
        payloadLength = pending.readUInt16BE(offset);
        offset += 2;
      } else if (payloadLength === 127) {
        if (pending.length < offset + 8) return;
        const bigLength = pending.readBigUInt64BE(offset);
        if (bigLength > BigInt(Number.MAX_SAFE_INTEGER)) {
          sendClose(socket, 1009, 'Payload too large');
          return;
        }
        payloadLength = Number(bigLength);
        offset += 8;
      }

      if (payloadLength > MAX_MESSAGE_BYTES) {
        sendClose(socket, 1009, 'Payload too large');
        return;
      }

      const maskLength = masked ? 4 : 0;
      if (pending.length < offset + maskLength + payloadLength) return;

      const mask = masked ? pending.subarray(offset, offset + 4) : null;
      offset += maskLength;

      const payload = Buffer.from(pending.subarray(offset, offset + payloadLength));
      pending = pending.subarray(offset + payloadLength);

      if (masked && mask) {
        for (let i = 0; i < payload.length; i += 1) {
          payload[i] ^= mask[i % 4];
        }
      }

      if (opcode === 0x8) {
        sendClose(socket);
        return;
      }

      if (opcode === 0x9) {
        sendFrame(socket, 0xA, payload);
        continue;
      }

      if (opcode === 0xA) {
        continue;
      }

      if (opcode === 0x0) {
        if (fragmentOpcode === null) {
          sendClose(socket, 1002, 'Unexpected continuation');
          return;
        }
        fragmentParts.push(payload);
        if (fin) {
          dispatchMessage(fragmentOpcode, Buffer.concat(fragmentParts));
          fragmentOpcode = null;
          fragmentParts = [];
        }
        continue;
      }

      if (!fin) {
        fragmentOpcode = opcode;
        fragmentParts = [payload];
        continue;
      }

      dispatchMessage(opcode, payload);
    }
  };
}

const server = net.createServer((socket) => {
  const connectionId = nextConnectionId++;
  let handshakeBuffer = Buffer.alloc(0);
  let handshaken = false;
  let parseFrames = null;

  socket.on('data', (data) => {
    if (handshaken) {
      parseFrames(data);
      return;
    }

    handshakeBuffer = Buffer.concat([handshakeBuffer, data]);
    const headerEnd = handshakeBuffer.indexOf('\r\n\r\n');
    if (headerEnd === -1) return;

    const rawHeader = handshakeBuffer.subarray(0, headerEnd).toString('utf8');
    const remaining = handshakeBuffer.subarray(headerEnd + 4);
    handshaken = acceptHandshake(socket, rawHeader, connectionId);

    if (!handshaken) return;

    parseFrames = createFrameParser(socket, connectionId);
    if (remaining.length > 0) parseFrames(remaining);
  });

  socket.on('error', (error) => {
    console.warn(`[ws:${connectionId}] socket error: ${error.message}`);
  });

  socket.on('close', () => {
    lastFrameByConnection.delete(connectionId);
    console.log(`[ws:${connectionId}] closed`);
  });
});

server.listen(PORT, HOST, () => {
  console.log(`Ubstream local WebSocket server listening on ws://${HOST}:${PORT}`);
  console.log(`Protocol mode: ${SIM_PROTOCOL}`);
  console.log(`Saving binary payloads to ${RECEIVED_DIR}`);
});

const viewerServer = http.createServer(handleViewerRequest);

viewerServer.listen(VIEW_PORT, VIEW_HOST, () => {
  console.log(`Ubstream frame viewer listening on http://${VIEW_HOST}:${VIEW_PORT}`);
});

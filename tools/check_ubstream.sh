#!/usr/bin/env zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

echo "[check] whitespace"
git diff --check

echo "[check] websocket simulator syntax"
node --check tools/ws_server/server.mjs
node --check tools/ws_server/smoke_client.mjs

echo "[check] policy scan"
rg -n "SpawnActor|PixelStreaming|WebRTC|ReadPixels|ReadLinearColorPixels|bCaptureEveryFrame\\s*=\\s*true|base64|Base64" \
  Plugins/UbstreamModeler docs tools \
  -g '!tools/ws_server/received/**' \
  -g '!tools/check_ubstream.sh' || true

echo "[check] Unreal build command"
echo "DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer '/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh' UbstreamPOCEditor Mac Development -Project='$ROOT_DIR/UbstreamPOC.uproject' -WaitMutex -NoHotReload"

# Loop for Unreal

Unreal Engine editor plugin for [Unbound Loop](https://loop.unbound.io). Generate live
AI concept previews from your viewport, then turn images into 3D meshes, without
leaving the editor.

- Stream an isolated modeling stage or Unreal's Modeling Mode viewport and get
  generated frames back live
- Generate textured 3D meshes with Unbound Zero and import them as assets
- Block out with built-in CSG and sculpt tools

> Beta. Requires an Unbound Loop account.

## Requirements

- Unreal Engine 5.7
- Windows (Win64) or macOS

## Install

**From a release:** download the ZIP for your engine version and platform from
[Releases](https://github.com/UnboundTechnologies/LoopForUnreal/releases), unzip it into
your project's `Plugins/` folder, and restart the editor.

**From source:** copy `Plugins/UbstreamModeler` into your project's `Plugins/` folder,
or open `UbstreamPOC.uproject` from this repo. To build a distributable ZIP:

```bash
python3 tools/package_plugin.py --engine "/path/to/UE_5.7"
```

## Usage

1. Open **Window → Unbound Loop**.
2. Choose **Continue in browser** and sign in. Your key is stored in the OS
   credential store, never in project files.
3. Type a prompt and start streaming.

## Development

A local WebSocket simulator for testing without the backend lives in
[`tools/ws_server`](tools/ws_server/README.md):

```bash
node tools/ws_server/server.mjs
```

## Support

Questions and bug reports: [GitHub Issues](https://github.com/UnboundTechnologies/LoopForUnreal/issues)

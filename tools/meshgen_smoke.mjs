#!/usr/bin/env node
/**
 * Smoke-test the Unbound Zero image-to-mesh task API (/api/v2), the way
 * the plugin calls it: create a task, then wait on the download.
 * Each run creates one billed task (1 credit).
 *
 * Env:
 *   MESHGEN_BASE_URL  default https://unbound-3dgen-server-.../api/v2
 *   MESHGEN_TOKEN     required Unbound ubue_ account key
 *   MESHGEN_IMAGE     path to png/jpg/webp (optional; uses a tiny PNG if unset)
 *   MESHGEN_SEED      default 42
 *   MESHGEN_QUADS     default 5000 (1000-20000)
 *   MESHGEN_NORMAL_MAP default true
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const BASE =
  process.env.MESHGEN_BASE_URL?.replace(/\/$/, "") ||
  "https://unbound-3dgen-server-618775028388.us-central1.run.app/api/v2";
const TOKEN = process.env.MESHGEN_TOKEN || "";
const SEED = Number(process.env.MESHGEN_SEED || 42);
const QUADS = Number(process.env.MESHGEN_QUADS || 5000);
const NORMAL_MAP = (process.env.MESHGEN_NORMAL_MAP || "true") !== "false";
const OUT_DIR = path.join(__dirname, "meshgen_received");
const MIN_POLL_MS = 5000;

function tinyPng() {
  // 1x1 PNG
  return Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==",
    "base64"
  );
}

function authHeaders() {
  return { Authorization: `Bearer ${TOKEN}` };
}

async function main() {
  if (!TOKEN) throw new Error("Set MESHGEN_TOKEN to an Unbound account key (ubue_...).");
  fs.mkdirSync(OUT_DIR, { recursive: true });

  let imageBuf;
  let filename = "smoke.png";
  if (process.env.MESHGEN_IMAGE) {
    imageBuf = fs.readFileSync(process.env.MESHGEN_IMAGE);
    filename = path.basename(process.env.MESHGEN_IMAGE);
  } else {
    imageBuf = tinyPng();
    console.warn("No MESHGEN_IMAGE set; using 1x1 PNG (may fail generation).");
  }

  // The same fields the plugin sends (UbstreamMeshGenService::BuildMultipartBody).
  const form = new FormData();
  form.append("image", new Blob([imageBuf]), filename);
  form.append("model", "unbound-zero");
  form.append("material", "pbr");
  form.append("quads", String(QUADS));
  form.append("normal_map", String(NORMAL_MAP));
  form.append("seed", String(SEED));

  console.log(`POST ${BASE}/tasks`);
  const createRes = await fetch(`${BASE}/tasks`, {
    method: "POST",
    headers: authHeaders(),
    body: form,
  });
  const createText = await createRes.text();
  console.log(`create HTTP ${createRes.status}: ${createText.slice(0, 400)}`);
  if (!createRes.ok) {
    process.exit(1);
  }
  const taskId = JSON.parse(createText).task_id;
  if (!taskId) {
    console.error("missing task_id");
    process.exit(1);
  }

  // The download holds open until the mesh is ready (up to 60 s), then 409.
  const started = Date.now();
  let buf;
  for (;;) {
    const waitStarted = Date.now();
    const dlRes = await fetch(`${BASE}/tasks/${taskId}/download?wait=60`, {
      headers: authHeaders(),
    });
    const elapsed = ((Date.now() - started) / 1000).toFixed(1);
    if (dlRes.ok) {
      buf = Buffer.from(await dlRes.arrayBuffer());
      console.log(`download HTTP 200 (+${elapsed}s)`);
      break;
    }
    if (dlRes.status !== 409) {
      console.error(`download HTTP ${dlRes.status}: ${(await dlRes.text()).slice(0, 400)}`);
      process.exit(1);
    }
    const statusRes = await fetch(`${BASE}/tasks/${taskId}`, { headers: authHeaders() });
    const statusBody = await statusRes.json();
    console.log(`not ready (+${elapsed}s): status=${statusBody.status} error=${statusBody.error ?? null}`);
    if (statusBody.status === "failed") {
      process.exit(1);
    }
    if (Date.now() - started > 15 * 60 * 1000) {
      console.error("timeout");
      process.exit(1);
    }
    const sinceWait = Date.now() - waitStarted;
    if (sinceWait < MIN_POLL_MS) await new Promise((r) => setTimeout(r, MIN_POLL_MS - sinceWait));
  }

  const outPath = path.join(OUT_DIR, `${taskId}.glb`);
  fs.writeFileSync(outPath, buf);
  console.log(`wrote ${outPath} (${buf.length} bytes)`);
  if (buf.length >= 4 && buf.toString("utf8", 0, 4) === "glTF") {
    console.log("ok: glTF magic present");
  } else {
    console.warn("warning: missing glTF magic");
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});

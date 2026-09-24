#!/usr/bin/env node
// Manual WebSocket/node-action test. Uses isolated settings and a local page
// whose load counter proves that reload actually reached CEF.
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, open, readFile, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { setTimeout as delay } from "node:timers/promises";

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const binary = path.resolve(
  process.argv[2] || path.join(root, "build/miximus"),
);
const cefDisabled = process.argv.includes("--cef-disabled");
const api = "http://127.0.0.1:7351/api/v1";
try {
  const response = await fetch(`${api}/config`);
  if (response.ok)
    throw new Error(
      "An application is already serving the API; refusing to replace it",
    );
} catch (error) {
  if (!(error instanceof TypeError)) throw error;
}
const work = await mkdtemp(path.join(tmpdir(), "miximus-node-actions-"));
let loads = 0;
const page = createServer((request, response) => {
  if (request.url !== "/") {
    response.writeHead(404);
    response.end();
    return;
  }
  ++loads;
  response.writeHead(200, {
    "Content-Type": "text/html",
    "Cache-Control": "no-store",
  });
  response.end(`<body style="background:red">Load ${loads}</body>`);
});
page.listen(0, "127.0.0.1");
await once(page, "listening");
const settings = path.join(work, "settings.json");
await writeFile(
  settings,
  JSON.stringify({
    schema_version: 1,
    nodes: [
      {
        id: "browser",
        type: "cef_browser",
        schema_version: 1,
        options: {
          enabled: false,
          url: `http://127.0.0.1:${page.address().port}/`,
          size: [320, 180],
        },
      },
    ],
    connections: [],
  }),
);
const logPath = path.join(work, "app.log");
const log = await open(logPath, "w");
const app = spawn(binary, ["--settings", settings, "--stop-after", "90"], {
  cwd: root,
  stdio: ["ignore", log.fd, log.fd],
});
const exited = once(app, "exit");
let socket;
const pending = new Map();
let nextToken = 0;
async function until(predicate) {
  const deadline = Date.now() + 20000;
  while (Date.now() < deadline) {
    if (app.exitCode !== null) throw new Error(`App exited: ${app.exitCode}`);
    if (await predicate()) return;
    await delay(50);
  }
  throw new Error("Timed out waiting for state");
}
async function config() {
  const response = await fetch(`${api}/config`);
  const json = await response.json();
  return json.config || json;
}
function request(message) {
  const token = `test-${++nextToken}`;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(token);
      reject(new Error(`No reply for ${token}`));
    }, 10000);
    pending.set(token, { resolve, reject, timer });
    socket.send(JSON.stringify({ ...message, action: "command", token }));
  });
}
const action = (name, payload = {}, id = "browser") =>
  request({ topic: "node_action", id, name, payload });
const update = async (options) => {
  assert.equal(
    (await request({ topic: "update_node", id: "browser", options })).action,
    "result",
  );
};
try {
  await until(async () => {
    try {
      return (await fetch(`${api}/config`)).ok;
    } catch {
      return false;
    }
  });
  socket = new WebSocket("ws://127.0.0.1:7351/");
  const connected = new Promise((resolve, reject) => {
    socket.addEventListener("error", reject, { once: true });
    socket.addEventListener("message", ({ data }) => {
      const message = JSON.parse(data);
      if (message.action === "socket_info") resolve();
      const waiter = pending.get(message.token);
      if (waiter) {
        pending.delete(message.token);
        clearTimeout(waiter.timer);
        waiter.resolve(message);
      }
    });
  });
  await connected;
  assert.equal((await action("reload", {}, "missing")).error, "not_found");
  assert.equal(
    (await action("anything", [1, null, { nested: true }], "$app")).error,
    "unsupported_action",
  );
  assert.equal(
    (await action("unknown", [1, null])).error,
    "unsupported_action",
  );
  assert.equal(
    (await request({ topic: "node_action", id: "browser", payload: {} })).error,
    "malformed_payload",
  );
  assert.equal((await action("reload")).error, "unavailable");
  if (!cefDisabled) {
    assert.equal(
      (await action("reload", { ignore_cache: "yes" })).error,
      "invalid_payload",
    );
    assert.equal(
      (await action("reload", "x".repeat(65536))).error,
      "malformed_payload",
    );
    await update({ enabled: true });
    await until(
      async () => (await config()).status.browser.cef_state === "ready",
    );
    const before = await config();
    const options = before.nodes.find((node) => node.id === "browser").options;
    for (const payload of [{}, { ignore_cache: true }]) {
      const oldLoads = loads;
      assert.equal((await action("reload", payload)).action, "result");
      await until(
        async () =>
          loads > oldLoads &&
          (await config()).status.browser.cef_state === "ready",
      );
    }
    const after = await config();
    assert.deepEqual(
      after.nodes.find((node) => node.id === "browser").options,
      options,
    );
    assert.equal(
      after.status.browser.cef_restarts,
      before.status.browser.cef_restarts,
    );
    await update({ enabled: false });
    assert.equal((await action("reload")).error, "unavailable");
  } else {
    await update({ enabled: true });
    assert.equal((await action("reload")).error, "unavailable");
  }
  assert.equal(
    (await request({ topic: "remove_node", id: "browser" })).action,
    "result",
  );
  assert.equal((await action("reload")).error, "not_found");
  console.log(
    `Node action routing, errors${cefDisabled ? " and CEF-disabled rejection" : ", reload and cache-bypass reload"} passed. Artifacts: ${work}`,
  );
} finally {
  for (const waiter of pending.values()) {
    clearTimeout(waiter.timer);
    waiter.reject(new Error("Test ending"));
  }
  socket?.close();
  if (app.exitCode === null) app.kill("SIGINT");
  const [code] = await exited;
  await log.close();
  page.close();
  assert.equal(code, 0, `App failed; see ${logPath}`);
  const output = await readFile(logPath, "utf8");
  assert.doesNotMatch(output, /VUID-|Validation Error/);
}

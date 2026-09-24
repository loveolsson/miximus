import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import test from "node:test";
import vm from "node:vm";
import ts from "typescript";

const require = createRequire(import.meta.url);
const compile = (source) =>
  ts.transpileModule(source, {
    compilerOptions: {
      module: ts.ModuleKind.CommonJS,
      target: ts.ScriptTarget.ES2022,
      esModuleInterop: true,
    },
  }).outputText;

// Exercise the real wrapper with deterministic transport/timers. Only Vite's
// environment constant is substituted; protocol enums come from generated code.
function harness() {
  const contracts = { exports: {} };
  vm.runInNewContext(
    compile(readFileSync(new URL("../src/generated/json_contracts.ts", import.meta.url), "utf8")),
    {
      exports: contracts.exports,
      module: contracts,
    },
  );
  const timers = new Map();
  const sockets = [];
  let nextTimer = 0;
  class Socket {
    sent = [];
    constructor() {
      sockets.push(this);
    }
    send(text) {
      this.sent.push(JSON.parse(text));
    }
    close() {}
    receive(message) {
      this.onmessage({ data: JSON.stringify(message) });
    }
  }
  const module = { exports: {} };
  vm.runInNewContext(
    compile(
      readFileSync(new URL("../src/websocket.ts", import.meta.url), "utf8").replaceAll(
        "import.meta.env.PROD",
        "false",
      ),
    ),
    {
      exports: module.exports,
      module,
      require: (name) => (name === "./messages" ? contracts.exports : require(name)),
      WebSocket: Socket,
      location: { hostname: "localhost" },
      console: { log() {}, warn() {}, error() {}, info() {} },
      setTimeout: (fn, ms) => {
        const id = ++nextTimer;
        timers.set(id, { fn, ms });
        return id;
      },
      clearTimeout: (id) => timers.delete(id),
      clearInterval: (id) => timers.delete(id),
    },
  );
  const ws = new module.exports.ws_wrapper();
  const socket = sockets[0];
  socket.receive({ action: "socket_info", id: 1, bundle_hash: "test" });
  return {
    ws,
    socket,
    sockets,
    timers,
    fire(ms) {
      const [id, timer] = [...timers].find(([, timer]) => timer.ms === ms);
      timers.delete(id);
      timer.fn();
    },
  };
}
const command = {
  action: "command",
  topic: "node_action",
  id: "browser",
  name: "reload",
  payload: {},
};

test("correlates action results and server errors with cleanup", async () => {
  const { ws, socket, timers } = harness();
  const result = ws.request(command);
  const sent = socket.sent.at(-1);
  assert.equal(sent.name, "reload");
  socket.receive({ action: "result", token: sent.token, data: [1, { custom: true }] });
  assert.equal(JSON.stringify((await result).data), '[1,{"custom":true}]');
  const failure = ws.request(command);
  socket.receive({
    action: "error",
    token: socket.sent.at(-1).token,
    error: "busy",
    message: "Not ready",
  });
  assert.equal((await failure).error, "busy");
  assert.equal(ws.callbacks.size, 0);
  assert.equal([...timers.values()].filter((timer) => timer.ms === 10000).length, 0);
  assert.equal(ws.listenerCount("on_disconnected"), 0);
});

test("routes out-of-order replies independently and ignores duplicate replies", async () => {
  const { ws, socket } = harness();
  const first = ws.request(command);
  const firstToken = socket.sent.at(-1).token;
  const second = ws.request({ ...command, id: "another-node" });
  const secondToken = socket.sent.at(-1).token;
  socket.receive({ action: "result", token: secondToken, data: "second" });
  assert.equal((await second).data, "second");
  socket.receive({ action: "result", token: secondToken, data: "duplicate" });
  assert.equal(ws.callbacks.size, 1);
  socket.receive({ action: "result", token: firstToken, data: "first" });
  assert.equal((await first).data, "first");
  assert.equal(ws.callbacks.size, 0);
});

test("disconnect settles pending waits and reconnect does not replay actions", async () => {
  const { ws, socket, sockets, fire } = harness();
  const result = ws.request(command);
  const rejected = assert.rejects(result, /Disconnected; action outcome unknown/);
  socket.onclose({ code: 1006, reason: "test" });
  await rejected;
  fire(2000);
  sockets[1].receive({ action: "socket_info", id: 2, bundle_hash: "test" });
  assert.equal(sockets[1].sent.filter((message) => message.topic === "node_action").length, 0);
  assert.equal(ws.callbacks.size, 0);
});

test("timeout and cancellation release waits without retrying", async () => {
  const { ws, socket, fire } = harness();
  const result = ws.request(command);
  const rejected = assert.rejects(result, /Reply timed out; action outcome unknown/);
  fire(10000);
  await rejected;
  const abort = new AbortController();
  const cancelled = assert.rejects(ws.request(command, abort.signal), /Request cancelled/);
  abort.abort();
  await cancelled;
  assert.equal(socket.sent.filter((message) => message.topic === "node_action").length, 2);
  assert.equal(ws.callbacks.size, 0);
  assert.equal(ws.listenerCount("on_disconnected"), 0);
});

test("bounds outstanding waits and cleans serialization failures", async () => {
  const { ws } = harness();
  const abort = new AbortController();
  const pending = Array.from({ length: 64 }, () => ws.request(command, abort.signal));
  const settled = Promise.allSettled(pending);
  await assert.rejects(ws.request(command), /Too many pending requests/);
  abort.abort();
  await settled;
  const circular = {};
  circular.self = circular;
  await assert.rejects(ws.request({ ...command, payload: circular }), /circular/i);
  assert.equal(ws.callbacks.size, 0);
  assert.equal(ws.listenerCount("on_disconnected"), 0);
});

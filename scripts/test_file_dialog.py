#!/usr/bin/env python3
"""Exercise Windows native file browsing against an owned Miximus process."""

import ctypes
from ctypes import wintypes
import json
import os
import socket
import base64
import struct
import subprocess
import threading
import time
import urllib.error
import urllib.request

from cef_test_process import PROCESS_CREATION_FLAGS, stop_process
from test_cef_inputs import ROOT, API, command, config
from test_image_node import write_png


def wait_for(predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError("Timed out waiting for condition")


class WebSocket:
    def __init__(self, host="127.0.0.1", headers=None):
        self.events = []
        self.socket = socket.create_connection((host, 7351), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        fields = {"Host": f"{host}:7351", "Upgrade": "websocket", "Connection": "Upgrade",
                  "Sec-WebSocket-Key": key, "Sec-WebSocket-Version": "13", **(headers or {})}
        request = "GET / HTTP/1.1\r\n" + "".join(f"{k}: {v}\r\n" for k, v in fields.items()) + "\r\n"
        self.socket.sendall(request.encode())
        response = b""
        while not response.endswith(b"\r\n\r\n"):
            response += self.read(1)
        assert response.startswith(b"HTTP/1.1 101"), response
        self.socket.settimeout(None)
        self.thread = threading.Thread(target=self.receive, daemon=True)
        self.thread.start()
        self.info = wait_for(lambda: next((e for e in self.events if e.get("action") == "socket_info"), None))

    def read(self, count):
        data = b""
        while len(data) < count:
            part = self.socket.recv(count - len(data))
            if not part:
                raise EOFError()
            data += part
        return data

    def receive(self):
        try:
            while True:
                opcode, length = self.read(2)
                length &= 127
                if length == 126:
                    length = struct.unpack("!H", self.read(2))[0]
                elif length == 127:
                    length = struct.unpack("!Q", self.read(8))[0]
                body = self.read(length)
                if opcode & 15 == 8:
                    return
                if opcode & 15 == 1:
                    self.events.append(json.loads(body))
        except (OSError, EOFError):
            pass

    def request(self, **payload):
        token = str(time.monotonic_ns())
        data = json.dumps({**payload, "token": token}).encode()
        mask = os.urandom(4)
        length = bytes([0x80 | len(data)]) if len(data) < 126 else b"\xfe" + struct.pack("!H", len(data))
        self.socket.sendall(b"\x81" + length + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))
        return wait_for(lambda: next((e for e in self.events if e.get("token") == token), None), timeout=3)

    def close(self):
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.socket.close()
        self.thread.join(timeout=3)


def main():
    if os.name != "nt":
        raise SystemExit("Windows interactive desktop required")
    try:
        config()
    except OSError:
        pass
    else:
        raise SystemExit("An app already serves the API; refusing to change its graph")

    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.EnumChildWindows.argtypes = [wintypes.HWND, callback_type, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.GetParent.argtypes = [wintypes.HWND]
    user.GetParent.restype = wintypes.HWND
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.IsWindowEnabled.argtypes = [wintypes.HWND]
    user.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.SendMessageW.restype = wintypes.LPARAM
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    def class_name(hwnd):
        text = ctypes.create_unicode_buffer(256)
        user.GetClassNameW(hwnd, text, len(text))
        return text.value

    def dialog(pid):
        found = []

        @callback_type
        def visit(hwnd, _):
            owner = wintypes.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and class_name(hwnd) == "#32770" and user.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True

        user.EnumWindows(visit, 0)
        return found[0] if found else None

    def select(hwnd, path):
        edits = []

        @callback_type
        def visit(child, _):
            if (class_name(child) == "Edit" and class_name(user.GetParent(child)) == "ComboBox"
                    and user.IsWindowVisible(child) and user.IsWindowEnabled(child)):
                edits.append(child)
            return True

        user.EnumChildWindows(hwnd, visit, 0)
        assert len(edits) == 1, f"Expected one filename edit, got {len(edits)}"
        value = ctypes.create_unicode_buffer(str(path))
        user.SendMessageW(edits[0], 0x000C, 0, ctypes.addressof(value))  # WM_SETTEXT
        user.PostMessageW(hwnd, 0x0111, 1, 0)  # WM_COMMAND, IDOK

    work = ROOT / "build/integration-tests" / time.strftime("file-dialog-%Y%m%d-%H%M%S")
    work.mkdir(parents=True)
    print("Artifacts:", work, flush=True)
    image = work / "image-\u00e5.png"
    write_png(image, 16, 16)
    text = work / "text-\u00e5.txt"
    text.write_text("Teleprompter test", encoding="utf-8")
    settings = work / "settings.json"
    settings.write_text(json.dumps(dict(schema_version=1, nodes=[
        dict(id="image", type="image", options={}),
        dict(id="text", type="teleprompter", options={}),
    ], connections=[])))
    with (work / "app.log").open("w") as log:
        app = subprocess.Popen([str(ROOT / "build/miximus.exe"), "--settings", str(settings), "--stop-after", "120"],
                               cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                               creationflags=PROCESS_CREATION_FLAGS)
        observer = None
        try:
            def ready():
                assert app.poll() is None, "Application exited"
                try:
                    return config()
                except OSError:
                    return None

            wait_for(ready)
            for method in ("GET", "POST"):
                req = urllib.request.Request(API + "/file-dialog", method=method)
                try:
                    urllib.request.urlopen(req, timeout=3)
                    raise AssertionError("Removed HTTP endpoint still exists")
                except urllib.error.HTTPError as error:
                    assert error.code == 404

            for headers in ({"Host": "untrusted.invalid:7351"}, {"X-Forwarded-For": "192.0.2.1"},
                            {"Origin": "http://untrusted.invalid"}):
                denied = WebSocket(headers=headers)
                try:
                    assert denied.info["can_browse_files"] is False
                    assert denied.request(action="command", topic="file_dialog", id="image")["error"] == "unavailable"
                    assert not dialog(app.pid)
                finally:
                    denied.close()
            for address in {entry[4][0] for entry in socket.getaddrinfo(socket.gethostname(), 7351, socket.AF_INET)} | {"localhost", "::1"}:
                local = WebSocket(address, headers={"Host": "localhost:7351"} if address == "::1" else None)
                assert local.info["can_browse_files"] is True
                local.close()

            observer = WebSocket(headers={"Origin": "http://127.0.0.1:7351"})
            assert observer.info["can_browse_files"] is True
            events = observer.events
            assert observer.request(action="subscribe", topic="update_node")["action"] == "result"
            assert observer.request(action="command", topic="file_dialog")["error"] == "malformed_payload"
            assert observer.request(action="command", topic="file_dialog", id="missing")["error"] == "not_found"

            def browse(node_id):
                # Reply must arrive before any user selection, not when the dialog closes.
                def admitted():
                    reply = observer.request(action="command", topic="file_dialog", id=node_id)
                    if reply.get("error") == "busy":
                        return None  # Shell teardown can outlast window visibility.
                    assert reply["action"] == "result", reply
                    return reply
                reply = wait_for(admitted)
                return reply["token"], wait_for(lambda: dialog(app.pid))

            def settled(hwnd):
                wait_for(lambda: not user.IsWindowVisible(hwnd))
                time.sleep(0.15)

            for node_id, path in [("image", image), ("text", text)]:
                token, hwnd = browse(node_id)
                assert observer.request(action="command", topic="file_dialog", id=node_id)["error"] == "busy"
                for _ in range(5):
                    assert config()
                select(hwnd, path)
                wait_for(lambda: any(e.get("topic") == "update_node" and e.get("id") == node_id
                                     and e.get("options", {}).get("file_path") == str(path) for e in events))
                assert next(n for n in config()["nodes"] if n["id"] == node_id)["options"]["file_path"] == str(path)
                assert sum(e.get("token") == token for e in events) == 1
                print("Selected and broadcast:", node_id, flush=True)
                settled(hwnd)

            token, hwnd = browse("image")
            user.PostMessageW(hwnd, 0x0010, 0, 0)
            settled(hwnd)
            assert sum(e.get("token") == token for e in events) == 1
            assert next(n for n in config()["nodes"] if n["id"] == "image")["options"]["file_path"] == str(image)

            token, hwnd = browse("image")
            command("remove_node", id="image")
            command("add_node", type="image", options={})
            select(hwnd, image)
            settled(hwnd)
            assert next(n for n in config()["nodes"] if n["type"] == "image")["options"]["file_path"] == ""
            assert sum(e.get("token") == token for e in events) == 1

            token, hwnd = browse("text")
            assert stop_process(app) == 0
            assert not dialog(app.pid)
            (work / "broadcasts.json").write_text(json.dumps(events, indent=2))
        finally:
            if observer:
                observer.close()
            stop_process(app)
    saved = json.loads(settings.read_text(encoding="utf-8"))
    assert next(n for n in saved["nodes"] if n["id"] == "text")["options"]["file_path"] == str(text)
    print("PASS: selection, broadcasts, persistence, cancellation, busy, replacement, access checks, shutdown")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Talk to the Steam client's settings through the webhelper debug port.

Modes:
  get                 -> print JSON {"enabled": bool, "limit": int}
  set <profile>       -> set steamos_platform_performance_profile

This uses the same in-memory settings the QAM uses, so it is authoritative
(Steam only reads config.vdf once at startup).

Exits 0 on success, non-zero otherwise.
"""

import base64
import json
import os
import socket
import struct
import sys
import urllib.request

DEBUG_URL = os.environ.get("STEAM_DEBUG_URL", "http://127.0.0.1:8080/json")
TARGET_TITLE = "SharedJSContext"

# Shared prelude: grab the webpack require and locate the settings store.
PRELUDE = r"""
  let req = null;
  webpackChunksteamui.push([[Math.random()], {}, r => { req = r; }]);
  if (!req) return { error: "no-require" };
"""

FIND_STORE = r"""
  let store = null;
  for (const id of Object.keys(req.m || {})) {
    let mod;
    try { mod = req(id); } catch (e) { continue; }
    if (!mod || typeof mod !== "object") continue;
    for (const key in mod) {
      const value = mod[key];
      if (value && typeof value === "object"
          && value.clientSettings && typeof value.clientSettings === "object") {
        store = value;
        break;
      }
    }
    if (store) break;
  }
"""

READ_JS = r"""
(() => {
""" + PRELUDE + FIND_STORE + r"""
  if (!store) return { error: "no-store" };
  const cs = store.clientSettings;
  return {
    enabled: !!cs["steamos_tdp_limit_enabled"],
    limit: cs["steamos_tdp_limit"],
  };
})()
"""

SET_JS = r"""
(() => {
""" + PRELUDE + FIND_STORE + r"""
  let msgClass = null;
  for (const id of Object.keys(req.m || {})) {
    let mod;
    try { mod = req(id); } catch (e) { continue; }
    if (!mod || typeof mod !== "object") continue;
    for (const key in mod) {
      const value = mod[key];
      if (typeof value === "function" && value.M && typeof value.M === "function") {
        try {
          const fields = value.M().fields;
          if (fields && fields.steamos_platform_performance_profile) {
            msgClass = value;
            break;
          }
        } catch (e) {}
      }
    }
    if (msgClass) break;
  }
  if (!msgClass) return { error: "no-class" };
  const b64 = msgClass
    .fromObject({ steamos_platform_performance_profile: __PROFILE__ })
    .serializeBase64String();
  SteamClient.Settings.SetSetting(b64);
  return { ok: true };
})()
"""


def find_target():
    with urllib.request.urlopen(DEBUG_URL, timeout=2) as response:
        targets = json.load(response)
    for target in targets:
        if target.get("title") == TARGET_TITLE:
            return target["webSocketDebuggerUrl"]
    return None


def connect(url):
    rest = url[len("ws://"):]
    hostport, path = rest.split("/", 1)
    host, port = hostport.split(":")
    sock = socket.create_connection((host, int(port)), timeout=3)
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        f"GET /{path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(request.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            return None
        buf += chunk
    return sock


def send_text(sock, text):
    data = text.encode()
    header = bytearray([0x81])
    mask = os.urandom(4)
    length = len(data)
    if length < 126:
        header.append(0x80 | length)
    elif length < 65536:
        header.append(0x80 | 126)
        header += struct.pack(">H", length)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", length)
    header += mask
    sock.sendall(bytes(header) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def recv_frame(sock):
    def read(count):
        out = b""
        while len(out) < count:
            chunk = sock.recv(count - len(out))
            if not chunk:
                raise EOFError
            out += chunk
        return out

    header = read(2)
    length = header[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", read(2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read(8))[0]
    if header[1] & 0x80:
        read(4)
    return read(length).decode(errors="replace")


def evaluate(expression):
    url = find_target()
    if not url:
        return None
    sock = connect(url)
    if not sock:
        return None
    send_text(sock, json.dumps({
        "id": 1,
        "method": "Runtime.evaluate",
        "params": {"expression": expression, "returnByValue": True,
                   "awaitPromise": True},
    }))
    while True:
        message = json.loads(recv_frame(sock))
        if message.get("id") == 1:
            return (message.get("result", {})
                           .get("result", {})
                           .get("value"))


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: steam-set-profile.py get|set <profile>\n")
        return 2

    mode = sys.argv[1]
    if mode == "get":
        expression = READ_JS
    elif mode == "set" and len(sys.argv) >= 3:
        expression = SET_JS.replace("__PROFILE__", json.dumps(sys.argv[2]))
    else:
        sys.stderr.write("usage: steam-set-profile.py get|set <profile>\n")
        return 2

    try:
        value = evaluate(expression)
    except Exception as exc:  # noqa: BLE001 - best effort helper
        sys.stderr.write(f"steam-set-profile: {exc}\n")
        return 1

    if not isinstance(value, dict):
        return 1
    if "error" in value:
        sys.stderr.write(f"steam-set-profile: {value['error']}\n")
        return 1

    if mode == "get":
        print(json.dumps(value))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Load test for the station's web API, see docs/web-api.md. Standard library only.

    python tools/stress_web.py 192.168.178.67 --seconds 120

Runs websocket clients that poll status and fire bursts of commands, one client
that connects and then never reads, and a loop of snapshot downloads. Fails if a
snapshot is incomplete, a command goes unanswered or the station restarts.
Lost connections are counted and retried.
Exits 0 when all is well.
"""

import argparse
import base64
import http.client
import json
import os
import socket
import struct
import sys
import threading
import time


class Ws:
    def __init__(self, host, rcvbuf=None):
        self.sock = socket.socket()
        self.sock.settimeout(10)
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.connect((host, 80))
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        reply = b""
        while b"\r\n\r\n" not in reply:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise ConnectionError("closed during handshake")
            reply += chunk
        if b" 101 " not in reply.split(b"\r\n")[0]:
            raise ConnectionError(reply[:80])
        self.rest = reply.split(b"\r\n\r\n", 1)[1]

    def send(self, obj):
        data = json.dumps(obj).encode()
        mask = os.urandom(4)
        head = bytes([0x81])
        if len(data) < 126:
            head += bytes([0x80 | len(data)])
        else:
            head += bytes([0x80 | 126]) + struct.pack(">H", len(data))
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _need(self, n):
        # Nothing is consumed here: a timeout must not lose a frame half way.
        while len(self.rest) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed")
            self.rest += chunk

    def recv(self):
        """Next text frame as an object."""
        while True:
            self._need(2)
            b0, b1 = self.rest[0], self.rest[1]
            n = b1 & 0x7F
            start = 2
            if n == 126:
                self._need(4)
                n = struct.unpack(">H", self.rest[2:4])[0]
                start = 4
            elif n == 127:
                self._need(10)
                n = struct.unpack(">Q", self.rest[2:10])[0]
                start = 10
            self._need(start + n)
            payload = self.rest[start:start + n]
            self.rest = self.rest[start + n:]
            if b0 & 0x0F == 1:
                return json.loads(payload)
            if b0 & 0x0F == 8:
                raise ConnectionError("close frame")


class Result:
    def __init__(self):
        self.lock = threading.Lock()
        self.errors = []
        self.counts = {}
        self.status = {}
        self.first_uptime = None
        self.heap_min_seen = None
        self.block_min_seen = None

    def error(self, text):
        with self.lock:
            self.errors.append(text)
        print("ERROR:", text, flush=True)

    def count(self, key, n=1):
        with self.lock:
            self.counts[key] = self.counts.get(key, 0) + n


def ws_client(host, name, burst, until, result):
    """Reconnects when the connection goes: the station closes the least recently
    used socket when more than seven are open, and WiFi can drop. Only what goes
    missing on a connection that stays up is an error."""
    while time.time() < until:
        try:
            ws_session(host, burst, until, result)
        except OSError as err:
            result.count("reconnects")
            print(f"{name}: {err}", flush=True)
            time.sleep(1)


def monitor(until, result):
    """One line every 30 s, so a problem can be placed in time afterwards."""
    while time.time() < until:
        time.sleep(30)
        with result.lock:
            status = dict(result.status)
            counts = dict(result.counts)
        print(f"{time.strftime('%H:%M:%S')} up {status.get('uptime')} s: heap {status.get('heap')}, "
              f"lowest {status.get('heapMin')}, block {status.get('heapBlock')}, ws {status.get('ws')}, "
              f"status frames {counts.get('status')}, snapshots {counts.get('snapshot')}", flush=True)


def ws_session(host, burst, until, result):
    ws = Ws(host)
    ws.sock.settimeout(1.0)
    sent = answered = 0
    resynced = False
    next_status = next_burst = 0.0
    session_end = min(until, time.time() + 60)
    while time.time() < session_end:
        now = time.time()
        try:
            if now >= next_status:
                ws.send({"cmd": "status"})
                next_status = now + 2
            if burst and now >= next_burst:
                # More at once than the mailbox of the HTTP server has slots.
                for _ in range(10):
                    ws.send({"cmd": "info"})
                    sent += 1
                next_burst = now + 5
            msg = ws.recv()
        except (socket.timeout, TimeoutError):
            continue
        kind = msg.get("type") or next(iter(msg))
        result.count(kind)
        resynced = resynced or kind == "resync"
        if kind == "info":
            answered += 1
        if kind == "status":
            track_status(msg, result)
    # Let the last answers arrive.
    deadline = time.time() + 4
    while answered < sent and time.time() < deadline:
        try:
            msg = ws.recv()
        except (socket.timeout, TimeoutError):
            continue
        if msg.get("type") == "info":
            answered += 1
    ws.sock.close()
    if answered < sent and not resynced:
        result.error(f"{sent - answered} of {sent} commands unanswered, no resync either")


def track_status(msg, result):
    with result.lock:
        result.status = msg
        if result.first_uptime is None:
            result.first_uptime = msg["uptime"]
        # The clients run side by side, so one may report a second behind another.
        restarted = msg["uptime"] < result.first_uptime - 10
        if restarted:
            result.errors.append(f"station restarted: uptime {result.first_uptime} -> {msg['uptime']}")
        result.first_uptime = msg["uptime"] if restarted else max(result.first_uptime, msg["uptime"])
        heap, block = msg["heap"], msg.get("heapBlock", msg["heap"])
        result.heap_min_seen = min(heap, result.heap_min_seen or heap)
        result.block_min_seen = min(block, result.block_min_seen or block)


def stalled_client(host, until, result):
    """Connects, asks for everything, never reads."""
    try:
        ws = Ws(host, rcvbuf=1024)
        for _ in range(100):
            ws.send({"cmd": "config"})
        time.sleep(max(0, until - time.time()))
        ws.sock.close()
    except OSError as err:
        result.count("stalled client closed by the station")
        print(f"stalled client: {err}", flush=True)


def guarded(target, result, *args):
    """A test thread that dies must fail the run, not end it quietly."""
    def run():
        try:
            target(*args, result)
        except Exception as err:
            result.error(f"{target.__name__}: {err!r}")
    return threading.Thread(target=run)


def snapshots(host, until, result):
    sizes = []
    times = []
    while time.time() < until:
        start = time.time()
        try:
            conn = http.client.HTTPConnection(host, 80, timeout=30)
            conn.request("GET", "/api/snapshot")
            resp = conn.getresponse()
            body = resp.read()
            conn.close()
        except (OSError, http.client.HTTPException) as err:
            result.count("snapshot transport errors")
            print(f"snapshot: {err}", flush=True)
            time.sleep(2)
            continue
        if resp.status == 503:
            result.count("snapshot 503")
            time.sleep(5)
            continue
        if resp.status != 200:
            result.error(f"snapshot: HTTP {resp.status}")
            continue
        try:
            doc = json.loads(body)
        except ValueError as err:
            result.error(f"snapshot: not JSON ({err}), {len(body)} bytes")
            continue
        if not doc.get("complete"):
            result.error("snapshot: not complete")
        sizes.append(len(body))
        times.append(time.time() - start)
        result.count("snapshot")
        result.snapshot = doc
        time.sleep(1)
    if sizes:
        print(f"snapshots: {len(sizes)}, largest {max(sizes)} B, slowest {max(times):.2f} s", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host")
    parser.add_argument("--seconds", type=int, default=120)
    args = parser.parse_args()

    result = Result()
    until = time.time() + args.seconds
    threads = [guarded(ws_client, result, args.host, f"client{i}", i == 0, until) for i in range(3)]
    threads.append(guarded(stalled_client, result, args.host, until))
    threads.append(guarded(snapshots, result, args.host, until))
    threads.append(threading.Thread(target=monitor, args=(until, result), daemon=True))
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    time.sleep(2)
    ws = Ws(args.host)
    ws.sock.settimeout(5)
    ws.send({"cmd": "info"})
    ws.send({"cmd": "status"})
    final = {}
    while len(final) < 2:
        msg = ws.recv()
        if msg.get("type") in ("info", "status"):
            final[msg["type"]] = msg
    track_status(final["status"], result)
    ws.sock.close()

    status = final["status"]
    print(f"frames: {result.counts}")
    print(f"heap now {status['heap']}, lowest since boot {status['heapMin']}, largest block now "
          f"{status.get('heapBlock')}, lowest seen {result.heap_min_seen} / block {result.block_min_seen}")
    print(f"ws: {status.get('ws')}, uptime {status['uptime']} s, last reset: {final['info'].get('resetReason')}")
    if not result.counts.get("snapshot"):
        result.error("no snapshot arrived")
    if result.errors:
        print(f"FAILED, {len(result.errors)} errors:")
        for error in result.errors:
            print(" ", error)
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

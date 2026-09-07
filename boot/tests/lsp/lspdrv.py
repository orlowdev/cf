#!/usr/bin/env python3
"""Minimal LSP driver for cf lsp: spawn the server, run a scripted exchange, check answers."""
import json, os, subprocess, sys, threading, queue

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
CF = os.environ.get("CF", os.path.join(ROOT, "var", "cf"))
CWD = ROOT

class Server:
    def __init__(self, pwd=None):
        pwd = pwd or CWD
        env = dict(os.environ, PWD=pwd)  # the server absolutizes relative module paths against $PWD
        self.p = subprocess.Popen([CF, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, cwd=pwd, env=env)
        self.q = queue.Queue()
        self.nid = 0
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = self.p.stdout
        while True:
            line = buf.readline()
            if not line: break
            if line.lower().startswith(b"content-length:"):
                n = int(line.split(b":")[1].strip())
                while True:
                    l2 = buf.readline()
                    if l2 in (b"\r\n", b"\n", b""): break
                body = buf.read(n)
                self.q.put(json.loads(body))

    def notify(self, method, params):
        body = json.dumps({"jsonrpc": "2.0", "method": method, "params": params}).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def request(self, method, params, timeout=30):
        self.nid += 1
        rid = self.nid
        body = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()
        pending = []
        while True:
            msg = self.q.get(timeout=timeout)
            if msg.get("id") == rid:
                for m in pending: self.q.put(m)
                return msg
            pending.append(msg)  # notifications (diagnostics) — stash

    def drain(self):
        out = []
        try:
            while True: out.append(self.q.get_nowait())
        except queue.Empty:
            return out

    def stop(self):
        try:
            self.request("shutdown", {}, timeout=5)
            self.notify("exit", {})
        except Exception: pass
        self.p.wait(timeout=5)

def uri_of(path): return "file://" + path

def tdp(path, line, char, extra=None):
    d = {"textDocument": {"uri": uri_of(path)}, "position": {"line": line, "character": char}}
    if extra: d.update(extra)
    return d

passed = 0
failed = []

def check(name, cond, detail=""):
    global passed
    if cond:
        passed += 1
        print(f"  ok  {name}")
    else:
        failed.append(name)
        print(f"FAIL  {name}  {detail}")

def main():
    import tests
    s = Server()
    s.request("initialize", {"capabilities": {}})
    try:
        tests.run(s, check, tdp, uri_of)
    finally:
        s.stop()
    print(f"\n{passed} passed, {len(failed)} failed")
    if failed:
        for f in failed: print("  FAILED:", f)
        sys.exit(1)

if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()

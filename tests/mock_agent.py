#!/usr/bin/env python3
"""
Mock retroctl_agent, in pure Python, so the CLI can be tested end-to-end on
the host without the box.  It mimics the C agent's JSON-lines responses
(handshake, exec streaming, ps, put/get staging, kill, ls, info) closely
enough to catch protocol / field-name / framing bugs in cli/retroctl.py.

Run two processes:
    python3 tests/mock_agent.py            # listens on 46911 (or PORT=..)
    python3 cli/retroctl.py -H 127.0.0.1 -T mocktok ps
"""
import socket, json, base64, os, threading, sys

PORT = int(os.environ.get("PORT", "46911"))
TOKEN = os.environ.get("TOKEN", "mocktok")
FILES = {}   # remote-path -> bytes (staging store for put/get)


def send_line(sock, obj):
    sock.sendall((json.dumps(obj) + "\n").encode())


def handle(conn, addr):
    buf = b""
    # handshake
    while not buf.endswith(b"\n"):
        d = conn.recv(1)
        if not d:
            return
        buf += d
    req = json.loads(buf.decode().rstrip("\n"))
    if req.get("op") != "hello" or req.get("token") != TOKEN:
        send_line(conn, {"id": "0", "ok": False, "err": "bad token"})
        conn.close()
        return
    send_line(conn, {"id": "0", "ok": True, "agent": "1.0.0-mock"})

    def read_req():
        nonlocal buf
        buf = b""
        while not buf.endswith(b"\n"):
            d = conn.recv(1)
            if not d:
                return None
            buf += d
        return json.loads(buf.decode().rstrip("\n"))

    while True:
        r = read_req()
        if r is None:
            break
        op, rid = r.get("op"), r.get("id", "0")
        if op == "exec":
            data = ("MOCK-OUT cmd=%s\n" % r.get("cmd", "")).encode()
            for i in range(0, len(data) or 1, 32768):
                ch = data[i:i + 32768]
                send_line(conn, {"id": rid, "type": "out",
                                 "b64": base64.b64encode(ch).decode()})
            send_line(conn, {"id": rid, "type": "done", "code": 0})
        elif op == "ps":
            procs = [{"pid": 4, "ppid": 0, "name": "System Idle Process"},
                     {"pid": 764, "ppid": 4, "name": "services.exe"},
                     {"pid": 812, "ppid": 4, "name": "aiweb3.exe"}]
            send_line(conn, {"id": rid, "ok": True, "procs": procs,
                             "count": len(procs)})
        elif op == "kill":
            if "pid" in r:
                send_line(conn, {"id": rid, "ok": True, "killed": True})
            else:
                send_line(conn, {"id": rid, "ok": True, "matched": 1,
                                 "killed": 1})
        elif op == "ls":
            send_line(conn, {"id": rid, "ok": True, "path": r.get("path"),
                             "entries": [
                                 {"name": "gozweb", "dir": True},
                                 {"name": "aiweb.c", "dir": False, "size": 9000},
                                 {"name": "README", "dir": False, "size": 120}]})
        elif op == "info":
            send_line(conn, {"id": rid, "ok": True, "agent_ver": "1.0.0-mock",
                             "hostname": "MOCKBOX", "os_family": "Windows 2000/NT4"})
        elif op == "put":
            FILES[r.get("path")] = b""
            send_line(conn, {"id": rid, "ok": True,
                             "staging": r.get("path") + "~rctl.tmp"})
        elif op == "putchunk":
            raw = base64.b64decode(r.get("data", ""))
            p = r.get("path")
            FILES[p] = FILES.get(p, b"") + raw
            send_line(conn, {"id": rid, "ok": True, "written": len(raw)})
        elif op == "putend":
            send_line(conn, {"id": rid, "ok": True, "moved": True})
        elif op == "get":
            blob = FILES.get(r.get("path"), b"hello-from-mock")
            for i in range(0, len(blob), 65536):
                ch = blob[i:i + 65536]
                send_line(conn, {"id": rid, "type": "getchunk",
                                  "b64": base64.b64encode(ch).decode()})
            send_line(conn, {"id": rid, "type": "getdone", "ok": True})
        else:
            send_line(conn, {"id": rid, "ok": False, "err": "unknown op"})
    conn.close()


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", PORT))
    s.listen(2)
    print("mock agent on 127.0.0.1:%d token=%s" % (PORT, TOKEN))
    while True:
        c, a = s.accept()
        threading.Thread(target=handle, args=(c, a), daemon=True).start()


if __name__ == "__main__":
    main()

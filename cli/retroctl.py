#!/usr/bin/env python3
"""
retroctl - a tiny command-line controller for legacy (and modern) Windows boxes.

Pairs with retroctl_agent.c, a native TCP service on the target box.  The agent
speaks JSON-lines over one connection: send a "hello" with a shared token, then
stream commands.  No web UI, no per-call service spawning, no output-file locks.

This client is dependency-free (Python stdlib only) so it runs anywhere and is
trivial to review.  One file, no install.

Usage:
    retroctl [global opts] <command> [args]

Global options (or in ~/.retroctl.json, or RETROCTL_* env):
    -H/--host  -P/--port  -T/--token  --timeout SECS

Commands:
    exec "cmd" [--cwd DIR] [--timeout MS] [--live]
    ps [--json]
    kill PID | --name NAME [--all]
    put LOCAL REMOTE
    get REMOTE [LOCAL]
    ls [PATH]
    info
    svc install|stop|start|status|del
    shell            interactive REPL on a persistent connection

Examples:
    retroctl exec "ver & systeminfo"
    retroctl ps | head
    retroctl put ./report.html C:\\Inetpub\\gozweb\\report.html
    retroctl shell
"""

import socket
import json
import base64
import argparse
import os
import sys
import time

AGENT_DEFAULT_PORT = 46911
CONFIG_PATH = os.path.expanduser("~/.retroctl.json")
PUT_CHUNK = 64 * 1024          # raw bytes per putchunk frame
MAX_LINE = 2_000_000          # largest JSON line we accept back


class AgentError(Exception):
    pass


# ===========================================================================
# Low-level JSON-lines client
# ===========================================================================
class RetroCtl:
    def __init__(self, host, port, token, timeout=30.0):
        self.host = host
        self.port = int(port)
        self.token = token
        self.timeout = timeout
        self.sock = None
        self.agent_ver = None
        self._id = 0

    def connect(self):
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        s.settimeout(self.timeout)
        self.sock = s
        self._send({"op": "hello", "token": self.token})
        resp = self._read_line()
        if resp is None:
            raise AgentError("agent closed connection during handshake")
        if not resp.get("ok"):
            raise AgentError(resp.get("err", "handshake failed"))
        self.agent_ver = resp.get("agent")
        return self

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *a):
        self.close()

    def _next_id(self):
        self._id += 1
        return str(self._id)

    def _send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode("utf-8"))

    def _read_line(self):
        buf = b""
        while not buf.endswith(b"\n"):
            ch = self.sock.recv(1)
            if not ch:
                return None
            buf += ch
            if len(buf) > MAX_LINE:
                raise AgentError("response line too large")
        return json.loads(buf.decode("utf-8", "replace").rstrip("\n") or "null")

    def _read_until(self, idv, terminal_types):
        """Collect frames with this id until a terminal frame (type in
        terminal_types, or a typeless single-shot reply).  Returns
        (list_of_non_terminal, terminal_frame)."""
        frames = []
        while True:
            resp = self._read_line()
            if resp is None:
                raise AgentError("connection closed mid-response")
            if resp.get("id") != idv:
                continue
            ftype = resp.get("type")
            if ftype in terminal_types or ftype is None:
                return frames, resp
            frames.append(resp)

    # -- ops ------------------------------------------------------------------
    def exec(self, cmd, cwd=None, timeout_ms=60000, on_chunk=None):
        idv = self._next_id()
        req = {"op": "exec", "id": idv, "cmd": cmd, "timeout": int(timeout_ms)}
        if cwd:
            req["cwd"] = cwd
        self._send(req)
        out = bytearray()
        frames, term = self._read_until(idv, {"done"})
        for f in frames:
            if f.get("type") == "out":
                raw = base64.b64decode(f.get("b64", ""))
                out.extend(raw)
                if on_chunk:
                    on_chunk(raw)
        code = term.get("code", 0)
        if term.get("timed_out"):
            code = -1
        return int(code), bytes(out)

    def ps(self):
        idv = self._next_id()
        self._send({"op": "ps", "id": idv})
        _, term = self._read_until(idv, set())
        if not term.get("ok"):
            raise AgentError(term.get("err", "ps failed"))
        return term.get("procs", [])

    def kill(self, pid=None, name=None, all=False):
        idv = self._next_id()
        req = {"op": "kill", "id": idv}
        if pid is not None:
            req["pid"] = int(pid)
        if name is not None:
            req["name"] = name
            req["all"] = bool(all)
        self._send(req)
        _, term = self._read_until(idv, set())
        if not term.get("ok"):
            raise AgentError(term.get("err", "kill failed"))
        return term

    def ls(self, path=r"C:\\"):
        idv = self._next_id()
        self._send({"op": "ls", "id": idv, "path": path})
        _, term = self._read_until(idv, set())
        if not term.get("ok"):
            raise AgentError(term.get("err", "ls failed"))
        return term.get("entries", [])

    def info(self):
        idv = self._next_id()
        self._send({"op": "info", "id": idv})
        _, term = self._read_until(idv, set())
        if not term.get("ok"):
            raise AgentError(term.get("err", "info failed"))
        return term

    def put(self, local_path, remote_path, on_progress=None):
        with open(local_path, "rb") as fh:
            data = fh.read()
        idv = self._next_id()
        self._send({"op": "put", "id": idv, "path": remote_path})
        _, begin = self._read_until(idv, set())
        if not begin.get("ok"):
            raise AgentError(begin.get("err", "put begin failed"))
        off = 0
        total = len(data)
        while off < total:
            chunk = data[off:off + PUT_CHUNK]
            cid = self._next_id()
            self._send({"op": "putchunk", "id": cid, "path": remote_path,
                        "data": base64.b64encode(chunk).decode("ascii")})
            _, cterm = self._read_until(cid, set())
            if not cterm.get("ok"):
                raise AgentError(cterm.get("err", "putchunk failed"))
            off += len(chunk)
            if on_progress:
                on_progress(off, total)
        eid = self._next_id()
        self._send({"op": "putend", "id": eid, "path": remote_path})
        _, eterm = self._read_until(eid, set())
        if not eterm.get("ok"):
            raise AgentError(eterm.get("err", "putend failed"))
        return total

    def get(self, remote_path, local_path=None, on_progress=None):
        idv = self._next_id()
        self._send({"op": "get", "id": idv, "path": remote_path})
        buf = bytearray()
        frames, term = self._read_until(idv, {"getdone"})
        for f in frames:
            if f.get("type") == "getchunk":
                buf.extend(base64.b64decode(f.get("b64", "")))
                if on_progress:
                    on_progress(len(buf))
        if term.get("ok") is False:
            raise AgentError(term.get("err", "get failed"))
        data = bytes(buf)
        if local_path:
            with open(local_path, "wb") as fh:
                fh.write(data)
        return data


# ===========================================================================
# config
# ===========================================================================
def load_config(args):
    host, port, token = "127.0.0.1", AGENT_DEFAULT_PORT, "changeme"
    if os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH) as fh:
                c = json.load(fh)
            host = c.get("host", host)
            port = c.get("port", port)
            token = c.get("token", token)
        except (json.JSONDecodeError, OSError):
            pass
    host = os.environ.get("RETROCTL_HOST", host)
    port = os.environ.get("RETROCTL_PORT", port)
    token = os.environ.get("RETROCTL_TOKEN", token)
    if args.host:
        host = args.host
    if args.port:
        port = args.port
    if args.token:
        token = args.token
    return host, int(port), token


def open_agent(args, timeout=None):
    host, port, token = load_config(args)
    ag = RetroCtl(host, port, token, timeout=timeout or args.timeout)
    return ag, (host, port)


# ===========================================================================
# command implementations
# ===========================================================================
def print_bytes(buf, stream=sys.stdout):
    stream.write(buf.decode("utf-8", "replace"))
    stream.flush()


def cmd_exec(args):
    ag, (host, port) = open_agent(args)
    with ag:
        live = args.live or (sys.stdin.isatty() and args.live)
        if args.live:
            def on_chunk(raw):
                sys.stdout.buffer.write(raw)
                sys.stdout.flush()
        else:
            on_chunk = None
        code, out = ag.exec(args.cmd, cwd=args.cwd, timeout_ms=args.timeout_ms,
                            on_chunk=on_chunk)
        if not args.live:
            print_bytes(out)
        print("retroctl: exit=%d" % code, file=sys.stderr)
        if code == 0:
            return 0
        if code == -1:
            print("retroctl: command timed out", file=sys.stderr)
            return 124
        return code


def cmd_ps(args):
    ag, _ = open_agent(args)
    with ag:
        procs = ag.ps()
    if args.json:
        print(json.dumps(procs, indent=2))
        return 0
    # human table: PID  PPID  NAME
    def w(s, n):
        return (str(s) + " " * n)[:n]
    print("   %s  %s" % (w("", 8), "NAME"))
    for p in procs:
        name = p.get("name", "")
        mark = " *" if "System" in name or p.get("pid") == 4 else ""
        print("%6s %6s  %s%s" % (w(p.get("pid", 0), 6),
                                  w(p.get("ppid", 0), 6), name, mark))
    print("(%d processes)" % len(procs), file=sys.stderr)
    return 0


def cmd_kill(args):
    ag, _ = open_agent(args)
    with ag:
        if args.pid:
            res = ag.kill(pid=args.pid)
            print("killed pid %d: %s" % (args.pid, res.get("killed")))
        elif args.name:
            res = ag.kill(name=args.name, all=args.all)
            print("matched=%s killed=%s" % (res.get("matched"), res.get("killed")))
        else:
            print("give a pid or --name", file=sys.stderr)
            return 2
    return 0


def cmd_put(args):
    ag, _ = open_agent(args)
    with ag:
        last = [0]
        def prog(done, total):
            last[0] = done
            sys.stderr.write("\r  %d/%d bytes (%.0f%%)"
                             % (done, total, 100.0 * done / max(total, 1)))
            sys.stderr.flush()
        total = ag.put(args.local, args.remote, on_progress=prog)
        sys.stderr.write("\n")
    print("uploaded %d bytes -> %s" % (total, args.remote))
    return 0


def cmd_get(args):
    ag, _ = open_agent(args)
    with ag:
        local = args.local if args.local else None
        data = ag.get(args.remote, local)
        if local is None:
            print_bytes(data)
        else:
            print("downloaded %d bytes -> %s" % (len(data), local))
    return 0


def cmd_ls(args):
    ag, _ = open_agent(args)
    with ag:
        entries = ag.ls(args.path)
    for e in entries:
        if e.get("dir"):
            print("%s/" % e.get("name"))
        else:
            print("%9d  %s" % (e.get("size", 0), e.get("name")))
    print("(%d entries in %s)" % (len(entries), args.path), file=sys.stderr)
    return 0


def cmd_info(args):
    ag, (host, port) = open_agent(args)
    with ag:
        inf = ag.info()
    print("agent      v%s" % inf.get("agent_ver"))
    print("hostname   %s" % inf.get("hostname"))
    print("os_family  %s" % inf.get("os_family"))
    print("endpoint   %s:%s" % (host, port))
    return 0


# service management via the agent's own subcommands (run as exec)
def _svc_cmd(ag, sub):
    # The agent exe supports: install / uninstall / status.  We drive them
    # through exec so it works whether the box can start the service itself
    # or we need to stop/start it from the SCM.
    if sub == "install":
        code, out = ag.exec(r"C:\retroctl_agent.exe install", timeout_ms=30000)
    elif sub == "del":
        code, out = ag.exec(r"C:\retroctl_agent.exe uninstall", timeout_ms=30000)
    elif sub == "status":
        code, out = ag.exec(r"C:\retroctl_agent.exe status", timeout_ms=10000)
    elif sub == "stop":
        code, out = ag.exec("net stop retroctl_agent /y", timeout_ms=20000)
    elif sub == "start":
        code, out = ag.exec("net start retroctl_agent", timeout_ms=20000)
    else:
        return 2, b""
    if out:
        print_bytes(out)
    return code, out


def cmd_svc(args):
    ag, _ = open_agent(args)
    with ag:
        code, _ = _svc_cmd(ag, args.svc)
    return 0 if code in (0,) else code


def cmd_shell(args):
    ag, (host, port) = open_agent(args)
    ag.connect()
    print("retroctl shell @ %s:%s  (agent v%s)" % (host, port, ag.agent_ver))
    print('commands: !exec "..." | ps | kill <pid> | put L R | get R [L] | ls [P] | quit')
    cwd = args.cwd
    while True:
        try:
            prompt = "(%s) " % (cwd or "")
            line = input(prompt)
        except (EOFError, KeyboardInterrupt):
            break
        line = line.strip()
        if not line:
            continue
        if line in ("quit", "exit", "q"):
            break
        try:
            if line in ("ps", "ps --json"):
                procs = ag.ps()
                for p in procs:
                    print("%6s %6s  %s" % (p.get("pid"), p.get("ppid"), p.get("name")))
            elif line.startswith("kill "):
                rest = line[5:].strip()
                if rest.isdigit():
                    print(ag.kill(pid=int(rest)))
                else:
                    print(ag.kill(name=rest, all=True))
            elif line.startswith("put "):
                a, b = line.split(None, 2)[1:3]
                print(ag.put(a, b))
            elif line.startswith("get "):
                parts = line.split()
                print(ag.get(parts[1], parts[2] if len(parts) > 2 else None))
            elif line.startswith("ls"):
                parts = line.split()
                p = parts[1] if len(parts) > 1 else r"C:\\"
                for e in ag.ls(p):
                    print("%s%s" % (e.get("name"), "/" if e.get("dir") else ""))
            elif line.startswith("cd "):
                cwd = line[3:].strip()
            elif line.startswith("!") or True:
                code, out = ag.exec(line.lstrip("!").strip() or "cmd",
                                    cwd=cwd or None,
                                    timeout_ms=args.timeout_ms)
                print_bytes(out)
                print("[exit %d]" % code)
        except AgentError as e:
            print("error: %s" % e, file=sys.stderr)
            # on connection drop, break
            if ag.sock is None:
                break
    ag.close()
    return 0


# ===========================================================================
# CLI
# ===========================================================================
def build_parser():
    p = argparse.ArgumentParser(prog="retroctl", add_help=True,
                                description="control a Windows box over retroctl_agent")
    p.add_argument("-H", "--host", help="agent host (default 127.0.0.1 or ~/.retroctl.json)")
    p.add_argument("-P", "--port", help="agent port (default 46911)")
    p.add_argument("-T", "--token", help="shared token (default from config/changeme)")
    p.add_argument("--timeout", type=float, default=30.0,
                   help="socket timeout in seconds (default 30)")
    sub = p.add_subparsers(dest="cmd")

    e = sub.add_parser("exec", help="run a command line, stream output back")
    e.add_argument("cmd", help="command line to run (as cmd /c would see it)")
    e.add_argument("--cwd", help="working directory on the box")
    e.add_argument("--timeout-ms", type=int, default=60000, help="per-command ms")
    e.add_argument("--live", action="store_true", help="stream chunks to stdout as they arrive")
    e.set_defaults(func=cmd_exec)

    ps = sub.add_parser("ps", help="list processes")
    ps.add_argument("--json", action="store_true", help="emit raw JSON")
    ps.set_defaults(func=cmd_ps)

    k = sub.add_parser("kill", help="terminate a process by pid or name")
    k.add_argument("pid", nargs="?", type=int, help="pid to kill")
    k.add_argument("--name", help="process name to kill (all matching)")
    k.add_argument("--all", action="store_true", help="with --name, kill every match")
    k.set_defaults(func=cmd_kill)

    pu = sub.add_parser("put", help="upload a local file to the box")
    pu.add_argument("local", help="local path")
    pu.add_argument("remote", help="remote path")
    pu.set_defaults(func=cmd_put)

    g = sub.add_parser("get", help="download a file from the box")
    g.add_argument("remote", help="remote path")
    g.add_argument("local", nargs="?", help="local path (default: stdout)")
    g.set_defaults(func=cmd_get)

    l = sub.add_parser("ls", help="list a directory on the box")
    l.add_argument("path", nargs="?", default=r"C:\\", help="directory")
    l.set_defaults(func=cmd_ls)

    i = sub.add_parser("info", help="agent / box info")
    i.set_defaults(func=cmd_info)

    s = sub.add_parser("svc", help="manage the agent's service")
    s.add_argument("svc", choices=["install", "uninstall", "del", "stop",
                                   "start", "status"])
    s.set_defaults(func=cmd_svc)

    sh = sub.add_parser("shell", help="interactive REPL on a persistent connection")
    sh.add_argument("--cwd", help="initial working directory on the box")
    sh.add_argument("--timeout-ms", type=int, default=60000)
    sh.set_defaults(func=cmd_shell)
    return p


def main(argv=None):
    argv = argv if argv is not None else sys.argv[1:]
    p = build_parser()
    args = p.parse_args(argv)
    if not getattr(args, "cmd", None):
        p.print_help()
        return 0
    try:
        return args.func(args) or 0
    except AgentError as e:
        print("retroctl: %s" % e, file=sys.stderr)
        return 1
    except (ConnectionRefusedError, socket.timeout) as e:
        host, port, _ = load_config(args)
        print("retroctl: cannot reach agent at %s:%s (%s)"
              % (host, port, e), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

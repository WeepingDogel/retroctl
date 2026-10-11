#!/usr/bin/env python3
"""
End-to-end protocol test against the in-process mock agent.  Spawns
mock_agent.py on a port, then drives cli/retroctl.py's RetroCtl class through
every op and asserts the field shapes match what the C agent emits.  This is
the fast way to catch C<->Python framing bugs before touching the box.

Run:  python3 tests/test_protocol.py
"""
import os, sys, time, socket, subprocess, json, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "cli"))
from retroctl import RetroCtl, AgentError  # noqa

PORT = 47321
TOKEN = "mocktok"
HERE = os.path.dirname(os.path.abspath(__file__))


def start_mock():
    env = dict(os.environ, PORT=str(PORT), TOKEN=TOKEN)
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "mock_agent.py")],
                         env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(50):
        time.sleep(0.1)
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.3).close()
            return p
        except OSError:
            continue
    raise SystemExit("mock agent did not come up")


def main():
    p = start_mock()
    ag = RetroCtl("127.0.0.1", PORT, TOKEN, timeout=10)
    fails = []
    try:
        ag.connect()
        print("[ok] handshake agent=%s" % ag.agent_ver)

        # exec (live + batch)
        code, out = ag.exec("ver & echo hi", timeout_ms=5000)
        assert "MOCK-OUT" in out.decode() or code == 0, out
        print("[ok] exec -> %r exit=%d" % (out[:40], code))

        # ps
        procs = ag.ps()
        names = [x["name"] for x in procs]
        assert "aiweb3.exe" in names, names
        print("[ok] ps -> %d procs, has aiweb3.exe" % len(procs))

        # kill by pid and by name
        r = ag.kill(pid=812)
        assert r.get("killed") is True, r
        r = ag.kill(name="aiweb3.exe", all=True)
        assert r.get("matched") == 1, r
        print("[ok] kill pid=%s name(all)=%s" % (r.get("killed"), r.get("killed")))

        # put (local -> remote) round-trip
        with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as tf:
            local = tf.name
            tf.write(b"retroctl-bytes-" * 1000)  # ~14KB
        n = ag.put(local, "C:\\test\\upload.bin")
        assert n == os.path.getsize(local), n
        print("[ok] put -> %d bytes" % n)

        # get (remote -> local + stdout)
        got = ag.get("C:\\test\\upload.bin")
        assert got == b"retroctl-bytes-" * 1000, len(got)
        print("[ok] get round-trip -> %d bytes (matches)" % len(got))

        # ls
        entries = ag.ls("C:\\")
        assert any(e.get("name") == "aiweb.c" for e in entries), entries
        print("[ok] ls -> %d entries" % len(entries))

        # info
        inf = ag.info()
        assert inf.get("hostname") == "MOCKBOX", inf
        print("[ok] info -> host=%s os=%s" % (inf.get("hostname"), inf.get("os_family")))

        print("\nALL PROTOCOL TESTS PASSED")
        return 0
    except AssertionError as e:
        print("\nFAIL:", e)
        return 1
    except AgentError as e:
        print("\nAGENT ERR:", e)
        return 1
    finally:
        try:
            ag.close()
        except Exception:
            pass
        p.terminate()


if __name__ == "__main__":
    sys.exit(main())

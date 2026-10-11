#!/usr/bin/env python3
"""
retroctl bootstrap - one-time install of the agent onto a Windows box.

The agent is a native .exe that must be BUILT on the box (Dev-C++ / old MinGW
lineage, or any MSVC).  This script drives that install over an existing
remote-exec channel.  It is channel-agnostic: you point it at a shell that can
run a single Windows command line and print its output, e.g.:

    RETROCTL_EXEC_CMD="python3 /path/win2k.py"  ./bootstrap.py --box 1.2.3.4

Default RETROCTL_EXEC_CMD assumes a `win2k.py`-style driver: it is invoked as
    <EXEC_CMD> "<windows command line>"
and its stdout is treated as the command output.

What bootstrap does
  1. push agent/retroctl_agent.c  -> C:\\retroctl_agent.c
  2. push agent/wsock.h           -> C:\\wsock.h
  3. compile:  C:\\Dev-C++\\bin\\gcc.exe -O2 -o C:\\retroctl_agent.exe
                C:\\retroctl_agent.c
  4. run:      C:\\retroctl_agent.exe install --port N --token T
  5. verify:   connect to the agent with the CLI, call `info`

You run this ONCE per box.  After that, day-to-day control is all retroctl CLI.
"""

import os
import sys
import base64
import json

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DEFAULT_GCC = r"C:\Dev-C++\bin\gcc.exe"
DEFAULT_AGENT_SRC_DIR = "C:\\"
DEFAULT_EXE = r"C:\retroctl_agent.exe"
DEFAULT_PORT = 46911


def exec_win(cmdline, timeout=120):
    """Run one Windows command line through the configured channel; return output."""
    driver = os.environ.get("RETROCTL_EXEC_CMD", "").strip()
    if not driver:
        raise SystemExit("set RETROCTL_EXEC_CMD to a remote-exec driver "
                         "(e.g. 'python3 /path/win2k.py')")
    import subprocess
    parts = driver.split()
    parts.append(cmdline)
    p = subprocess.run(parts, input=b"exit\r\n", capture_output=True, timeout=timeout)
    return p.stdout.decode("cp1252", "replace")


def push_file(local_path, remote_path, outvar):
    """SMB/SMB-less file push: base64 the file, carry it in the command line,
    decode on the box with a PowerShell/cscript-free one-liner.  On Win2000
    there is no PowerShell, so we stage the b64 to a .b64 and run a tiny C
    decoder is overkill - instead we use certutil if present, else a C helper.
    Simplest portable path: write b64 to a file via echo, then a C build step.

    For bootstrap we keep it dead simple: the box already has the C source in
    hand from the .c push below, so we only need to get the two source files up.
    We use the driver's own file-push when available; otherwise we require the
    operator to have copied agent/ to the box.
    """
    raise NotImplementedError("push_file (see bootstrap notes; use manual copy)")


def main():
    import argparse
    ap = argparse.ArgumentParser(description="one-time install of retroctl_agent")
    ap.add_argument("--box", default="", help="target box (informational; driver knows it)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--token", default="changeme")
    ap.add_argument("--gcc", default=os.environ.get("RETROCTL_GCC", DEFAULT_GCC))
    ap.add_argument("--srcdir", default=DEFAULT_AGENT_SRC_DIR)
    ap.add_argument("--verify-only", action="store_true",
                    help="skip build; just verify the agent is already up")
    args = ap.parse_args()

    # 1. verify the channel
    print("[1/5] checking remote channel ...")
    out = exec_win("ver & echo rctl-ping")
    print("      " + out.strip().replace("\n", " | ")[:120])

    if args.verify_only:
        print("      --verify-only: skipping build; run the CLI to check the agent.")
        return

    # 2-3. build (assumes the two source files are already on the box in srcdir)
    src = os.path.join(args.srcdir, "retroctl_agent.c")
    exe = os.path.join(args.srcdir, "retroctl_agent.exe")
    gsrc = os.path.join(args.srcdir, "wsock.h")
    compile_line = ('%s -O2 -o "%s" "%s" '
                    '& if errorlevel 1 (echo COMPILE_FAILED) else (echo COMPILE_OK)') % (
        args.gcc, exe, src)
    print("[2/5] compiling %s -> %s" % (src, exe))
    out = exec_win(compile_line)
    if "COMPILE_FAILED" in out or "error:" in out:
        print(out)
        raise SystemExit("compile failed - see output above")
    print("      compile ok")

    # 4. install + start the service
    install_line = '"%s" install --port %d --token %s' % (exe, args.port, args.token)
    print("[3/5] installing service on port %d" % args.port)
    out = exec_win(install_line)
    print("      " + out.strip().replace("\n", " | ")[:160])

    # 5. verify via the CLI
    print("[4/5] verifying agent is reachable ...")
    sys.path.insert(0, os.path.join(ROOT, "cli"))
    try:
        from retroctl import RetroCtl
        ag = RetroCtl(args.box or "127.0.0.1", args.port, args.token, timeout=10)
        ag.connect()
        inf = ag.info()
        print("[5/5] agent v%s on %s (%s)" % (
            inf.get("agent_ver"), inf.get("hostname"), inf.get("os_family")))
        print("\nbootstrap complete.  Try:")
        print("  python3 cli/retroctl.py -H %s -P %d -T %s info"
              % (args.box or "127.0.0.1", args.port, args.token))
    except Exception as e:
        print("    verify step: %s" % e, file=sys.stderr)
        print("    (the service may need a moment; re-run 'retroctl info')")


if __name__ == "__main__":
    main()

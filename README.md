# retroctl

A tiny two-part tool for driving Windows boxes from a host terminal —
**including Windows 2000**. One native C **agent** lives on the box, one
dependency-free **Python CLI** lives on the host. No web UI, no install,
no per-operation service juggling.

```
host (macOS/Linux/Win)                     box (Windows, 2000 -> 11)
─────────────────────────────              ───────────────────────────
retroctl.py  ── JSON-lines over TCP ──▶  retroctl_agent.exe (service)
   cli/                                  agent/
```

## Why

Remote control of a legacy Windows box normally means shelling out through
SMB/PsExec for *every* command: spawn a transient service, redirect output to a
shared file, read it back, tear down the service. Works for one-off forensics;
impossible for day-to-day driving. retroctl flips that: install a small
resident agent **once**, then control the box over a plain TCP socket with a
JSON-lines protocol. The psexec channel is only used (if at all) for the
one-time bootstrap.

## What you get

| Command | What it does |
|---------|--------------|
| `retroctl exec "cmd"` | run a command line, stream stdout+stderr back, real exit code |
| `retroctl ps` / `kill` | native process list (toolhelp32) and terminate |
| `retroctl put` / `get` | file transfer, staged + atomic on the destination |
| `retroctl ls` | directory listing with sizes |
| `retroctl info` | agent version, hostname, OS family |
| `retroctl svc …` | manage the agent's own Windows service |
| `retroctl shell` | interactive REPL on a persistent connection |

## Layout

```
agent/retroctl_agent.c   the box-side service (strict C89, runtime LoadLibrary)
agent/wsock.h            Winsock function-pointer table
agent/stub_w32/          host-side #include stub for C89 syntax-checks
cli/retroctl.py          the host-side CLI (Python stdlib only, one file)
bootstrap/bootstrap.py   one-time installer (drives an existing remote channel)
tests/                   in-process mock agent + protocol round-trip test
docs/protocol.md         the wire contract, field by field
examples/                runnable CLI snippets
```

## Install & build

The agent compiles **on the box** with its existing C toolchain — on the
Win2000 box that's Dev-C++'s old MinGW:

```bat
C:\Dev-C++\bin\gcc.exe -O2 -o retroctl_agent.exe retroctl_agent.c
retroctl_agent.exe install --port 46911 --token "somesecret"
```

That creates and autostarts the `retroctl_agent` service. The CLI needs only
Python 3 and no third-party packages:

```
python3 cli/retroctl.py -H <box-ip> -P 46911 -T somesecret info
```

Or drop a config at `~/.retroctl.json`:

```json
{ "host": "192.168.71.34", "port": 46911, "token": "somesecret" }
```

and just run `python3 cli/retroctl.py ps`.

## Try it without a box

```
python3 tests/test_protocol.py     # spins up tests/mock_agent.py, exercises
                                   # every op end-to-end, asserts the shapes
```

This is how the CLI/protocol pair is kept honest between C-side edits.

## Design notes

- **C89 + runtime LoadLibrary**: the target toolchains are old and their import
  tables are incomplete. So the agent links *only* `kernel32` and resolves
  `ws2_32.dll`, `advapi32.dll` and the toolhelp32 process-enumeration exports at
  runtime — the same pattern that proved out on Windows 2000.
- **toolhelp32 offset**: on Win2000 the ANSI `PROCESSENTRY32` is 512 bytes with
  the image name at byte +36; the agent walks a raw buffer, not the struct.
- **exec without shared files**: each exec redirects to a *unique* temp file
  (no more output-file lock contention between concurrent sessions), then
  streams it back base64 and deletes it.
- **No passwords on the wire**: auth is a shared token the agent compares in
  memory at handshake; the connection is LAN-only. Treat the token like a
  session key, not a credential.
- **Mixed languages on purpose**: C where it must be small and resident,
  Python where humans will actually type. That's the whole point of a small,
  auditable tool.

## License

MIT — see [LICENSE](LICENSE).

# retroctl wire protocol

One TCP connection per CLI session.  The client speaks **JSON-lines**: every
message is a single JSON object on its own `\n`-terminated line, UTF-8.  The
first line in **must** be a handshake; after that each line is one command or
one response.

All identifiers (`id`) are opaque strings chosen by the client; the agent
echoes the same `id` back on every frame of that command's response so a
client can de-multiplex if it ever overlaps requests on one socket.

## Handshake

```
client -> {"op":"hello","id":"0","token":"<shared>"}
agent  -> {"id":"0","ok":true,"agent":"1.0.0"}            # success
agent  -> {"id":"0","ok":false,"err":"bad token"}          # wrong token -> close
```

The token is a plain shared secret, not a password; the channel is intended to
be LAN-only and behind a firewall.  A wrong token is answered and the
connection is dropped.

## Command catalogue

Each command is one request line; unless noted, the response is a single
typeless JSON object carrying `id` + `ok` (or `ok:false` + `err`).

### exec
Run `cmd` as `cmd /d /c cmd > <unique.tmp> 2>&1`, stream the captured output
back, then a terminal frame.

```
client -> {"op":"exec","id":"1","cmd":"ver & echo hi","cwd":"C:\\","timeout":60000}
agent  -> {"id":"1","type":"out","b64":"..."}          # 0..N chunks (~32KB raw each)
agent  -> {"id":"1","type":"out","b64":"..."}
agent  -> {"id":"1","type":"done","code":0}
agent  -> {"id":"1","type":"done","code":-1,"timed_out":true}   # on timeout
```

`out` frames carry base64-encoded stdout+stderr.  `done.code` is the child's
exit code (`-1` when it had to be terminated for a timeout).

### ps
```
client -> {"op":"ps","id":"2"}
agent  -> {"id":"2","ok":true,
           "procs":[{"pid":4,"ppid":0,"name":"System Idle Process"}, ...],
           "count":N}
```

### kill
```
client -> {"op":"kill","id":"3","pid":812}                      # by pid
agent  -> {"id":"3","ok":true,"killed":true}

client -> {"op":"kill","id":"3","name":"aiweb3.exe","all":true} # by name
agent  -> {"id":"3","ok":true,"matched":2,"killed":2}
```

### ls
```
client -> {"op":"ls","id":"4","path":"C:\\"}
agent  -> {"id":"4","ok":true,"path":"C:\\",
           "entries":[{"name":"gozweb","dir":true},
                      {"name":"aiweb.c","dir":false,"size":9000}, ...]}
```

### info
```
client -> {"op":"info","id":"5"}
agent  -> {"id":"5","ok":true,"agent_ver":"1.0.0",
           "hostname":"ME0W00F-DA47CB3","os_family":"Windows 2000/NT4"}
```

### put (upload) — three frames, staged + atomic move
```
client -> {"op":"put","id":"6","path":"C:\\Inetpub\\gozweb\\x.html"}
agent  -> {"id":"6","ok":true,"staging":"C:\\...\\x.html~rctl.tmp"}
client -> {"op":"putchunk","id":"6a","path":"C:\\...\\x.html","data":"<b64>"}
agent  -> {"id":"6a","ok":true,"written":65536}
   ... more putchunk frames, each ~64KB raw ...
client -> {"op":"putend","id":"7","path":"C:\\...\\x.html"}
agent  -> {"id":"7","ok":true,"moved":true}
```

The agent appends each chunk to the staging file and, on `putend`, renames it
over the real path with `MoveFileEx(REPLACE)` — so a half-written upload never
leaves a truncated destination.

### get (download) — chunked
```
client -> {"op":"get","id":"8","path":"C:\\...\\aiweb.c"}
agent  -> {"id":"8","type":"getchunk","b64":"..."}   # ~64KB raw each
agent  -> {"id":"8","type":"getchunk","b64":"..."}
agent  -> {"id":"8","type":"getdone","ok":true}
```

## Framing rules
- Every line is one JSON object + `\n`.  No binary; payloads are base64.
- The agent serves one client at a time (single accept loop); `send_line` is
  serialized under a critical section so an in-flight `exec` stream can't
  interleave with another op's reply.
- A client MUST wait for a command's terminal frame (`done` / `getdone` / the
  typeless single-shot object) before sending the next command.

## Why JSON-lines over a richer protocol
It is human-readable, greppable, and the CLI needs only `json` + `socket` +
`base64` from the standard library — which is what makes this forkable and
auditable.  There is no framing magic: if you can `print` a dict, you can
speak retroctl.

---
name: io-agent-mcp
description: |
  Drive an IO_Agent ESP32-S3 dongle over its MCP server: inject keyboard/mouse
  into a remote Windows machine and read back what is on its screen. Use this
  whenever the user wants to operate, watch, or automate a machine that has the
  IO_Agent dongle plugged into it — phrases like "用 IO_Agent 控制那台机器",
  "看这台机器的屏幕", "远程点一下那台电脑", "dongle 注入", "截那台机器的图",
  "the other PC", "remote desktop via the dongle". Covers device discovery over
  mDNS, the five MCP tools, and the injection discipline (single trigger, never
  auto-retry, IME probe before typing). Do NOT use for generic remote control
  that does not go through this dongle, for building/flashing its firmware, or
  for reading files on the local machine.
---

# IO_Agent (MCP server on the dongle)

## The one thing to get right first

The dongle is **plugged into the TARGET machine** (a USB dongle that enumerates
as a HID keyboard+mouse plus a virtual disk). The dongle then **reaches the
network by itself** over WiFi and runs an MCP server on it. **You** (the agent)
sit on some other host and connect *to the dongle*.

```
you ──WiFi──▶ dongle ──USB HID──▶ target machine's keyboard+mouse
                └──virtual disk──▶ target machine reads/writes frames there
```

So "the target machine" and "the host you run on" are two different computers.
The dongle is the only thing that bridges them.

## Inputs to collect

- **Device name** — the mDNS instance name, `ioagent-lab` by default. If the
  user has more than one dongle, ask which one; `scripts/discover.py --name X`
  selects it.
- **Permission to inject** — injecting keystrokes into a live machine is
  destructive. If the request implies *typing into the target machine* and the
  user has not said it is a test/sandbox machine, confirm first. Observation
  (`observe`) is read-only and needs no confirmation.
- **Whether the target machine is unattended** — a locked screen turns injected
  keystrokes into PIN input and can lock the account out.

## Procedure

The two helper scripts live in `scripts/` next to this file (`discover.py`,
`mcp_call.py`); run them with that path. `$EP` below means the endpoint
`discover.py` printed.

### 1. Find the device

```bash
python scripts/discover.py            # -> http://<ip>:8080/mcp
```

The IP is DHCP-assigned and changes; do not cache it across sessions. The host
OS usually cannot resolve `.local` names, so this script speaks mDNS directly
and joins `224.0.0.251` itself.

If it prints nothing: the dongle is unpowered, it is on a different subnet, or
its mDNS responder is down (check `device/info.led` — see step 2).

### 2. Health check before anything else

```bash
python scripts/mcp_call.py --endpoint $EP --info
```

Read these fields and stop early if they are wrong:

| field | meaning | if wrong |
|---|---|---|
| `usb.link_alive` | target machine is enumerating the dongle | `false` ⇒ no injection is possible at all. Stop and report. |
| `usb.suspended` | host suspended the USB link | `true` ⇒ wake it; injection silently does nothing |
| `hid.endpoint_ready` | the HID endpoint is being polled | `false` ⇒ the target machine is not reading input |
| `busy` | an action is running right now | `true` ⇒ wait and re-poll; do **not** queue work |
| `led` | 0 ready / 1 booting / 2 net down / 3 acting / 4 fault | 4 ⇒ the last injection self-test failed, the "hand" is broken |

### 3. See the screen (read-only)

The device never decodes images — it moves bytes. Frames are produced on the
target machine by a **watcher** script that writes them to the dongle's virtual
disk.

```bash
python scripts/mcp_call.py --endpoint $EP --tool watcher/status
python scripts/mcp_call.py --endpoint $EP --tool observe --args '{"target":"preview"}'
```

`observe` returns a `url`; fetch it with

```bash
python scripts/mcp_call.py --fetch <url> --out frame.jpg
```

`target` values: `preview` (`SNAP_P.JPG`), `full` (`SNAP_A.JPG`), `roi`
(`SNAP_R.JPG`), `diff` (returns `DIFF.IDX` as **text**, not an image), or any
8.3 filename on the volume (e.g. `INJ.TXT`, `CAP.TXT`) to read it back.

If `watcher/status` says `running:false`, the watcher is not up. Starting it
requires **injecting a command into the target machine** — that is a
destructive step, so get the user's go-ahead first, then follow
`references/injection-playbook.md`.

### 4. Act on the target machine (destructive)

**Gate — do not skip this.** Every `act` call, including `macro=selftest` and
`macro=ime_toggle`, **types real keystrokes into the target machine**. The
self-test is not a read-only probe: it opens a Run box and executes a command
there. If the user has not authorised injecting into that machine, **send no
`act` at all** — not even a "harmless" one — and report what you can do
read-only instead. When in doubt, ask.

Read `references/injection-playbook.md` before your first injection. The short
version:

- **One action per call. The device never retries.** If you are not sure it
  landed, ask the user before sending it again — repeated keystrokes into a
  live machine are how accounts get locked out.
- **Probe the input mode before typing.** On a Chinese-locale Windows the
  injected text comes out as garbage. Send `act macro=selftest` and check
  whether `INJ.TXT` appears; only type after the probe passes.
- **Long actions are asynchronous.** `macro=*` and `type` of more than 16
  characters come back as `{"accepted":true,"busy":true}` and finish in the
  background. Poll `device/info` until `busy` is `false`, then read the marker
  file to learn the outcome.
- **Judge success by files, not by the reply.** The device cannot see the
  target machine's screen. `INJ.TXT` (written by the injected command) and
  `CAP.TXT` (written by the watcher) are the objective evidence.

### 5. Report

State what you observed, not what you sent. If an injection's outcome is
unknown, say so and say which marker file you checked — "the call was accepted"
is not the same as "it worked".

## Output contract

Tool results arrive as MCP `content` blocks, so the real payload is a JSON
string inside `result.content[0].text` — **parse it a second time**:

```json
{"result": {"content": [{"type": "text", "text": "{\"device\":\"ioagent-lab\", ...}"}]}}
```

`scripts/mcp_call.py` already unwraps and re-prints that, so prefer it over
hand-rolling JSON-RPC.

Do not use the MCP `isError` flag to decide whether something worked — only
`act` and `deploy_payload` set it. `observe` and `watcher/status` report
failure as `ok:false` inside the payload, so always read that field.

Everything else (schemas, response fields, error codes, request rules) is in
`references/mcp-contract.md`.

## Failure handling

| symptom | cause | action |
|---|---|---|
| `discover.py` finds nothing | dongle off / other subnet / mDNS down | check power, then `--info` on a known IP if you have one |
| `403` | `Host`/`Origin` not bound to the device | use the exact IP or `<name>.local`; do not send an `Origin` header |
| `415` | `Content-Type` not `application/json` | the scripts always set it; only hand-written curl needs the header |
| `413` | request body over 128 KB | split the payload; use `deploy_payload` for bulk instead |
| `400` | `MCP-Protocol-Version` not one of the supported versions | send `2025-06-18` |
| `{"busy":true}` | another action is running | wait, re-poll `--info`; do not queue |
| `led:4` | last injection self-test failed | the target machine likely blocked or the USB link dropped — re-check `--info` |
| `observe` returns `ok:false, note:"file not on the volume"` | watcher not running, or the device rebooted (RAM disk is rebuilt on every boot) | `watcher/status`; if the device rebooted, the watcher must be started again |
| injected text comes out as Chinese/garbled | the target's IME was in Chinese mode | run the selftest probe; if it fails, send `macro=ime_toggle` and probe again |

## Examples

**Example 1 — look at the other machine's screen**

```
$ python scripts/discover.py
http://172.17.21.225:8080/mcp
$ python scripts/mcp_call.py --endpoint http://172.17.21.225:8080/mcp --info
  ... "usb":{"link_alive":true}, "busy":false, "led":0 ...
$ python scripts/mcp_call.py --endpoint http://172.17.21.225:8080/mcp --tool watcher/status
  ... {"running":true,"cap_bytes":128} ...
$ python scripts/mcp_call.py --endpoint http://172.17.21.225:8080/mcp --tool observe --args '{"target":"preview"}'
  ... {"ok":true,"bytes":20452,"url":"http://172.17.21.225:8080/blob/b0"} ...
$ python scripts/mcp_call.py --fetch http://172.17.21.225:8080/blob/b0 --out frame.jpg
saved 20452 bytes -> frame.jpg
```

**Example 2 — send one keystroke and verify it landed**

```
$ python scripts/mcp_call.py --endpoint http://172.17.21.225:8080/mcp \
    --tool act --args '{"action":"tap","key":"ENTER"}'
  ... {"ok":true,"action":{"key":"ENTER"}} ...
$ python scripts/mcp_call.py --endpoint http://172.17.21.225:8080/mcp --info
  ... "busy":false ...
```

A short action returns the real result inline. `ok:true` here means the device
emitted the keystroke — it does **not** prove the target machine reacted; for
that, check the target's own evidence (marker file or a fresh frame).

## Windows (win32) platform notes

PowerShell hands arguments to native programs by re-parsing them, and it
**strips the inner double quotes of a JSON argument**:

```powershell
python mcp_call.py --args '{"target":"preview"}'   # arrives as {target:preview}  <- broken
python mcp_call.py --args '{\"target\":\"preview\"}' # arrives intact
python mcp_call.py --% --args "{\"target\":\"preview\"}"  # stop-parsing, works
```

Reliable forms, in order of preference:

1. Put the JSON in a file and pass `@file`:
   ```powershell
   '{"target":"preview"}' | Set-Content -Encoding UTF8 args.json
   python mcp_call.py --endpoint $EP --tool observe --args '@args.json'
   ```
2. Escape the quotes as `\"` (shown above).
3. Use `--%` before the script name.

Also on Windows: the dongle is on the LAN, so **any** configured HTTP proxy
(`http_proxy`, Clash, corporate proxy) will break the call. `mcp_call.py`
bypasses proxies on purpose — if you write your own HTTP code, do the same
(`ProxyHandler({})` in Python, or `--noproxy '*'` in curl).

The scripts need Python 3 (standard library only). Run them with `python` or
`py` on Windows, `python3` on macOS/Linux.

# IO_Agent MCP contract

Everything here is what the **device** actually implements. If a field is not
listed here, do not assume it exists.

## Transport

| endpoint | method | purpose |
|---|---|---|
| `/mcp` | POST | JSON-RPC 2.0 (MCP Streamable HTTP) |
| `/ping` | GET | liveness + identity, plain JSON (not JSON-RPC) |
| `/blob/b0` … `/blob/b3` | GET | raw bytes of a blob slot |

Port 8080. The device is the **server**; the agent is the client.

## Request rules (violating them gives a 4xx, not a silent no-op)

| rule | value | failure |
|---|---|---|
| `Content-Type` | `application/json` (a `charset` parameter is fine) | `415` |
| body size | ≤ 128 KB | `413` |
| `Host` | the device's own IP, device name, or `<name>.local` | `403` |
| `Origin` | absent, or one of the device's own | `403` |
| `MCP-Protocol-Version` | `2024-11-05`, `2025-03-26`, `2025-06-18` | `400` |
| `Mcp-Session-Id` | **do not send**; the device never sends one | — |

The device is **stateless**: it does not issue a session id and does not want
one. A notification (`notifications/initialized`) is answered with `202` and an
empty body.

Before rejecting a request the device **drains the request body** (bounded),
so the client reliably reads the status code instead of a connection reset.

## Response envelope

JSON-RPC responses are normal MCP envelopes. A tool result is a JSON **string**
inside a text content block — parse it twice:

```json
{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"{\"ok\":true}"}]}}
```

Tool errors are reported inside the tool result (`ok:false` plus `error`), not
as JSON-RPC `error` objects.

**`isError` is only set on `act` and `deploy_payload`.** `observe` and
`watcher/status` never set it — their failures appear only as `ok:false` inside
the payload. So do not treat "no `isError`" as "the call succeeded"; always
read the inner `ok`.

## Handshake

`initialize` → `{"protocolVersion": "..."}`. The device supports
`2024-11-05`, `2025-03-26`, `2025-06-18`:

- request a supported version → the device echoes it back;
- request anything else → the device answers with `2025-06-18` and the client
  decides whether to continue.

After `initialize`, send `MCP-Protocol-Version: 2025-06-18` on every request.

## Tools

### `device/info` — no arguments

```json
{"device":"ioagent-lab","fw":"m2-remote","ip":"172.17.21.225",
 "wifi_connected":true,"uptime_s":54366,"sectors":8192,
 "usb":{"mounted":true,"suspended":false,"host_configured":true,"link_alive":true},
 "hid":{"endpoint_ready":true,"layout_ok":true},
 "busy":false,"led":0,
 "slots":[{"name":"WATCH.PS1","kind":"slot","capacity":32768,"len":20452},
          {"name":"AGENT.SH","kind":"slot","capacity":32768,"len":134},
          {"name":"AGENT.EXE","kind":"slot","capacity":65536,"len":0},
          {"name":"PAYLOAD.BIN","kind":"inbox","capacity":131072,"len":0}],
 "payload_seq":3}
```

- `usb.link_alive` — the target machine is enumerating the dongle. `false`
  means injection cannot work at all.
- `usb.suspended` — the target suspended the USB link.
- `hid.endpoint_ready` — the HID endpoint is being polled (the target machine
  is reading input).
- `busy` — an action is running. Read-only tools keep working while busy.
- `led` — device status code: `0` ready, `1` booting, `2` network not ready,
  `3` action in progress, `4` fault (last injection self-test failed).
  Priority: 3 > 4 > 1 > 2 > 0.

Slot table (fixed): `0 WATCH.PS1` 32 KB, `1 AGENT.SH` 32 KB, `2 AGENT.EXE`
64 KB, `3 PAYLOAD.BIN` 128 KB (inbox — the host writes it over USB, it is not
writable through `deploy_payload`).

### `act` — inject one action

| argument | type | notes |
|---|---|---|
| `action` | `type` \| `tap` \| `hotkey` \| `move` \| `click` \| `scroll` \| `macro` | required |
| `text` | string | `action=type` |
| `key` | string | `action=tap`/`hotkey`; a single character **or** a named key |
| `modifiers` | array | `ctrl` / `shift` / `alt` / `win` |
| `dx`, `dy` | number | `action=move`; `dy` for `scroll` |
| `button` | `left`\|`right`\|`middle` | `action=click` |
| `macro` | `selftest` \| `launch_watcher` \| `focus_reset` \| `ime_toggle` | `action=macro` |
| `ime` | `toggle` | with `macro=launch_watcher`: press Shift once before typing |
| `hold_ms` | number | key hold time; default 60 (single key) / 600 (chord) |
| `per_char_ms` | number | per-character hold in `type`; default 60 |

Named keys (case-insensitive): `SHIFT` `CTRL` `ALT` `WIN` `ENTER` `ESC`
`SPACE` `TAB` `BACKSPACE` `DELETE`. Anything else is treated as a single
character (ASCII `0x20`–`0x7E`).

**Sync vs async.** Short actions (`tap` / `hotkey` / `move` / `click` / `scroll`
/ `type` of ≤16 characters) run inline and the result carries the real outcome:

```json
{"ok":true,"action":{"key":"ENTER"}}
```

`macro=*` and `type` longer than 16 characters are handed to a background
worker; the reply is immediate and the outcome is **not** in it:

```json
{"ok":true,"accepted":true,"busy":true,
 "note":"accepted and running in background; poll device/info (busy=false) then read the marker file (INJ.TXT / CAP.TXT) to judge the outcome"}
```

Only one action runs at a time. A second action request while one is running
comes back as `{"ok":false,"busy":true,"error":"device busy ..."}` — it is
**not queued**. Poll `device/info` until `busy` is `false`.

The device never retries an action by itself.

### `observe` — read a file off the target's virtual disk

`target`: `preview` (`SNAP_P.JPG`), `full` (`SNAP_A.JPG`), `roi`
(`SNAP_R.JPG`), `diff` (`DIFF.IDX`, returned as **text**), or any 8.3 filename
(`INJ.TXT`, `CAP.TXT`, `WATCH.PS1`, …).

Success:

```json
{"ok":true,"file":"SNAP_P.JPG","tag":"preview","bytes":20452,"seq":7,
 "url":"http://172.17.21.225:8080/blob/b0"}
```

Failure: `{"ok":false,"file":"...","note":"file not on the volume (is the watcher running on the host?)"}`.

**Blob lifetime.** There are 4 blob slots (256 KB each) used as a **ring**: every
`observe` takes the next slot and overwrites the oldest. Fetch the URL before
making another `observe` call, or you will get a different frame. The URL also
embeds the device's IP, so it dies with an IP change.

The device only moves bytes. It never decodes, compares, or crops.

### `watcher/status` — no arguments

```json
{"running":true,"cap_bytes":128,"cap_head":"..."}
```

or `{"running":false,"note":"CAP.TXT not found -- the watcher is not running on the host"}`.

`CAP.TXT` is written **once** when the watcher starts, so it is a liveness
signal that goes stale: after the dongle reboots, the RAM disk is rebuilt and
`CAP.TXT` is gone even though the watcher on the target machine may still be
running.

### `deploy_payload` — write a slot into flash

| argument | type | notes |
|---|---|---|
| `slot` | number | 0 `WATCH.PS1`, 1 `AGENT.SH`, 2 `AGENT.EXE` |
| `text` | string | plain text payload |
| `base64` | string | binary payload, as an alternative to `text` |

```json
{"ok":true,"slot":"WATCH.PS1","len":20452,"payload_seq":4,
 "note":"stored in flash only -- the RAM disk copy is written at next boot"}
```

The content lands in the flash A/B bank and only takes effect **at the next
boot** — the running RAM disk copy is not touched. Writing is atomic; if the
device is busy with another action the call returns `busy`.

The flash bank is the single source of truth: the RAM disk slot is a copy
laid down at boot. Do not expect a running script to change after a deploy.

## `/ping`

Plain JSON, not JSON-RPC:

```json
{"ok":true,"device":"ioagent-lab","ip":"172.17.21.225","uptime_s":54366,"mcp":"2025-06-18"}
```

## Error codes

| HTTP | meaning | first thing to check |
|---|---|---|
| 400 | unsupported `MCP-Protocol-Version`, or malformed JSON | the header value |
| 403 | `Host`/`Origin` not bound to the device | use the exact IP or `<name>.local` |
| 413 | body over 128 KB | split it; use `deploy_payload` for bulk |
| 415 | `Content-Type` not `application/json` | set the header |
| 408 | the client stopped sending the body | the connection died mid-request |
| 500 | out of memory / internal error | retry once, then report |

## What the device deliberately does NOT do

- No `wifi set` / reboot / OTA tool. Any action that would drop the very
  connection the call came in on is refused by design — reconfigure the dongle
  over its USB disk (`DEV.CFG`), not over MCP.
- No job id, no polling endpoint. The marker files on the target machine are
  the state.
- No image processing. Cropping and frame diffing belong on the agent side.

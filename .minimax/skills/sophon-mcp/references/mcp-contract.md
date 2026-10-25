# Sophon MCP contract

Everything here is what the **device** actually implements. If a field is not
listed here, do not assume it exists.

## Transport

| endpoint | method | purpose |
|---|---|---|
| `/mcp` | POST | JSON-RPC 2.0 (MCP Streamable HTTP) |
| `/mcp` | GET | `405` — the device has no SSE stream, by design |
| `/ping` | GET | liveness + identity, plain JSON (not JSON-RPC) |
| `/files/<name>` | GET | raw bytes of one file on the shared disk |

Port 8080. The device is the **server**; the agent is the client.

`/files/<name>` takes the **8.3 file name** — the same name `ls` reports and
`read` takes. There is no blob slot and no slot id: the file name *is* the
address.

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

A tool result carries three things:

| field | what it is |
|---|---|
| `structuredContent` | the machine-readable payload — **read this** |
| `content[]` | content blocks: a `text` block with the same JSON, plus an `image` block for JPEGs |
| `isError` | `true` when the tool itself failed |

```json
{"jsonrpc":"2.0","id":1,
 "result":{"structuredContent":{"ok":true,"file":"SNAP_P.JPG","bytes":20452},
           "content":[{"type":"text","text":"{\"ok\":true,...}"},
                       {"type":"image","data":"<base64>","mimeType":"image/jpeg"}],
           "isError":false}}
```

**`isError` is set on every tool now** — a failed call is `isError:true` with
`ok:false` inside the payload. Check both.

Protocol-level problems are **not** tool results: an unknown tool name comes
back as a top-level JSON-RPC `error` object with code `-32602`. An unsupported
method is `-32601`.

Every tool declares an `outputSchema`; it is `object` with a required `ok`
boolean.

## Handshake

`initialize` → `{"protocolVersion": "..."}`. The device supports
`2024-11-05`, `2025-03-26`, `2025-06-18`:

- request a supported version → the device echoes it back;
- request anything else → the device answers with `2025-06-18` and the client
  decides whether to continue.

After `initialize`, send `MCP-Protocol-Version: 2025-06-18` on every request.

## Tools

### `status` — no arguments

Identity, link state, slot table, and whether the target's watcher is running.

```json
{"ok":true,"device":"sophon-lab","fw":"m2-remote","ip":"172.17.21.225",
 "wifi_connected":true,"uptime_s":54366,"sectors":8192,
 "usb":{"mounted":true,"suspended":false,"host_configured":true,"link_alive":true},
 "hid":{"endpoint_ready":true,"layout_ok":true},
 "busy":false,"led":0,
 "slots":[{"name":"WATCH.PS1","kind":"slot","capacity":32768,"len":20452},
          {"name":"AGENT.SH","kind":"slot","capacity":32768,"len":134},
          {"name":"AGENT.EXE","kind":"slot","capacity":65536,"len":0},
          {"name":"PAYLOAD.BIN","kind":"inbox","capacity":131072,"len":0}],
 "payload_seq":3,
 "watcher":{"running":true,"cap_mtime":815226168},
 "log":{"port":8090,"active":true,"open":true,"allowed":1,
        "rx":12,"dropped":0,"tx":12,"bytes":4096,"hook":530},
 "ota":{"state":"idle","received":0,"total":-1,"boot":"ota_0","next":"ota_1"},
 "app":{"version":"348dd80","idf":"v5.5.6","build":"Oct 25 2026"}}
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
- `watcher.running` — the target's watcher is up. The judgement is "is `CAP.TXT`
  on the volume", so it goes stale after a dongle reboot (the RAM disk is
  rebuilt on every boot). `cap_mtime` is present when it is running.
- `log` — the network log channel. `active` means an allowlist is configured
  (no allowlist ⇒ the channel is off and `allowed` is 0). `open` means the UDP
  socket is up. `dropped` counts requests dropped because the **source IP was
  not in the allowlist** — it rising means someone is asking who is not
  allowed to. `rx`/`tx` are packet counts, `bytes` the bytes sent out.
- `ota` — the firmware update channel. `state` is `idle` / `downloading` /
  `staged` / `rebooting` / `failed`. `boot` is the slot the device is running
  from, `next` the slot the next update writes to; after a successful flash
  `boot` flips to the other slot.
- `app` — the running firmware: `version` (git describe), `idf`, `build`
  (compile timestamp).

Slot table (fixed): `0 WATCH.PS1` 32 KB, `1 AGENT.SH` 32 KB, `2 AGENT.EXE`
64 KB, `3 PAYLOAD.BIN` 128 KB (inbox — the host writes it over USB, it is not
writable through `update`).

### `ls` — no arguments

Everything on the shared disk.

```json
{"ok":true,"count":4,
 "files":[{"name":"WATCH.PS1","size":20452,"mtime":815226168,"frame":false},
          {"name":"SNAP_P.JPG","size":20452,"mtime":815226999,"frame":true}]}
```

- `name` — the 8.3 name. Use it verbatim in `read` and in `/files/<name>`.
- `size` — bytes.
- `mtime` — last-write time of the file, in **seconds**. Read it as a *relative
  stamp*, not an absolute clock: FAT16 stores **local** wall-clock time with no
  timezone, and the device interprets it as UTC, so the absolute value differs
  from the host's UTC epoch by the host's timezone offset. FAT's granularity is
  **2 seconds**. `0` means the directory entry carries no timestamp. What you
  can rely on: **which file is newer**, and **whether a file was rewritten**.
- `frame` — `true` for the watcher's frame files (`SNAP_P.JPG`, `SNAP_A.JPG`,
  `SNAP_R.JPG`).

### `read` — one file by name

| argument | type | notes |
|---|---|---|
| `name` | string | 8.3 file name, e.g. `INJ.TXT`, `CAP.TXT`, `SNAP_P.JPG` — required |

Success (text):

```json
{"ok":true,"file":"INJ.TXT","size":128,"mtime":815226168,"bytes":128,
 "text":"..."}
```

Success (JPEG): same fields, plus `mime:"image/jpeg"` and an **image content
block** with the base64 body. `text` is absent.

Too large to inline (> 64 KB):

```json
{"ok":true,"file":"SNAP_A.JPG","size":307200,"mtime":815226168,
 "url":"http://172.17.21.225:8080/files/SNAP_A.JPG",
 "note":"too large to inline; fetch the url"}
```

Failure: `{"ok":false,"file":"...","error":"no such file on the volume"}` with
`isError:true`.

### `capture` — no arguments

The **newest** frame on the disk, picked by last-write time.

```json
{"ok":true,"file":"SNAP_P.JPG","size":20452,"mtime":815226999,"bytes":20452}
```

plus an image content block with the JPEG. If the frame is too large to inline,
the result carries a `url` instead, like `read`.

Failure: `{"ok":false,"error":"no frame on the volume (is the watcher running on the target?)"}`
with `isError:true`.

Frames are the watcher's output: `SNAP_P.JPG` (preview), `SNAP_A.JPG` (full),
`SNAP_R.JPG` (roi). "Newest" means the largest `mtime`, so capture twice in a
row to see whether the target's screen changed.

The device only moves bytes. It never decodes, compares, or crops.

### `input` — inject one action

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
 "note":"accepted and running in background; poll status (busy=false) then read the marker file (INJ.TXT / CAP.TXT) to judge the outcome"}
```

Only one action runs at a time. A second action request while one is running
comes back as `{"ok":false,"busy":true,"error":"device busy ..."}` with
`isError:true` — it is **not queued**. Poll `status` until `busy` is `false`.

The device never retries an action by itself.

### `update` — write a slot into flash

| argument | type | notes |
|---|---|---|
| `slot` | number | 0 `WATCH.PS1`, 1 `AGENT.SH`, 2 `AGENT.EXE` — required |
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
laid down at boot. Do not expect a running script to change after an update.

### `flash` — replace the dongle's own firmware (OTA)

| argument | type | notes |
|---|---|---|
| `url` | string | `http(s)://` URL of the firmware image — required |

```json
{"ok":true,"accepted":true,"url":"http://192.168.1.5:8000/sophon.bin","note":"..."}
```

The device **downloads** the image (it does not accept an upload — an upload
would occupy the device's only HTTP task for the whole transfer and freeze
MCP). It writes the image into the other OTA slot, verifies it (descriptor +
SHA256), switches the boot slot and reboots.

- The reply arrives **immediately**; the reboot drops the connection a few
  seconds later. That drop is expected, not a failure.
- A bad image is rejected and the current firmware keeps running — the boot
  slot is only switched after verification passes.
- To confirm the new firmware is really the one running, check
  `status.ota.boot`: it must have flipped to the other slot. The `version`
  string does **not** change when you reflash the same build, so it is not
  a sufficient check on its own.
- Only relevant for a single-Type-C dongle (the only USB port is occupied by
  the HID+MSC link to the target machine). With a dev board that still has a
  serial port, flashing over USB is simpler.

## `/ping`

Plain JSON, not JSON-RPC:

```json
{"ok":true,"device":"sophon-lab","ip":"172.17.21.225","uptime_s":54366,"mcp":"2025-06-18"}
```

## Error codes

| HTTP | meaning | first thing to check |
|---|---|---|
| 400 | unsupported `MCP-Protocol-Version`, or malformed JSON | the header value |
| 403 | `Host`/`Origin` not bound to the device | use the exact IP or `<name>.local` |
| 405 | `GET /mcp` — by design, there is no SSE stream | POST to `/mcp` |
| 413 | body over 128 KB | split it; use `update` for bulk |
| 415 | `Content-Type` not `application/json` | set the header |
| 408 | the client stopped sending the body | the connection died mid-request |
| 500 | out of memory / internal error | retry once, then report |

## What the device deliberately does NOT do

- No `wifi set` / reboot tool. Any action that would drop the very connection
  the call came in on is refused by design — reconfigure the dongle over its
  USB disk (`DEV.CFG`), not over MCP. (`flash` is the one exception: it
  answers first and reboots afterwards, so the reply does reach the caller.)
- No job id, no polling endpoint. The marker files on the target machine are
  the state.
- No image processing. Cropping and frame diffing belong on the agent side.
- No file writing to the shared disk. The host writes the disk over USB; the
  device only reads it. `update` writes to flash, not to the volume.

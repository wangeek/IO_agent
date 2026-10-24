# Injection playbook (the destructive path)

Everything on this page types into a **live machine**. Read it before the first
injection.

## Ground rules

0. **Every `input` call types into a live machine — including `macro=selftest`
   and `macro=ime_toggle`.** The self-test is *not* a read-only probe: it opens
   a Run box on the target and executes a command there. "It's only a probe" is
   not a reason to send one without authorisation. If authorisation is
   missing, send no `input` at all and report the read-only options instead.
1. **One action per call, and the device never retries.** If you are unsure it
   landed, ask the user before sending it again. Repeated keystrokes into a
   live machine are how accounts get locked out.
2. **A locked screen turns keystrokes into PIN input.** On a locked target,
   injected characters go into the unlock box, and enough failures trigger the
   account's brute-force lockout. Check with the user whether the machine is
   unlocked before injecting.
3. **The device cannot see the target's screen.** It only knows whether bytes
   went out and whether a marker file came back. Judge the outcome by those
   files, never by "the call returned ok".
4. **Do not send `input` while `status.busy` is true.** The device would
   answer `busy` and drop the request; it does not queue.

## The exact sequence the device performs

`macro=launch_watcher` and the self-test share this skeleton:

```
Win+M        minimise everything, so nothing covers the desktop
  500ms
Win+R        open a NEW Run box
  500ms
Ctrl+A       select the Run box's prefilled history (see below)
  500ms
[Shift]      ONLY with ime:"toggle" -- one bare Shift tap, to switch IME
  500ms
type text    at 60 ms per character
  400ms
Enter
```

`macro=focus_reset` is just the first three steps. `macro=ime_toggle` is
`focus_reset` plus one bare Shift tap.

Two details that are easy to get wrong:

- **The Run box prefills your previous command and does not select it.** Without
  `Ctrl+A` your text is *appended* to the old command (`regeditcmd /c ...`),
  the command fails, and it looks like "the injection did nothing".
- **A fresh Run box starts in the host's default input mode** (Chinese on a
  Chinese-locale Windows). Shift has to be pressed *after* the box opens and
  *before* the typing, because the input mode belongs to the focused context.

## Why you must probe before typing

Microsoft Pinyin keeps the Chinese/English state **per input-box context**,
and it is remembered by the IME instance, not reset by opening a new box.
Consequences, all measured:

- `Ctrl+Space` and `Shift` are **the same switch** in Microsoft Pinyin.
  Sending both = the switch toggles twice = nothing changes. There is no
  "belt and braces" version.
- A **blind** Shift is a coin flip: repeated runs alternated success/failure.
  So do not guess the mode.
- The self-test command contains ASCII punctuation (`% \ > @ ( )`). In Chinese
  mode it is typed as mixed Chinese/ASCII and therefore does not run, so
  `INJ.TXT` never appears. **`INJ.TXT` appearing therefore proves the mode was
  English/ASCII.**

## The procedure

### 1. Probe

```bash
python scripts/mcp_call.py --endpoint $EP --tool input --args '{"action":"macro","macro":"selftest"}'
```

This is a long action: the reply is `{"ok":true,"accepted":true,"busy":true}`.
Wait for it to finish:

```bash
python scripts/mcp_call.py --endpoint $EP --status      # wait until busy == false
python scripts/mcp_call.py --endpoint $EP --tool read --args '{"name":"INJ.TXT"}'
```

- `read` returns `ok:true` ⇒ `INJ.TXT` exists ⇒ the mode was English.
  **Proceed.**
- `read` returns `isError:true` / `ok:false` ⇒ the mode was Chinese (or the
  injection did not reach the machine at all).

### 2. If the probe failed, switch the mode once — then probe again

```bash
python scripts/mcp_call.py --endpoint $EP --tool input --args '{"action":"macro","macro":"ime_toggle"}'
```

`ime_toggle` only toggles the input mode; it types nothing, so it is safe even
on a locked screen. **Then repeat step 1.** If the second probe also fails, the
problem is not the IME — the injection is not reaching the machine. Check
`status`: `usb.link_alive` must be true and `hid.endpoint_ready` must be true.

### 3. Do the real thing

```bash
python scripts/mcp_call.py --endpoint $EP --tool input \
  --args '{"action":"macro","macro":"launch_watcher"}'
```

If the probe had **failed** (Chinese mode), add the Shift step:

```bash
python scripts/mcp_call.py --endpoint $EP --tool input \
  --args '{"action":"macro","macro":"launch_watcher","ime":"toggle"}'
```

`ime:"toggle"` presses Shift once, on the freshly opened Run box, right before
typing. It is a blind toggle — only use it when a probe has just told you the
mode was Chinese.

### 4. Verify

```bash
python scripts/mcp_call.py --endpoint $EP --status      # busy == false
python scripts/mcp_call.py --endpoint $EP --tool ls
```

`status` reports `watcher.running:true` when `CAP.TXT` is on the volume. The
watcher writes `CAP.TXT` once when it starts, so it can be stale after the
dongle rebooted — a `running:false` right after a reboot does not prove the
watcher died.

## Typing arbitrary text

`action=type` with `text` types the string verbatim at the target's current
focus. It does **not** open a Run box, does not press Enter, and does not
select anything.

- ≤ 16 characters: runs inline, the result says whether it was emitted.
- \> 16 characters: runs in the background (`accepted:true, busy:true`); poll
  `status` for `busy:false`.
- The text goes through the target's IME. **ASCII text typed in Chinese mode
  comes out as garbage** — that is what the probe is for. If you must type
  ASCII and cannot probe, there is no reliable shortcut.

## Key timing (already calibrated in the device — do not shorten it)

| what | minimum | why |
|---|---|---|
| modifier + key (chord) | 600 ms | 30 ms is dropped by Windows outright, so `Win+R` simply never opens the Run box |
| single key / one character | 60 ms | at 8 ms Microsoft Pinyin garbles the input (`ni hao ` became 49 Chinese characters) |

`hold_ms` overrides the chord/single-key default; `per_char_ms` overrides the
per-character delay in `type`.

## Mouse

- `{"action":"move","dx":10,"dy":-5}` — relative movement.
- `{"action":"click","button":"left"}` — `left` / `right` / `middle`.
- `{"action":"scroll","dy":-3}` — vertical wheel (negative scrolls up).

Mouse actions do not touch the IME and need no probe. They still count as
actions: one at a time, and never while `busy`.

## What not to do

- Do not re-send an injection because the reply looked unpromising. The device
  does not retry for exactly this reason; a retry is *your* decision and it
  hits a live machine.
- Do not inject into a machine the user did not authorise, and do not infer
  authorisation from "it is my other PC".
- Do not assume the watcher is running because `status.watcher.running` says
  `false` right after a dongle reboot — the RAM disk is rebuilt on
  every boot.
